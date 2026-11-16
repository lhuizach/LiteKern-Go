/* Camera: the pipeline imx135 -> csiphy0 -> csid0 -> ispif0 -> vfe0_rdi0 (or, for the front
 * camera, s5k5e3yx -> csiphy1 -> csid1 -> ispif0 -> vfe0_rdi0), set up through the media device, with
 * frames taken as they come and auto-exposure on the sensor's own controls. Nothing here waits: the
 * app polls from its timer.
 *
 * The rest of the app only knows one kind of frame: packed RAW10, red first (RGGB), CAM_W x CAM_H for
 * the live picture, turned and flipped for the rear camera's mounting. The front sensor's frames
 * (1920x1080, green first: GRBG) are made into that here (normalise), at the right turn for its own
 * mounting and mirrored like a selfie view. */
#include "cam.h"
#include "sdk/kern86.h"

#include <fcntl.h>
#include <linux/media.h>
#include <linux/v4l2-subdev.h>
#include <linux/videodev2.h>
#include <poll.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#define CODE MEDIA_BUS_FMT_SRGGB10_1X10
#define BUFS 4
#define EXPOSURE_MAX 2620               /* the rear's; the front's live mode has 1459 */
#define FRONT_EXPOSURE_MAX 2000
/* The front camera streams its whole sensor (2576x1932) all the time, and the live picture and the photos use
 * the middle 16:9 of it (1448 rows: 2576 x 1448 turned is the screen's 9:16), so what the preview shows is
 * what the photo gets, and a photo needs no change of mode. In cells of 2 x 2 pixels: */
#define FRONT_CELLS_W 1288
#define FRONT_OY 121                    /* cells down to the window */
#define FRONT_CELLS_H 724
#define GAIN_CODE_MAX 192               /* 4x: past that it's mostly noise */
#define TARGET 0.14f                    /* the mean brightness we aim for, linear */

static int media = -1, sensor = -1, video = -1, stride;
/* Which camera: the names of its parts, and the size its live picture really is. */
static int front;
static const char *s_name = "imx135", *s_phy = "msm_csiphy0", *s_csid = "msm_csid0";
static int live_w = CAM_W, live_h = CAM_H;
static int flip_x, flip_y;               /* the front picture's flips (see load_flips): none is upright on this phone */
static uint8_t *norm;                   /* the front's live frame, as the app wants it */
static int norm_stride;
static void *mem[BUFS];
static uint32_t mem_len[BUFS];
static unsigned nbufs;
static int exposure = 1600, gain_code = 128;
static int paused;

static int find(const char *prefix, struct media_entity_desc *out)
{
    uint32_t id = MEDIA_ENT_ID_FLAG_NEXT;
    for (;;) {
        struct media_entity_desc e;
        memset(&e, 0, sizeof(e));
        e.id = id;
        if (ioctl(media, MEDIA_IOC_ENUM_ENTITIES, &e))
            return -1;
        if (!strncmp(e.name, prefix, strlen(prefix))) {
            *out = e;
            return 0;
        }
        id = e.id | MEDIA_ENT_ID_FLAG_NEXT;
    }
}

static int open_node(const struct media_entity_desc *e)
{
    char path[64], line[128], dev[64] = "";
    snprintf(path, sizeof(path), "/sys/dev/char/%u:%u/uevent", e->dev.major, e->dev.minor);
    FILE *f = fopen(path, "r");
    if (!f)
        return -1;
    while (fgets(line, sizeof(line), f))
        if (!strncmp(line, "DEVNAME=", 8)) {
            line[strcspn(line, "\n")] = 0;
            snprintf(dev, sizeof(dev), "/dev/%.50s", line + 8);
        }
    fclose(f);
    return open(dev, O_RDWR | O_NONBLOCK);
}

static int set_fmt(const char *name, unsigned pad, int w, int h)
{
    struct media_entity_desc e;
    struct v4l2_subdev_format f;
    if (find(name, &e))
        return -1;
    int fd = open_node(&e);
    if (fd < 0)
        return -1;
    memset(&f, 0, sizeof(f));
    f.which = V4L2_SUBDEV_FORMAT_ACTIVE;
    f.pad = pad;
    f.format.width = w;
    f.format.height = h;
    f.format.code = CODE;
    f.format.field = V4L2_FIELD_NONE;
    int err = ioctl(fd, VIDIOC_SUBDEV_S_FMT, &f);
    close(fd);
    return err;
}

static int link_pads(const char *from, uint16_t from_pad, const char *to, uint16_t to_pad)
{
    struct media_entity_desc a, b;
    struct media_link_desc l;
    if (find(from, &a) || find(to, &b))
        return -1;
    memset(&l, 0, sizeof(l));
    l.source.entity = a.id;
    l.source.index = from_pad;
    l.source.flags = MEDIA_PAD_FL_SOURCE;
    l.sink.entity = b.id;
    l.sink.index = to_pad;
    l.sink.flags = MEDIA_PAD_FL_SINK;
    l.flags = MEDIA_LNK_FL_ENABLED;
    return ioctl(media, MEDIA_IOC_SETUP_LINK, &l);
}

static void set_ctrl(uint32_t id, int value)
{
    struct v4l2_control c = { id, value };
    ioctl(sensor, VIDIOC_S_CTRL, &c);
}

/* The size every stage works at; the sensor picks the mode of that size. */
static int set_sizes(int w, int h)
{
    const char *const nodes[] = { s_phy, s_csid, "msm_ispif0", "msm_vfe0_rdi0" };
    if (set_fmt(s_name, 0, w, h))
        return -1;
    for (unsigned i = 0; i < 4; i++)
        if (set_fmt(nodes[i], 0, w, h) || set_fmt(nodes[i], 1, w, h))
            return -1;
    return 0;
}

/* Both cameras' links off first: a link to the ISPIF that's still on from the other camera is "busy". */
static void unlink_all(void)
{
    static const char *const phys[] = { "msm_csiphy0", "msm_csiphy1" }, *const csids[] = { "msm_csid0", "msm_csid1" };
    for (int i = 0; i < 2; i++)
        for (int j = 0; j < 2; j++) {
            struct media_entity_desc a, b;
            struct media_link_desc l;
            if (find(phys[i], &a) || find(csids[j], &b))
                continue;
            memset(&l, 0, sizeof(l));
            l.source.entity = a.id;
            l.source.index = 1;
            l.source.flags = MEDIA_PAD_FL_SOURCE;
            l.sink.entity = b.id;
            l.sink.index = 0;
            l.sink.flags = MEDIA_PAD_FL_SINK;
            ioctl(media, MEDIA_IOC_SETUP_LINK, &l);
        }
    for (int j = 0; j < 2; j++) {
        struct media_entity_desc a, b;
        struct media_link_desc l;
        if (find(csids[j], &a) || find("msm_ispif0", &b))
            continue;
        memset(&l, 0, sizeof(l));
        l.source.entity = a.id;
        l.source.index = 1;
        l.source.flags = MEDIA_PAD_FL_SOURCE;
        l.sink.entity = b.id;
        l.sink.index = 0;
        l.sink.flags = MEDIA_PAD_FL_SINK;
        ioctl(media, MEDIA_IOC_SETUP_LINK, &l);
    }
}

static int pipeline(void)
{
    struct media_entity_desc e;
    unlink_all();
    if (link_pads(s_phy, 1, s_csid, 0) || link_pads(s_csid, 1, "msm_ispif0", 0) ||
        link_pads("msm_ispif0", 1, "msm_vfe0_rdi0", 0) || set_sizes(live_w, live_h))
        return -1;
    if (find(s_name, &e) || (sensor = open_node(&e)) < 0)
        return -1;
    if (find("msm_vfe0_video0", &e) || (video = open_node(&e)) < 0)
        return -1;
    return 0;
}

/* The video node's format and buffers for a size; 0, or -1. */
static int buffers_up(int w, int h, unsigned want)
{
    struct v4l2_format f;
    memset(&f, 0, sizeof(f));
    f.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    f.fmt.pix_mp.width = (uint32_t)w;
    f.fmt.pix_mp.height = (uint32_t)h;
    f.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_SRGGB10P;
    f.fmt.pix_mp.field = V4L2_FIELD_NONE;
    f.fmt.pix_mp.num_planes = 1;
    if (ioctl(video, VIDIOC_S_FMT, &f)) {
        k86_logf("camera: %dx%d: S_FMT failed, errno %d", (uint32_t)w, (uint32_t)h, errno);
        return -1;
    }
    stride = (int)f.fmt.pix_mp.plane_fmt[0].bytesperline;

    struct v4l2_requestbuffers req;
    memset(&req, 0, sizeof(req));
    req.count = want;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    req.memory = V4L2_MEMORY_MMAP;
    if (ioctl(video, VIDIOC_REQBUFS, &req) || req.count > BUFS || !req.count) {
        k86_logf("camera: %dx%d: REQBUFS failed, errno %d, count %d", (uint32_t)w, (uint32_t)h, errno, (int)req.count);
        return -1;
    }
    nbufs = req.count;
    for (unsigned i = 0; i < nbufs; i++) {
        struct v4l2_buffer b;
        struct v4l2_plane p;
        memset(&b, 0, sizeof(b));
        memset(&p, 0, sizeof(p));
        b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        b.memory = V4L2_MEMORY_MMAP;
        b.index = i;
        b.m.planes = &p;
        b.length = 1;
        if (ioctl(video, VIDIOC_QUERYBUF, &b))
            return -1;
        mem_len[i] = p.length;
        mem[i] = mmap(0, p.length, PROT_READ, MAP_SHARED, video, p.m.mem_offset);
        if (mem[i] == MAP_FAILED || ioctl(video, VIDIOC_QBUF, &b))
            return -1;
    }
    return 0;
}

/* Stops the stream and gives the buffers back. */
static void buffers_down(void)
{
    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    struct v4l2_requestbuffers req;
    ioctl(video, VIDIOC_STREAMOFF, &type);
    for (unsigned i = 0; i < nbufs; i++)
        munmap(mem[i], mem_len[i]);
    nbufs = 0;
    memset(&req, 0, sizeof(req));
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    req.memory = V4L2_MEMORY_MMAP;
    ioctl(video, VIDIOC_REQBUFS, &req);          /* count 0: free them */
}

static int stream_up(int w, int h, unsigned want, int exp, int gain)
{
    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    if (set_sizes(w, h)) {
        k86_logf("camera: %dx%d: the pipeline wouldn't take the size, errno %d", (uint32_t)w, (uint32_t)h, errno);
        return -1;
    }
    if (buffers_up(w, h, want))
        return -1;
    set_ctrl(V4L2_CID_EXPOSURE, exp);
    set_ctrl(V4L2_CID_ANALOGUE_GAIN, gain);
    int err = ioctl(video, VIDIOC_STREAMON, &type);
    if (err)
        k86_logf("camera: %dx%d: STREAMON failed, errno %d", (uint32_t)w, (uint32_t)h, errno);
    return err;
}

/* The front camera's turn: "x y" in /extras/camera-front.cfg (1 flips that way; none is upright). */
static void load_flips(void)
{
    FILE *f = fopen("/extras/camera-front.cfg", "r");
    int x, y;
    if (f) {
        if (fscanf(f, "%d %d", &x, &y) == 2) {
            flip_x = x != 0;
            flip_y = y != 0;
        }
        fclose(f);
    }
}

static void choose(int want_front)
{
    front = want_front;
    s_name = front ? "s5k5e3yx" : "imx135";
    s_phy = front ? "msm_csiphy1" : "msm_csiphy0";
    s_csid = front ? "msm_csid1" : "msm_csid0";
    live_w = front ? 2576 : CAM_W;
    live_h = front ? 1932 : CAM_H;
    if (front)
        load_flips();
}

int cam_front(void)
{
    return front;
}

int cam_open(void)
{
    for (int i = 0; i < 4 && media < 0; i++) {
        char path[32];
        struct media_entity_desc e;
        snprintf(path, sizeof(path), "/dev/media%d", i);
        media = open(path, O_RDWR);
        if (media >= 0 && find(s_name, &e)) {
            close(media);
            media = -1;
        }
    }
    if (media < 0 || pipeline())
        return -1;
    return stream_up(live_w, live_h, BUFS, exposure, gain_code);
}

/* The other camera. 0 when it's streaming; -1 if it wouldn't come up (the first one is back then). */
int cam_switch(void)
{
    int was = front;
    buffers_down();
    if (sensor >= 0)
        close(sensor);
    if (video >= 0)
        close(video);
    sensor = video = -1;
    paused = 0;
    choose(!was);
    if (!pipeline() && !stream_up(live_w, live_h, BUFS, exposure, gain_code))
        return 0;
    k86_logf("camera: the %s camera wouldn't come up", front ? "front" : "rear");
    buffers_down();
    if (sensor >= 0)
        close(sensor);
    if (video >= 0)
        close(video);
    sensor = video = -1;
    choose(was);
    if (pipeline() || stream_up(live_w, live_h, BUFS, exposure, gain_code))
        k86_log("camera: the first camera didn't come back either");
    return -1;
}

/* Not in front: stop the sensor and give the buffers back, so the camera costs the CPU nothing; cam_resume() brings
 * the live picture back (the lens needs focus_restore() after it, as after any restream). */

void cam_pause(void)
{
    if (!paused && video >= 0) {
        buffers_down();
        paused = 1;
    }
}

int cam_resume(void)
{
    if (!paused)
        return 0;
    paused = 0;
    return stream_up(live_w, live_h, BUFS, exposure, gain_code);
}

void cam_close(void)
{
    if (video >= 0) {
        buffers_down();
        close(video);
    }
    if (sensor >= 0)
        close(sensor);
    if (media >= 0)
        close(media);
    video = sensor = media = -1;
}

int cam_stride(void)
{
    return front ? norm_stride : stride;
}

/* --- the front sensor's frames, made into the rear's kind --------------------------------------------- */

static inline int rd10(const uint8_t *row, int x)
{
    const uint8_t *g = row + (x >> 2) * 5;
    return g[x & 3] << 2 | (g[4] >> ((x & 3) * 2) & 3);
}

/* The sensor's frame (green first, GRBG, scw x sch cells of 2 x 2 pixels) as RGGB, dw x dh pixels (dw a
 * multiple of 4): each cell of the result takes the colours of one cell of the source, scaled (scw and dw / 2
 * differ for the live picture) and flipped across and/or down as asked. Cells move whole, so the colours stay
 * right. */
static void normalise(const uint8_t *src, int sstride, int ox, int oy, int scw, int sch, uint8_t *dst, int dstride,
                      int dw, int dh, int fx, int fy)
{
    int dcw = dw / 2, dch = dh / 2;
    for (int cy = 0; cy < dch; cy++) {
        int sy = cy * sch / dch;
        if (fy)
            sy = sch - 1 - sy;
        sy += oy;
        const uint8_t *r0 = src + (size_t)(2 * sy) * (size_t)sstride, *r1 = r0 + sstride;
        uint8_t *d0 = dst + (size_t)(2 * cy) * (size_t)dstride, *d1 = d0 + dstride;
        for (int cx = 0; cx < dcw; cx += 2, d0 += 5, d1 += 5) {
            int a[2][4];                    /* [row][column] of the four pixels this group makes */
            for (int k = 0; k < 2; k++) {
                int sx = (cx + k) * scw / dcw;
                if (fx)
                    sx = scw - 1 - sx;
                int x0 = 2 * (sx + ox);
                a[0][2 * k] = rd10(r0, x0 + 1);         /* red */
                a[0][2 * k + 1] = rd10(r0, x0);         /* green, in the red row */
                a[1][2 * k] = rd10(r1, x0 + 1);         /* green, in the blue row */
                a[1][2 * k + 1] = rd10(r1, x0);         /* blue */
            }
            for (int j = 0; j < 2; j++) {
                uint8_t *d = j ? d1 : d0;
                d[0] = (uint8_t)(a[j][0] >> 2);
                d[1] = (uint8_t)(a[j][1] >> 2);
                d[2] = (uint8_t)(a[j][2] >> 2);
                d[3] = (uint8_t)(a[j][3] >> 2);
                d[4] = (uint8_t)((a[j][0] & 3) | (a[j][1] & 3) << 2 | (a[j][2] & 3) << 4 | (a[j][3] & 3) << 6);
            }
        }
    }
}

const uint8_t *cam_next(int *slot)
{
    struct v4l2_buffer b;
    struct v4l2_plane p;
    memset(&b, 0, sizeof(b));
    memset(&p, 0, sizeof(p));
    b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    b.memory = V4L2_MEMORY_MMAP;
    b.m.planes = &p;
    b.length = 1;
    if (ioctl(video, VIDIOC_DQBUF, &b))
        return 0;
    if (front) {
        if (!norm) {
            norm_stride = CAM_W / 4 * 5;
            norm = malloc((size_t)norm_stride * CAM_H);
        }
        if (norm)
            normalise(mem[b.index], stride, 0, FRONT_OY, FRONT_CELLS_W, FRONT_CELLS_H, norm, norm_stride, CAM_W, CAM_H,
                      flip_x, flip_y);
        ioctl(video, VIDIOC_QBUF, &b);          /* given back at once: the app works on the copy */
        *slot = 0;                              /* nothing to give back: cam_release() ignores it */
        return norm;
    }
    *slot = (int)b.index;
    return mem[b.index];
}

void cam_release(int slot)
{
    if (front)
        return;
    struct v4l2_buffer b;
    struct v4l2_plane p;
    memset(&b, 0, sizeof(b));
    memset(&p, 0, sizeof(p));
    b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    b.memory = V4L2_MEMORY_MMAP;
    b.index = (uint32_t)slot;
    b.m.planes = &p;
    b.length = 1;
    ioctl(video, VIDIOC_QBUF, &b);
}

/* A photo-sized frame: the live picture stops, the sensor goes to the photo mode for a few
 * frames (the first ones are dark or half-exposed, and the lens, which lost its power with
 * the stream, needs to get back to where it was), one is copied, and the live picture
 * comes back. The exposure was found for the live mode; a photo-mode line is longer, so
 * it takes fewer of them for the same time. Returns a malloc'd copy (free it), its size
 * and row pitch; NULL if the photo mode didn't come up (the live picture comes back all
 * the same). */
#define STILL_SKIP 5
/* The live mode's line length (4572) over a photo mode's: 5120 for 2080x1170, 9728 for 4160x2340. */
static const struct { int w, h, num, den; } still_modes[] = {
    { STILL_W, STILL_H, 4572, 9728 },          /* the full 4160x2340 */
    { 2080, 1170, 4572, 5120 },                /* every 2 x 2 made one: if the big one won't come up */
};

/* The front camera's photo: the next frame of the stream, which is already in the photo mode: the middle 16:9
 * of it (2572 across, whole groups of 4 pixels) as RGGB, flipped. No restart, so no wait. */
static uint8_t *front_still(int *w, int *h, int *row_bytes)
{
    uint8_t *copy = NULL;
    for (int tries = 0; tries < 6 && !copy; tries++) {
        struct pollfd pf = { video, POLLIN, 0 };
        struct v4l2_buffer b;
        struct v4l2_plane p;
        if (poll(&pf, 1, 1000) <= 0) {
            k86_log("camera: front photo: no frame");
            break;
        }
        memset(&b, 0, sizeof(b));
        memset(&p, 0, sizeof(p));
        b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        b.memory = V4L2_MEMORY_MMAP;
        b.m.planes = &p;
        b.length = 1;
        if (ioctl(video, VIDIOC_DQBUF, &b))
            continue;
        if (tries >= 1) {                       /* the first may be older than the shutter */
            int dw = 2572, dh = FRONT_CELLS_H * 2;
            *row_bytes = dw / 4 * 5;
            copy = malloc((size_t)*row_bytes * (size_t)dh);
            if (copy)
                normalise(mem[b.index], stride, 0, FRONT_OY, dw / 2, FRONT_CELLS_H, copy, *row_bytes, dw, dh, flip_x,
                          flip_y);
            *w = dw;
            *h = dh;
        }
        ioctl(video, VIDIOC_QBUF, &b);
    }
    return copy;
}

uint8_t *cam_still(int *w, int *h, int *row_bytes, void (*settled)(void))
{
    uint8_t *copy = NULL;
    int sw = STILL_W, sh = STILL_H, got = 0, e, up = -1;
    if (front)
        return front_still(w, h, row_bytes);
    buffers_down();
    for (unsigned m = 0; m < sizeof(still_modes) / sizeof(still_modes[0]) && up; m++) {
        sw = still_modes[m].w;
        sh = still_modes[m].h;
        e = exposure * still_modes[m].num / still_modes[m].den;
        e = e < 4 ? 4 : e;
        up = stream_up(sw, sh, BUFS, e, gain_code);
        if (up) {
            k86_logf("camera: photo mode %dx%d didn't come up", (uint32_t)sw, (uint32_t)sh);
            buffers_down();
        }
    }
    if (up == 0) {
        if (settled)
            settled();                         /* the lens back to its place */
        for (int tries = 0; tries < 40 && !copy; tries++) {
            struct pollfd pf = { video, POLLIN, 0 };
            struct v4l2_buffer b;
            struct v4l2_plane p;
            if (poll(&pf, 1, 1000) <= 0) {
                k86_logf("camera: photo mode: no frame after %d", (uint32_t)got);
                break;
            }
            memset(&b, 0, sizeof(b));
            memset(&p, 0, sizeof(p));
            b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
            b.memory = V4L2_MEMORY_MMAP;
            b.m.planes = &p;
            b.length = 1;
            if (ioctl(video, VIDIOC_DQBUF, &b))
                continue;
            if (++got > STILL_SKIP) {
                k86_logf("camera: photo mode: frame %d of %dx%d, rows %d bytes", (uint32_t)got, (uint32_t)sw, (uint32_t)sh, (uint32_t)stride);
                size_t len = (size_t)stride * (size_t)sh;
                copy = malloc(len);
                if (copy)
                    memcpy(copy, mem[b.index], len);
                *w = sw;
                *h = sh;
                *row_bytes = stride;
            }
            ioctl(video, VIDIOC_QBUF, &b);
        }
    }
    buffers_down();
    if (stream_up(live_w, live_h, BUFS, exposure, gain_code))
        k86_log("camera: the live picture didn't come back");
    return copy;
}

static float gain_of(int code)
{
    return 256.0f / (float)(256 - code);
}

void cam_expose(const uint8_t *frame)
{
    long sum = 0, bright = 0, n = 0;
    for (int y = 1; y < CAM_H; y += 8) {
        const uint8_t *row = frame + (size_t)y * (size_t)cam_stride();
        for (int x = 0; x < CAM_W; x += 8) {    /* odd rows start on green */
            int v = row[x / 4 * 5];
            sum += v;
            bright += v > 240;
            n++;
        }
    }
    float mean = ((float)sum / (float)n * 4 - CAM_BLACK) / (CAM_WHITE - CAM_BLACK);
    float clipped = (float)bright / (float)n;
    float ratio = TARGET / (mean > 0.002f ? mean : 0.002f);
    if (clipped > 0.03f && ratio > 0.7f)
        ratio = 0.7f;                           /* keep the highlights */
    if (ratio > 0.87f && ratio < 1.15f)
        return;
    ratio = ratio > 2 ? 2 : ratio < 0.5f ? 0.5f : ratio;
    float want = (float)exposure * gain_of(gain_code) * ratio;
    int emax = front ? FRONT_EXPOSURE_MAX : EXPOSURE_MAX;
    int e = want > emax ? emax : want < 4 ? 4 : (int)want;
    float g = want / (float)e;
    int code = g <= 1 ? 0 : (int)(256 - 256 / g);
    code = code > GAIN_CODE_MAX ? GAIN_CODE_MAX : code;
    if (e != exposure || code != gain_code)
        k86_logf("camera: exposure %d -> %d, gain code %d, mean %d/1000, clipped %d/1000", (uint32_t)exposure, (uint32_t)e,
                 code, (int)(mean * 1000), (int)(clipped * 1000));
    if (e != exposure) {
        exposure = e;
        set_ctrl(V4L2_CID_EXPOSURE, e);
    }
    if (code != gain_code) {
        gain_code = code;
        set_ctrl(V4L2_CID_ANALOGUE_GAIN, code);
    }
}
