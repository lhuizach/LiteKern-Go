/* LiteKern Go: lkx-gld, a small 3D renderer for apps (Traffic Racer first). Runs inside the Alpine sandbox
 * (musl, Mesa) on the phone's GPU (the Adreno 306 through freedreno), built there by build-gld.sh.
 *
 * Apps draw with the CPU and have no GL. So the app sends this program its SCENE (a few hundred bytes a frame:
 * where the camera is, the cars, the bend of the road) over a datagram socket, and this program draws it with
 * OpenGL ES 3 and writes the picture into a shared file the app reads, like lkx-webd does for the browser:
 *
 *   /dev/shm/lkxgl.sock    the app sends a struct gl_scene here (one datagram a frame)
 *   /dev/shm/lkxgl.frame   a header, then BGRA rows, top row first; `seq` goes up when a frame is complete
 *
 *   lkx-gld --serve [--size WxH] [--msaa N]      the real thing, started by the app
 *   lkx-gld --demo OUT.ppm [--size WxH]          one picture of a made-up scene
 *   lkx-gld --bench [--size WxH] [--msaa N]      frames a second with a moving scene, picture read back
 *
 * The world is the game's own (apps/trafficracer): units along and across a four-lane road, 220 units a metre,
 * lanes 500 wide, x to the right, z ahead. Cars are low-poly, built here from boxes. Everything is lit by one
 * sun and fades into haze with distance; the road, its lines and the sand stripes are drawn by a shader. */
#define _GNU_SOURCE
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <errno.h>
#include <fcntl.h>
#include <gbm.h>
#include <math.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#define FRAME_FILE "/dev/shm/lkxgl.frame"
#define SOCK_FILE "/dev/shm/lkxgl.sock"
#define FRAME_MAGIC 0x4c47474cu             /* "LGGL" */
#define SCENE_MAGIC 0x4e435347u             /* "GSCN" */
#define HEADER_BYTES 64
#define MAX_CARS 32

struct frame_header { uint32_t magic, seq, w, h, stride, scene_seq; int32_t player_x, player_y; uint32_t slot; };
/* player_x, player_y: the player's car on the picture, in its pixels. Two pictures follow the header: `slot` is
 * the finished one (the other is being drawn). */

/* The scene, in whole numbers (the apps have no floating point): units as the game has them, angles in 1/1000 rad. */
struct gl_car { int32_t x, z, kind; uint32_t col; int32_t yaw, roll, pad[2]; };
struct gl_scene {
    uint32_t magic, seq;
    int32_t xp, zp, scroll, curve;          /* the player's sideways place and distance, how far the road has scrolled,
                                             * the bend (-256..256) */
    int32_t speed, shake_x, shake_y, time_ms;
    int32_t ncars, player_vis;
    uint32_t player_col;
    int32_t player_lean;                    /* -40..40, as the game has it */
    struct gl_car cars[MAX_CARS];
};
struct scene_f { float xp, zp, scroll, curve, speed, shake_x, shake_y; int ncars, player_vis; uint32_t player_col; float player_lean; const struct gl_car *cars; };

#define ROAD_HW 1000.0f
#define SLOT 1100
#define ZFAR 70000.0f
static const float car_w[4] = { 410, 420, 430, 460 };
static const float car_l[4] = { 570 * 1.3f, 640 * 1.3f, 700 * 1.3f, 960 * 1.3f };   /* drawn longer than they collide */

static int W = 720, H = 1224, samples = 4;
static unsigned char *shm;
static struct frame_header *fh;

/* ---- tiny maths ---------------------------------------------------------------------------------- */

static void normalize(float *v)
{
    float l = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (l > 0) { v[0] /= l; v[1] /= l; v[2] /= l; }
}
static void cross(const float *a, const float *b, float *o)
{
    o[0] = a[1] * b[2] - a[2] * b[1]; o[1] = a[2] * b[0] - a[0] * b[2]; o[2] = a[0] * b[1] - a[1] * b[0];
}
static float dot(const float *a, const float *b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

/* view * projection, column-major. Left-handed (x right, y up, z ahead) and flipped in y so that the first row
 * glReadPixels gives is the top of the picture. */
static void view_proj(const float *eye, const float *target, float fovy, float aspect, float zn, float zf, float *m)
{
    float f[3] = { target[0] - eye[0], target[1] - eye[1], target[2] - eye[2] }, up[3] = { 0, 1, 0 }, r[3], u[3];
    normalize(f);
    cross(up, f, r);
    normalize(r);
    cross(f, r, u);
    float t = 1.0f / tanf(fovy * 0.5f), A = (zf + zn) / (zf - zn), B = -2 * zf * zn / (zf - zn);
    float rows[4][4] = {
        { t / aspect * r[0], t / aspect * r[1], t / aspect * r[2], -t / aspect * dot(r, eye) },
        { -t * u[0], -t * u[1], -t * u[2], t * dot(u, eye) },
        { A * f[0], A * f[1], A * f[2], -A * dot(f, eye) + B },
        { f[0], f[1], f[2], -dot(f, eye) },
    };
    for (int c = 0; c < 4; c++)
        for (int rw = 0; rw < 4; rw++)
            m[c * 4 + rw] = rows[rw][c];
}

/* ---- meshes: flat-shaded triangles, built from boxes --------------------------------------------- */

struct vtx { float p[3], n[3], c[3], mode; };   /* mode: 0 lit colour, 1 lit, takes the car's colour, 2 glows */
struct mesh { GLuint vbo, vao; int n; };
static struct vtx *mb;                          /* the mesh being built */
static int mb_n, mb_cap;

static void put(const float *p, const float *n, uint32_t c, float mode)
{
    if (mb_n == mb_cap) { mb_cap = mb_cap ? mb_cap * 2 : 1024; mb = realloc(mb, (size_t)mb_cap * sizeof(*mb)); }
    struct vtx *v = &mb[mb_n++];
    memcpy(v->p, p, 12);
    memcpy(v->n, n, 12);
    v->c[0] = ((c >> 16) & 255) / 255.0f; v->c[1] = ((c >> 8) & 255) / 255.0f; v->c[2] = (c & 255) / 255.0f;
    v->mode = mode;
}

static void quad(const float *a, const float *b, const float *c, const float *d, uint32_t col, float mode)
{
    float e1[3] = { b[0] - a[0], b[1] - a[1], b[2] - a[2] }, e2[3] = { d[0] - a[0], d[1] - a[1], d[2] - a[2] }, n[3];
    cross(e1, e2, n);
    normalize(n);
    put(a, n, col, mode); put(b, n, col, mode); put(c, n, col, mode);
    put(a, n, col, mode); put(c, n, col, mode); put(d, n, col, mode);
}

/* a box from (x0,y0,z0) to (x1,y1,z1), the top face drawn smaller by (ix, iz0, iz1): a roof narrower than the
 * body, or a sloped bonnet */
static void frustum(float x0, float y0, float z0, float x1, float y1, float z1, float ix, float iz0, float iz1,
                    uint32_t col, float mode)
{
    float b[4][3] = { { x0, y0, z0 }, { x1, y0, z0 }, { x1, y0, z1 }, { x0, y0, z1 } };
    float t[4][3] = { { x0 + ix, y1, z0 + iz0 }, { x1 - ix, y1, z0 + iz0 }, { x1 - ix, y1, z1 - iz1 }, { x0 + ix, y1, z1 - iz1 } };
    quad(t[0], t[3], t[2], t[1], col, mode);                    /* top */
    quad(b[0], b[1], b[2], b[3], col, mode);                    /* bottom */
    quad(b[0], t[0], t[1], b[1], col, mode);                    /* rear (z0) */
    quad(b[2], t[2], t[3], b[3], col, mode);                    /* front (z1) */
    quad(b[3], t[3], t[0], b[0], col, mode);                    /* left */
    quad(b[1], t[1], t[2], b[2], col, mode);                    /* right */
}
static void box(float x0, float y0, float z0, float x1, float y1, float z1, uint32_t col, float mode)
{
    frustum(x0, y0, z0, x1, y1, z1, 0, 0, 0, col, mode);
}

static struct mesh finish_mesh(void)
{
    struct mesh m = { 0, 0, mb_n };
    glGenVertexArrays(1, &m.vao);
    glGenBuffers(1, &m.vbo);
    glBindVertexArray(m.vao);
    glBindBuffer(GL_ARRAY_BUFFER, m.vbo);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)mb_n * sizeof(struct vtx), mb, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0); glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(struct vtx), (void *)0);
    glEnableVertexAttribArray(1); glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(struct vtx), (void *)12);
    glEnableVertexAttribArray(2); glVertexAttribPointer(2, 3, GL_FLOAT, GL_FALSE, sizeof(struct vtx), (void *)24);
    glEnableVertexAttribArray(3); glVertexAttribPointer(3, 1, GL_FLOAT, GL_FALSE, sizeof(struct vtx), (void *)36);
    mb_n = 0;
    return m;
}

#define GLASS 0x16202eu
#define TYRE 0x15161au
#define TAIL 0xff2f3fu
#define LAMP 0xfff1c0u

/* A car, seen from behind: its middle at the origin on the ground, the front towards +z. Kinds: hatchback,
 * saloon, van, lorry. Body parts of mode 1 take the car's colour. */
static struct mesh build_car(int kind)
{
    float w = car_w[kind], l = car_l[kind], hw = w / 2, hl = l / 2;
    float wheel_r = 85, wx = hw - 18, wz = l * 0.30f;
    for (int i = 0; i < 4; i++) {                                           /* the wheels, a little proud of the body */
        float x = i & 1 ? wx : -wx, z = i & 2 ? wz : -wz;
        box(x - 32 + (i & 1 ? 14 : -14), 0, z - wheel_r, x + 32 + (i & 1 ? 14 : -14), wheel_r * 2, z + wheel_r, TYRE, 0);
    }
    if (kind == 3) {                                                        /* a lorry: a cab, and a long box behind it */
        float cab0 = hl * 0.42f;
        box(-hw, 55, cab0, hw, 330, hl, 1 ? 0xd9dde4u : 0, 1);              /* the cab */
        box(-hw * 0.86f, 220, hl - 2, hw * 0.86f, 320, hl + 1, GLASS, 0);    /* its windscreen */
        box(-hw, 55, -hl, hw, 600, cab0 - 6, 0xe9ecf1u, 1);                 /* the load box */
        box(-hw * 0.96f, 130, -hl - 2, hw * 0.96f, 570, -hl, 0xc9ced8u, 0);  /* its back doors */
        box(-hw * 0.9f, 70, -hl - 3, -hw * 0.62f, 118, -hl, TAIL, 2);
        box(hw * 0.62f, 70, -hl - 3, hw * 0.9f, 118, -hl, TAIL, 2);
        box(-hw * 0.9f, 55, cab0 + 10, hw * 0.9f, 95, hl + 4, 0x3a3d46u, 0);   /* the bumper */
        return finish_mesh();
    }
    if (kind == 2) {                                                        /* a van: tall and square */
        frustum(-hw, 55, -hl, hw, 215, hl, 0, 0, 0, 1 ? 0xffffffu : 0, 1);
        frustum(-hw, 215, -hl, hw, 430, hl, hw * 0.05f, 0, hl * 0.12f, 0xffffffu, 1);
        box(-hw * 0.82f, 250, -hl - 2, hw * 0.82f, 370, -hl + 1, GLASS, 0);  /* the back windows */
        box(-hw * 0.9f, 80, -hl - 3, -hw * 0.62f, 128, -hl, TAIL, 2);
        box(hw * 0.62f, 80, -hl - 3, hw * 0.9f, 128, -hl, TAIL, 2);
        return finish_mesh();
    }
    /* a hatchback (0) and a saloon (1): a low body, glass, a roof */
    float body_top = kind == 1 ? 175 : 190, roof = kind == 1 ? 318 : 335;
    frustum(-hw, 55, -hl, hw, body_top, hl, 0, 0, hl * (kind == 1 ? 0.10f : 0.16f), 0xffffffu, 1);   /* the body, a sloping bonnet */
    float c0 = kind == 1 ? -hl * 0.38f : -hl * 0.78f, c1 = kind == 1 ? hl * 0.24f : hl * 0.20f;       /* the cabin, along z */
    float in = kind == 1 ? hw * 0.2f : hw * 0.16f, s0 = kind == 1 ? 60 : 24, s1 = 90;
    frustum(-hw * 0.9f, body_top - 2, c0, hw * 0.9f, roof - 12, c1, in * 0.5f, s0, s1, GLASS, 0);       /* the glass */
    frustum(-hw * 0.9f + in * 0.6f, roof - 14, c0 + s0 * 0.8f, hw * 0.9f - in * 0.6f, roof, c1 - s1 * 0.9f, 0, 0, 0, 0xffffffu, 1); /* the roof */
    box(-hw * 0.9f, 95, -hl - 3, -hw * 0.56f, 142, -hl, TAIL, 2);           /* the tail lights */
    box(hw * 0.56f, 95, -hl - 3, hw * 0.9f, 142, -hl, TAIL, 2);
    box(-hw * 0.3f, 100, -hl - 3, hw * 0.3f, 124, -hl, 0xe8e8ecu, 0);       /* the number plate */
    box(-hw * 0.9f, 55, -hl - 5, hw * 0.9f, 90, -hl, 0x2b2d34u, 0);         /* the bumper */
    box(-hw * 0.9f, 60, hl - 2, -hw * 0.5f, 110, hl + 3, LAMP, 2);
    box(hw * 0.5f, 60, hl - 2, hw * 0.9f, 110, hl + 3, LAMP, 2);
    return finish_mesh();
}

/* roadside things, 1 unit tall so that an instance's scale is its height in world units */
static void blob(float cx, float cy, float cz, float r, uint32_t col, float squash)
{
    int seg = 7, ring = 4;
    for (int j = 0; j < ring; j++) {
        float a0 = M_PI * j / ring - M_PI / 2, a1 = M_PI * (j + 1) / ring - M_PI / 2;
        for (int i = 0; i < seg; i++) {
            float b0 = 2 * M_PI * i / seg, b1 = 2 * M_PI * (i + 1) / seg;
            float p[4][3];
            float aa[4] = { a0, a0, a1, a1 }, bb[4] = { b0, b1, b1, b0 };
            for (int k = 0; k < 4; k++) {
                p[k][0] = cx + r * cosf(aa[k]) * cosf(bb[k]);
                p[k][1] = cy + r * sinf(aa[k]) * squash;
                p[k][2] = cz + r * cosf(aa[k]) * sinf(bb[k]);
            }
            quad(p[0], p[3], p[2], p[1], col, 0);
        }
    }
}

static struct mesh build_bush(void)
{
    blob(-0.2f, 0.20f, 0, 0.26f, 0x4f8433u, 0.9f);
    blob(0.22f, 0.19f, 0.05f, 0.24f, 0x4f8433u, 0.9f);
    blob(0, 0.36f, 0, 0.30f, 0x6a9e3fu, 0.9f);
    blob(-0.06f, 0.46f, -0.08f, 0.14f, 0x86b852u, 0.9f);
    return finish_mesh();
}
static struct mesh build_rock(void)
{
    blob(0, 0.28f, 0, 0.55f, 0x9b6f43u, 0.55f);
    blob(0.18f, 0.42f, -0.06f, 0.3f, 0xb28350u, 0.6f);
    return finish_mesh();
}
static struct mesh build_cactus(void)
{
    box(-0.12f, 0, -0.12f, 0.12f, 1.0f, 0.12f, 0x3f8a4au, 0);
    box(-0.36f, 0.50f, -0.06f, -0.12f, 0.62f, 0.06f, 0x3f8a4au, 0);
    box(-0.36f, 0.50f, -0.06f, -0.24f, 0.86f, 0.06f, 0x3f8a4au, 0);
    box(0.12f, 0.36f, -0.06f, 0.36f, 0.48f, 0.06f, 0x3f8a4au, 0);
    box(0.24f, 0.36f, -0.06f, 0.36f, 0.72f, 0.06f, 0x3f8a4au, 0);
    return finish_mesh();
}

/* the guard rails along both sides: a beam and posts, in pieces 512 long so that the road's bend can bend them.
 * Built over ZFAR; the posts are every 1024, so the whole thing is moved by the scroll modulo 1024. */
#define RAIL_PIECES ((int)(ZFAR / 512) + 4)
static struct mesh build_rails(void)
{
    for (int side = 0; side < 2; side++) {
        float s = side ? 1 : -1, x0 = ROAD_HW + 40, x1 = x0 + 26;
        for (int i = 0; i < RAIL_PIECES; i++) {
            float z0 = -4096 + i * 512.0f, z1 = z0 + 512;
            float a = s > 0 ? x0 : -x1, b = s > 0 ? x1 : -x0;
            box(a, 120, z0, b, 205, z1, 0xc2cad6u, 0);                          /* the beam */
            box(a, 205, z0, b, 214, z1, 0xe8edf3u, 0);                          /* its lit top */
            if (i % 2 == 0)
                box(a + (s > 0 ? -4 : 4), 0, z0 + 232, b + (s > 0 ? 4 : -4), 215, z0 + 280, 0x4d5565u, 0);   /* a post */
        }
    }
    return finish_mesh();
}

/* ---- shaders ------------------------------------------------------------------------------------- */

static GLuint compile(GLenum type, const char *src)
{
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) { char log[800]; glGetShaderInfoLog(s, sizeof(log), NULL, log); fprintf(stderr, "lkx-gld: shader: %s\n", log); exit(1); }
    return s;
}
static GLuint program(const char *vs, const char *fs)
{
    GLuint p = glCreateProgram();
    glAttachShader(p, compile(GL_VERTEX_SHADER, vs));
    glAttachShader(p, compile(GL_FRAGMENT_SHADER, fs));
    glLinkProgram(p);
    GLint ok = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) { char log[800]; glGetProgramInfoLog(p, sizeof(log), NULL, log); fprintf(stderr, "lkx-gld: link: %s\n", log); exit(1); }
    return p;
}

#define HEAD "#version 300 es\nprecision highp float;\n"
/* The road bends: a point `u` units beyond the player moves sideways by bendk * u * u (the game's bend()). */
#define BEND "uniform float u_bendk; uniform float u_zp;\n" \
             "vec3 bend(vec3 p){ float u = max(p.z - u_zp, 0.0); p.x += u_bendk * u * u; return p; }\n"
#define HAZE "const vec3 HAZE = vec3(0.92, 0.85, 0.70);\n" \
             "vec3 fog(vec3 c, float d){ float f = clamp((d - 14000.0) / 50000.0, 0.0, 1.0); return mix(c, HAZE, f * f * (3.0 - 2.0 * f)); }\n" \
             "vec3 srgb(vec3 c){ return c; }\n"

/* instanced things: cars and scenery */
static const char *obj_vs = HEAD BEND
    "uniform mat4 u_vp;\n"
    "layout(location=0) in vec3 a_pos; layout(location=1) in vec3 a_nrm; layout(location=2) in vec3 a_col; layout(location=3) in float a_mode;\n"
    "layout(location=4) in vec4 i_a;   /* x, z, scale, yaw */\n"
    "layout(location=5) in vec4 i_b;   /* body colour rgb, roll */\n"
    "out vec3 v_nrm; out vec3 v_col; out float v_mode; out float v_dist;\n"
    "void main(){\n"
    "  float cy = cos(i_a.w), sy = sin(i_a.w), cr = cos(i_b.w), sr = sin(i_b.w);\n"
    "  vec3 p = a_pos * i_a.z, n = a_nrm;\n"
    "  p = vec3(p.x * cr - p.y * sr, p.x * sr + p.y * cr, p.z); n = vec3(n.x * cr - n.y * sr, n.x * sr + n.y * cr, n.z);\n"
    "  p = vec3(p.x * cy + p.z * sy, p.y, -p.x * sy + p.z * cy); n = vec3(n.x * cy + n.z * sy, n.y, -n.x * sy + n.z * cy);\n"
    "  p += vec3(i_a.x, 0.0, i_a.y);\n"
    "  p = bend(p);\n"
    "  gl_Position = u_vp * vec4(p, 1.0);\n"
    "  v_nrm = n; v_col = a_mode > 0.5 && a_mode < 1.5 ? i_b.rgb : a_col; v_mode = a_mode; v_dist = gl_Position.w;\n"
    "}\n";
static const char *obj_fs = HEAD HAZE
    "in vec3 v_nrm; in vec3 v_col; in float v_mode; in float v_dist; out vec4 o;\n"
    "void main(){\n"
    "  vec3 n = normalize(v_nrm), L = normalize(vec3(-0.45, 0.8, -0.35));\n"
    "  float d = max(dot(n, L), 0.0), sky = 0.5 + 0.5 * n.y;\n"
    "  vec3 c = v_mode > 1.5 ? v_col * 1.15 : v_col * (0.38 + 0.30 * sky + 0.62 * d);\n"
    "  o = vec4(fog(c, v_dist).bgr, 1.0);\n"
    "}\n";

/* the rails: the same, as one static mesh moved along z */
static const char *rail_vs = HEAD BEND
    "uniform mat4 u_vp; uniform float u_dz;\n"
    "layout(location=0) in vec3 a_pos; layout(location=1) in vec3 a_nrm; layout(location=2) in vec3 a_col; layout(location=3) in float a_mode;\n"
    "out vec3 v_nrm; out vec3 v_col; out float v_mode; out float v_dist;\n"
    "void main(){ vec3 p = bend(a_pos + vec3(0.0, 0.0, u_dz)); gl_Position = u_vp * vec4(p, 1.0);\n"
    "  v_nrm = a_nrm; v_col = a_col; v_mode = a_mode; v_dist = gl_Position.w; }\n";

/* the ground: the sand, the road and its lines, by the position on the ground */
static const char *ground_vs = HEAD BEND
    "uniform mat4 u_vp;\n"
    "layout(location=0) in vec2 a_xz;\n"
    "out vec2 v_xz; out float v_dist;\n"
    "void main(){ v_xz = a_xz; vec3 p = bend(vec3(a_xz.x, 0.0, a_xz.y)); gl_Position = u_vp * vec4(p, 1.0); v_dist = gl_Position.w; }\n";
static const char *ground_fs = HEAD HAZE
    "uniform float u_scroll;\n"
    "in vec2 v_xz; in float v_dist; out vec4 o;\n"
    "float band(float d, float hw){ float fw = fwidth(d) + 0.0001; return clamp((hw - abs(d)) / fw + 0.5, 0.0, 1.0); }\n"
    "float stripe(float g, float period, float duty){ float t = mod(g, period) / period; float fw = fwidth(g) / period + 0.0001;\n"
    "  return clamp((duty - t) / fw + 0.5, 0.0, 1.0) * clamp(t / fw + 0.5, 0.0, 1.0); }\n"
    "void main(){\n"
    "  float x = v_xz.x, g = v_xz.y + u_scroll;\n"
    "  float odd = floor(mod(g, 1024.0) / 512.0);\n"
    "  vec3 sand = mix(vec3(0.863, 0.749, 0.502), vec3(0.827, 0.710, 0.455), odd);\n"
    "  vec3 asph = mix(vec3(0.302, 0.314, 0.349), vec3(0.278, 0.290, 0.325), odd);\n"
    "  float road = band(x - 0.0, 1000.0) ;\n"
    "  vec3 c = mix(sand, asph, road);\n"
    "  vec3 line = vec3(0.945, 0.945, 0.918);\n"
    "  float l = band(abs(x) - 910.0, 20.0) + band(x, 14.0);\n"
    "  l += band(abs(x) - 500.0, 14.0) * stripe(g, 1024.0, 0.25);\n"
    "  c = mix(c, line, clamp(l, 0.0, 1.0) * road);\n"
    "  o = vec4(fog(c, v_dist).bgr, 1.0);\n"
    "}\n";

/* the sky: a gradient down to the haze at the horizon (gl_FragCoord.y / H is the row from the top) */
static const char *sky_vs = HEAD
    "void main(){ vec2 p = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2); gl_Position = vec4(p * 2.0 - 1.0, 1.0, 1.0); }\n";
static const char *sky_fs = HEAD
    "uniform float u_hz; uniform float u_h; out vec4 o;\n"
    "void main(){ float r = gl_FragCoord.y / u_h; float t = clamp(r / max(u_hz, 0.01), 0.0, 1.0);\n"
    "  vec3 top = vec3(0.27, 0.58, 0.90), hor = vec3(0.92, 0.85, 0.70);\n"
    "  o = vec4(mix(top, hor, pow(t, 1.6)).bgr, 1.0); }\n";

/* blob shadows under the cars */
static const char *sh_vs = HEAD BEND
    "uniform mat4 u_vp;\n"
    "layout(location=0) in vec2 a_c;\n"
    "layout(location=4) in vec4 i_a;   /* x, z, half width, half length */\n"
    "out vec2 v_c;\n"
    "void main(){ v_c = a_c; vec3 p = bend(vec3(i_a.x + a_c.x * i_a.z, 1.5, i_a.y + a_c.y * i_a.w)); gl_Position = u_vp * vec4(p, 1.0); }\n";
static const char *sh_fs = HEAD
    "in vec2 v_c; out vec4 o;\n"
    "void main(){ float d = length(v_c); o = vec4(0.12, 0.08, 0.03, 0.5 * (1.0 - smoothstep(0.55, 1.0, d))); }\n";

/* ---- the scene ----------------------------------------------------------------------------------- */

static GLuint p_obj, p_rail, p_ground, p_sky, p_sh;
static struct mesh m_car[4], m_bush, m_rock, m_cactus, m_rails;
static GLuint ground_vao, ground_vbo, ground_n, inst_vbo, sh_vao, sh_vbo, empty_vao;
static GLuint fbo_ms, rb_col, rb_depth, fbo_res, tex_res, rb_res_depth;
static int ms_on;

struct inst { float a[4], b[4]; };

static uint32_t hash(uint32_t a)
{
    a ^= a >> 16; a *= 0x7feb352du; a ^= a >> 15; a *= 0x846ca68bu; a ^= a >> 16;
    return a;
}

static void bind_instances(GLuint vbo, int stride_floats)
{
    (void)stride_floats;
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glEnableVertexAttribArray(4); glVertexAttribPointer(4, 4, GL_FLOAT, GL_FALSE, sizeof(struct inst), (void *)0);
    glVertexAttribDivisor(4, 1);
    glEnableVertexAttribArray(5); glVertexAttribPointer(5, 4, GL_FLOAT, GL_FALSE, sizeof(struct inst), (void *)16);
    glVertexAttribDivisor(5, 1);
}

static void draw_instances(const struct mesh *m, const struct inst *list, int n)
{
    if (n <= 0)
        return;
    glBindVertexArray(m->vao);
    glBindBuffer(GL_ARRAY_BUFFER, inst_vbo);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)n * sizeof(struct inst), list, GL_STREAM_DRAW);
    bind_instances(inst_vbo, 8);
    glDrawArraysInstanced(GL_TRIANGLES, 0, m->n, n);
}

static void rgb_of(uint32_t c, float *o)
{
    o[0] = ((c >> 16) & 255) / 255.0f; o[1] = ((c >> 8) & 255) / 255.0f; o[2] = (c & 255) / 255.0f;
}

static void gl_init_resources(void)
{
    p_obj = program(obj_vs, obj_fs);
    p_rail = program(rail_vs, obj_fs);
    p_ground = program(ground_vs, ground_fs);
    p_sky = program(sky_vs, sky_fs);
    p_sh = program(sh_vs, sh_fs);
    for (int k = 0; k < 4; k++)
        m_car[k] = build_car(k);
    m_bush = build_bush();
    m_rock = build_rock();
    m_cactus = build_cactus();
    m_rails = build_rails();
    /* the ground: rows across the road, close together near the player and far apart far off */
    static float g[400 * 4 * 3 * 2];
    int rows = 0;
    float zs[400];
    for (float z = -4000; z < 20000 && rows < 300; z += 400) zs[rows++] = z;
    for (float z = 20000; z <= ZFAR + 2000 && rows < 399; z += 2500) zs[rows++] = z;
    int n = 0;
    for (int i = 0; i + 1 < rows; i++) {
        float x[3] = { -30000, 0, 30000 };
        (void)x;
        static const float xs[5] = { -40000, -1500, 0, 1500, 40000 };
        for (int j = 0; j < 4; j++) {
            float x0 = xs[j], x1 = xs[j + 1], z0 = zs[i], z1 = zs[i + 1];
            float q[6][2] = { { x0, z0 }, { x1, z0 }, { x1, z1 }, { x0, z0 }, { x1, z1 }, { x0, z1 } };
            memcpy(&g[n * 2], q, sizeof(q));
            n += 6;
        }
    }
    ground_n = (GLuint)n;
    glGenVertexArrays(1, &ground_vao);
    glGenBuffers(1, &ground_vbo);
    glBindVertexArray(ground_vao);
    glBindBuffer(GL_ARRAY_BUFFER, ground_vbo);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)n * 8, g, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0); glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 8, (void *)0);
    glGenVertexArrays(1, &empty_vao);
    glGenBuffers(1, &inst_vbo);
    /* the shadow's unit square (-1..1) */
    static const float sq[12] = { -1, -1, 1, -1, 1, 1, -1, -1, 1, 1, -1, 1 };
    glGenVertexArrays(1, &sh_vao);
    glGenBuffers(1, &sh_vbo);
    glBindVertexArray(sh_vao);
    glBindBuffer(GL_ARRAY_BUFFER, sh_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(sq), sq, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0); glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 8, (void *)0);
}

/* the framebuffers: drawn multisampled if the GPU allows, then resolved into a texture to read back */
static void make_targets(void)
{
    glGenFramebuffers(1, &fbo_res);
    glGenTextures(1, &tex_res);
    glBindTexture(GL_TEXTURE_2D, tex_res);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, W, H, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glGenRenderbuffers(1, &rb_res_depth);
    glBindRenderbuffer(GL_RENDERBUFFER, rb_res_depth);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT16, W, H);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo_res);
    if (getenv("GLD_RB")) {                                     /* a renderbuffer instead of a texture (maybe quicker to read) */
        GLuint rb;
        glGenRenderbuffers(1, &rb);
        glBindRenderbuffer(GL_RENDERBUFFER, rb);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, W, H);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, rb);
    } else {
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex_res, 0);
    }
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, rb_res_depth);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) { fprintf(stderr, "lkx-gld: framebuffer incomplete\n"); exit(1); }
    ms_on = 0;
    if (samples > 1) {
        glGenFramebuffers(1, &fbo_ms);
        glGenRenderbuffers(1, &rb_col);
        glGenRenderbuffers(1, &rb_depth);
        glBindRenderbuffer(GL_RENDERBUFFER, rb_col);
        glRenderbufferStorageMultisample(GL_RENDERBUFFER, samples, GL_RGBA8, W, H);
        glBindRenderbuffer(GL_RENDERBUFFER, rb_depth);
        glRenderbufferStorageMultisample(GL_RENDERBUFFER, samples, GL_DEPTH_COMPONENT16, W, H);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo_ms);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, rb_col);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, rb_depth);
        if (glGetError() == GL_NO_ERROR && glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE)
            ms_on = 1;
        else
            fprintf(stderr, "lkx-gld: no %dx multisampling, drawing without\n", samples);
    }
    fprintf(stderr, "lkx-gld: %dx%d, %s\n", W, H, ms_on ? "multisampled" : "no antialiasing");
}

static void render(const struct gl_scene *in_scene)
{
    struct scene_f fsc = { (float)in_scene->xp, (float)in_scene->zp, (float)in_scene->scroll, (float)in_scene->curve,
                           (float)in_scene->speed, (float)in_scene->shake_x, (float)in_scene->shake_y, in_scene->ncars,
                           in_scene->player_vis, in_scene->player_col, (float)in_scene->player_lean, in_scene->cars };
    const struct scene_f *sc = &fsc;
    glBindFramebuffer(GL_FRAMEBUFFER, ms_on ? fbo_ms : fbo_res);
    glViewport(0, 0, W, H);
    glDisable(GL_CULL_FACE);
    glDisable(GL_BLEND);
    glDepthMask(GL_TRUE);
    glClear(GL_DEPTH_BUFFER_BIT);

    /* the camera: high behind the player, looking down the road */
    float xc = sc->xp * 0.55f + sc->shake_x;
    float aspect = (float)W / (float)H;
    float eye[3] = { xc, 2700 + sc->shake_y, sc->zp - 3300 }, tgt[3] = { xc * 0.85f, 0, sc->zp + 3600 };
    float fovy = 1.02f;
    if (aspect > 1) {                                   /* landscape: a lower, closer camera and a narrower view up and down */
        eye[1] = 1500 + sc->shake_y; eye[2] = sc->zp - 2900;
        tgt[2] = sc->zp + 5200;
        fovy = 0.80f;
    }
    float vp[16];
    view_proj(eye, tgt, fovy, aspect, 80, ZFAR + 4000, vp);
    float bendk = sc->curve / 262144000.0f;
    /* the horizon's row: where the far end of the road is (a bit above: the haze) */
    float hz[3] = { 0, 0, ZFAR };
    float cy = vp[1] * hz[0] + vp[5] * hz[1] + vp[9] * hz[2] + vp[13], cw = vp[3] * hz[0] + vp[7] * hz[1] + vp[11] * hz[2] + vp[15];
    float hzrow = (cy / cw + 1) * 0.5f;
    {   /* where the player's car is on the picture (the road is straight up to the player) */
        float pp[3] = { sc->xp, 120, sc->zp };
        float px = vp[0] * pp[0] + vp[4] * pp[1] + vp[8] * pp[2] + vp[12], py = vp[1] * pp[0] + vp[5] * pp[1] + vp[9] * pp[2] + vp[13];
        float pw = vp[3] * pp[0] + vp[7] * pp[1] + vp[11] * pp[2] + vp[15];
        if (fh && pw > 1) { fh->player_x = (int)((px / pw + 1) * 0.5f * W); fh->player_y = (int)((py / pw + 1) * 0.5f * H); }
    }

    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);
#define SETUP(p) do { glUseProgram(p); glUniformMatrix4fv(glGetUniformLocation(p, "u_vp"), 1, GL_FALSE, vp);                       glUniform1f(glGetUniformLocation(p, "u_bendk"), bendk); glUniform1f(glGetUniformLocation(p, "u_zp"), sc->zp); } while (0)

    /* what is on the road: the cars (and the player), each kind in one instanced draw */
    static struct inst by_kind[4][MAX_CARS + 1], shadows[MAX_CARS + 1];
    int cnt[4] = { 0, 0, 0, 0 }, nsh = 0;
    for (int i = 0; i < sc->ncars && i < MAX_CARS; i++) {
        const struct gl_car *c = &sc->cars[i];
        int k = c->kind & 3;
        struct inst *in = &by_kind[k][cnt[k]++];
        in->a[0] = c->x; in->a[1] = c->z; in->a[2] = 1; in->a[3] = (float)c->yaw * 0.001f;
        rgb_of(c->col, in->b); in->b[3] = (float)c->roll * 0.001f;
        shadows[nsh++] = (struct inst){ { c->x - 60, c->z - 40, car_w[k] * 0.62f, car_l[k] * 0.6f }, { 0, 0, 0, 0 } };
    }
    if (sc->player_vis) {
        struct inst *in = &by_kind[0][cnt[0]++];
        float lean = sc->player_lean;
        in->a[0] = sc->xp; in->a[1] = sc->zp; in->a[2] = 1; in->a[3] = lean * 0.004f;
        rgb_of(sc->player_col, in->b); in->b[3] = -lean * 0.0015f;
        shadows[nsh++] = (struct inst){ { sc->xp - 60, sc->zp - 40, car_w[0] * 0.62f, car_l[0] * 0.6f }, { 0, 0, 0, 0 } };
    }

    /* the roadside things, by the same slots as the game's 2D scenery */
    static struct inst sc_list[3][160];
    int sn[3] = { 0, 0, 0 };
    static const float size_units[3] = { 330, 420, 600 };
    int k_hi = (int)((sc->scroll + 52000) / SLOT), k_lo = (int)floorf((sc->scroll - 4000) / SLOT);
    for (int k = k_hi; k >= k_lo; k--) {
        float z = k * (float)SLOT - sc->scroll;
        for (int side = 0; side < 2; side++) {
            uint32_t h = hash((uint32_t)k * 2u + (uint32_t)side + 77u);
            if (h % 100 >= 72)
                continue;
            float x = ROAD_HW + 420 + (float)((h >> 8) % 1000);
            int type = (int)((h >> 20) % 3);
            if (sn[type] >= 160)
                continue;
            struct inst *in = &sc_list[type][sn[type]++];
            in->a[0] = side ? x : -x; in->a[1] = z; in->a[2] = size_units[type]; in->a[3] = (float)(h & 255) * 0.0246f;
            in->b[0] = in->b[1] = in->b[2] = in->b[3] = 0;
        }
    }
    /* Front to back, so the GPU can skip what is hidden: the things first, then the ground, then the sky in what is
     * left. The shadows go on the ground, blended. */
    SETUP(p_obj);
    for (int k = 0; k < 4; k++)
        draw_instances(&m_car[k], by_kind[k], cnt[k]);
    draw_instances(&m_cactus, sc_list[2], sn[2]);
    draw_instances(&m_rock, sc_list[1], sn[1]);
    draw_instances(&m_bush, sc_list[0], sn[0]);

    SETUP(p_rail);
    float sm = fmodf(sc->scroll, 1024.0f);
    glUniform1f(glGetUniformLocation(p_rail, "u_dz"), -sm);
    glBindVertexArray(m_rails.vao);
    glDrawArrays(GL_TRIANGLES, 0, m_rails.n);

    SETUP(p_ground);
    glUniform1f(glGetUniformLocation(p_ground, "u_scroll"), sc->scroll);
    glBindVertexArray(ground_vao);
    glDrawArrays(GL_TRIANGLES, 0, (GLsizei)ground_n);

    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDepthMask(GL_FALSE);
    SETUP(p_sh);
    glBindVertexArray(sh_vao);
    glBindBuffer(GL_ARRAY_BUFFER, inst_vbo);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)nsh * sizeof(struct inst), shadows, GL_STREAM_DRAW);
    bind_instances(inst_vbo, 8);
    glDrawArraysInstanced(GL_TRIANGLES, 0, 6, nsh);
    glDisable(GL_BLEND);

    glUseProgram(p_sky);                                /* only where nothing was drawn: above the horizon */
    glUniform1f(glGetUniformLocation(p_sky, "u_hz"), hzrow);
    glUniform1f(glGetUniformLocation(p_sky, "u_h"), (float)H);
    glBindVertexArray(empty_vao);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glDepthMask(GL_TRUE);

    if (ms_on) {                                    /* resolve the multisampled picture */
        glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo_ms);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fbo_res);
        glBlitFramebuffer(0, 0, W, H, 0, 0, W, H, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, fbo_res);
}

/* ---- the EGL display on the GPU -------------------------------------------------------------------- */

static void gpu_init(void)
{
    int fd = open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
    if (fd < 0) { perror("lkx-gld: renderD128"); exit(1); }
    struct gbm_device *gbm = gbm_create_device(fd);
    EGLDisplay d = gbm ? eglGetPlatformDisplay(EGL_PLATFORM_GBM_KHR, gbm, NULL) : EGL_NO_DISPLAY;
    EGLint maj, min;
    if (d == EGL_NO_DISPLAY || !eglInitialize(d, &maj, &min)) { fprintf(stderr, "lkx-gld: no EGL display\n"); exit(1); }
    eglBindAPI(EGL_OPENGL_ES_API);
    static const EGLint ca[] = { EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT, EGL_NONE };
    EGLConfig cfg;
    EGLint n = 0;
    if (!eglChooseConfig(d, ca, &cfg, 1, &n) || n < 1) { fprintf(stderr, "lkx-gld: no ES3 config\n"); exit(1); }
    static const EGLint xa[] = { EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE };
    EGLContext ctx = eglCreateContext(d, cfg, EGL_NO_CONTEXT, xa);
    if (ctx == EGL_NO_CONTEXT || !eglMakeCurrent(d, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx)) {
        fprintf(stderr, "lkx-gld: no GL context (0x%x)\n", eglGetError());
        exit(1);
    }
    fprintf(stderr, "lkx-gld: %s, %s\n", (const char *)glGetString(GL_RENDERER), (const char *)glGetString(GL_VERSION));
}

static int map_frame(void)
{
    size_t size = HEADER_BYTES + 2 * (size_t)W * H * 4;
    int fd = open(FRAME_FILE, O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, 0666);
    if (fd < 0 || ftruncate(fd, (off_t)size)) { perror("lkx-gld: frame file"); return -1; }
    shm = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (shm == MAP_FAILED) { shm = NULL; return -1; }
    fh = (struct frame_header *)shm;
    memset(shm, 0, size);
    *fh = (struct frame_header){ FRAME_MAGIC, 0, (uint32_t)W, (uint32_t)H, (uint32_t)W * 4, 0, W / 2, H * 3 / 4, 0 };
    return 0;
}

/* The picture into dst. With GLD_PBO the read goes into a buffer object and what is copied out is the PREVIOUS
 * frame's, so the GPU copy overlaps the next frame's drawing (one frame later on screen). */
static GLuint pbo[2];
static int pbo_i, pbo_have;

static void read_back(unsigned char *dst)
{
    glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo_res);
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    if (!getenv("GLD_PBO")) {
        glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, dst);
        return;
    }
    size_t size = (size_t)W * H * 4;
    if (!pbo[0]) {
        glGenBuffers(2, pbo);
        for (int i = 0; i < 2; i++) {
            glBindBuffer(GL_PIXEL_PACK_BUFFER, pbo[i]);
            glBufferData(GL_PIXEL_PACK_BUFFER, (GLsizeiptr)size, NULL, GL_STREAM_READ);
        }
    }
    glBindBuffer(GL_PIXEL_PACK_BUFFER, pbo[pbo_i]);
    glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, 0);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, pbo[pbo_i ^ 1]);
    if (pbo_have) {
        void *p = glMapBufferRange(GL_PIXEL_PACK_BUFFER, 0, (GLsizeiptr)size, GL_MAP_READ_BIT);
        if (p) { memcpy(dst, p, size); glUnmapBuffer(GL_PIXEL_PACK_BUFFER); }
    }
    pbo_have = 1;
    pbo_i ^= 1;
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
}

static void demo_scene(struct gl_scene *s, float t)
{
    memset(s, 0, sizeof(*s));
    s->magic = SCENE_MAGIC;
    s->xp = (int)(-250 + 500 * sinf(t * 0.6f));
    s->zp = 3000;
    s->scroll = (int)(t * 6000);
    s->curve = (int)(140 * sinf(t * 0.3f));
    s->player_vis = 1;
    s->player_col = 0xe8384f;
    s->player_lean = (int)(20 * cosf(t * 0.6f));
    static const uint32_t cols[8] = { 0xf2f2f4, 0x2b2f3a, 0x8a94a6, 0x2e7dd1, 0xe3b21f, 0x7e57c2, 0xe05a2a, 0x2fa36b };
    int n = 0;
    for (int i = 0; i < 14; i++)
        s->cars[n++] = (struct gl_car){ (i * 3 + 1) % 4 * 500 - 750 + 250 - 250 + 0, 5200 + i * 2300 - (int)fmodf(t * 800, 2300), i % 4, cols[i % 8], 0, 0, { 0, 0 } };
    s->ncars = n;
}

int main(int argc, char **argv)
{
    const char *demo = NULL;
    int bench = 0, serve = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--size") && i + 1 < argc) sscanf(argv[++i], "%dx%d", &W, &H);
        else if (!strcmp(argv[i], "--msaa") && i + 1 < argc) samples = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--demo") && i + 1 < argc) demo = argv[++i];
        else if (!strcmp(argv[i], "--bench")) bench = 1;
        else if (!strcmp(argv[i], "--serve")) serve = 1;
    }
    if (!demo && !bench && !serve) { fprintf(stderr, "usage: lkx-gld --serve|--bench|--demo OUT.ppm [--size WxH] [--msaa N]\n"); return 2; }
    signal(SIGPIPE, SIG_IGN);
    gpu_init();
    make_targets();
    gl_init_resources();

    if (demo) {
        struct gl_scene s;
        demo_scene(&s, 1.0f);
        render(&s);
        unsigned char *px = malloc((size_t)W * H * 4);
        read_back(px);
        FILE *f = fopen(demo, "wb");
        fprintf(f, "P6\n%d %d\n255\n", W, H);
        for (int i = 0; i < W * H; i++) { fputc(px[i * 4 + 2], f); fputc(px[i * 4 + 1], f); fputc(px[i * 4], f); }   /* BGRA out of the shaders */
        fclose(f);
        fprintf(stderr, "lkx-gld: wrote %s (%d GL errors)\n", demo, glGetError() != 0);
        return 0;
    }
    if (bench) {
        unsigned char *px = malloc((size_t)W * H * 4);
        struct gl_scene s;
        struct timespec a, b;
        clock_gettime(CLOCK_MONOTONIC, &a);
        for (int i = 0; i < 240; i++) {
            demo_scene(&s, i / 30.0f);
            render(&s);
            if (!getenv("GLD_NOREAD"))
                read_back(px);
            else
                glFinish();
        }
        clock_gettime(CLOCK_MONOTONIC, &b);
        double sec = (b.tv_sec - a.tv_sec) + (b.tv_nsec - a.tv_nsec) / 1e9;
        fprintf(stderr, "lkx-gld: bench: 240 frames in %.2f s = %.1f fps\n", sec, 240 / sec);
        return 0;
    }

    /* serve: a scene in, a picture out */
    if (map_frame()) return 1;
    int s = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    strncpy(addr.sun_path, SOCK_FILE, sizeof(addr.sun_path) - 1);
    unlink(SOCK_FILE);
    if (s < 0 || bind(s, (struct sockaddr *)&addr, sizeof(addr))) { perror("lkx-gld: socket"); return 1; }
    chmod(SOCK_FILE, 0666);
    fprintf(stderr, "lkx-gld: serving %dx%d on %s\n", W, H, SOCK_FILE);
    struct gl_scene sc;
    memset(&sc, 0, sizeof(sc));
    demo_scene(&sc, 0);
    render(&sc);
    read_back(shm + HEADER_BYTES);
    fh->seq++;
    for (;;) {
        struct gl_scene in;
        ssize_t n = recv(s, &in, sizeof(in), 0);          /* waits for the next scene */
        if (n < 0 && errno == EINTR) continue;
        if (n < (ssize_t)offsetof(struct gl_scene, cars)) continue;
        if (in.magic == 0x5154494bu) break;               /* "KITQ": quit */
        for (;;) {                                        /* take the newest one waiting, drop the older */
            struct gl_scene more;
            ssize_t m = recv(s, &more, sizeof(more), MSG_DONTWAIT);
            if (m < (ssize_t)offsetof(struct gl_scene, cars)) break;
            if (more.magic == 0x5154494bu) { unlink(SOCK_FILE); return 0; }
            in = more;
            n = m;
        }
        if (in.magic != SCENE_MAGIC) continue;
        if (in.ncars > MAX_CARS) in.ncars = MAX_CARS;
        render(&in);
        uint32_t next = fh->slot ^ 1;                     /* the other picture: the app is reading the finished one */
        read_back(shm + HEADER_BYTES + (size_t)next * W * H * 4);
        __sync_synchronize();
        fh->scene_seq = in.seq;
        fh->slot = next;
        __sync_synchronize();
        fh->seq++;
    }
    unlink(SOCK_FILE);
    return 0;
}
