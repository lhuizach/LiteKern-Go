/* LiteKern Go, phone: a photo from the rear camera.
 *
 *   camgrab NAME
 *
 * Links imx135 -> csiphy0 -> csid0 -> ispif0 -> vfe0_rdi0, sets RAW10 1280x720 on
 * each, finds the exposure, and writes NAME.raw (packed SRGGB10) and NAME.ppm: each
 * 2x2 Bayer cell a pixel, with the black level off, lens shading evened out, grey
 * world white balance and a gamma. CAM_MEDIA names the media device, for a reloaded
 * driver. Prints what it did. */
#include <errno.h>
#include <fcntl.h>
#include <linux/media.h>
#include <linux/v4l2-subdev.h>
#include <linux/videodev2.h>
#include <math.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#define W 1280
#define H 720
#define CODE MEDIA_BUS_FMT_SRGGB10_1X10
#define BUFS 4
#define BLACK 64
#define WHITE 1023
#define EXPOSURE_MAX 2620
#define GAIN_CODE_MAX 192               /* 4x: past that it's mostly noise */
#define TARGET 0.14f                    /* the mean brightness we aim for, linear */
#define SHADING 0.9f                    /* the brightening at the corners */
#define SATURATION 1.35f

static int media;

static void die(const char *what)
{
    fprintf(stderr, "camgrab: %s: %s\n", what, strerror(errno));
    exit(1);
}

static void find(const char *prefix, struct media_entity_desc *out)
{
    __u32 id = MEDIA_ENT_ID_FLAG_NEXT;
    for (;;) {
        struct media_entity_desc e;
        memset(&e, 0, sizeof(e));
        e.id = id;
        if (ioctl(media, MEDIA_IOC_ENUM_ENTITIES, &e))
            break;
        if (!strncmp(e.name, prefix, strlen(prefix))) {
            *out = e;
            return;
        }
        id = e.id | MEDIA_ENT_ID_FLAG_NEXT;
    }
    fprintf(stderr, "camgrab: no entity %s\n", prefix);
    exit(1);
}

static int open_node(const struct media_entity_desc *e)
{
    char path[64], line[128], dev[64] = "";
    snprintf(path, sizeof(path), "/sys/dev/char/%u:%u/uevent", e->dev.major, e->dev.minor);
    FILE *f = fopen(path, "r");
    if (!f)
        die(path);
    while (fgets(line, sizeof(line), f))
        if (!strncmp(line, "DEVNAME=", 8)) {
            line[strcspn(line, "\n")] = 0;
            snprintf(dev, sizeof(dev), "/dev/%.50s", line + 8);
        }
    fclose(f);
    int fd = open(dev, O_RDWR);
    if (fd < 0)
        die(dev);
    return fd;
}

static void set_fmt(const char *name, int pad)
{
    struct media_entity_desc e;
    find(name, &e);
    int fd = open_node(&e);
    struct v4l2_subdev_format f;
    memset(&f, 0, sizeof(f));
    f.which = V4L2_SUBDEV_FORMAT_ACTIVE;
    f.pad = pad;
    f.format.width = W;
    f.format.height = H;
    f.format.code = CODE;
    f.format.field = V4L2_FIELD_NONE;
    if (ioctl(fd, VIDIOC_SUBDEV_S_FMT, &f))
        die(name);
    close(fd);
}

static void link_pads(const char *from, int from_pad, const char *to, int to_pad)
{
    struct media_entity_desc a, b;
    struct media_link_desc l;
    find(from, &a);
    find(to, &b);
    memset(&l, 0, sizeof(l));
    l.source.entity = a.id;
    l.source.index = from_pad;
    l.source.flags = MEDIA_PAD_FL_SOURCE;
    l.sink.entity = b.id;
    l.sink.index = to_pad;
    l.sink.flags = MEDIA_PAD_FL_SINK;
    l.flags = MEDIA_LNK_FL_ENABLED;
    if (ioctl(media, MEDIA_IOC_SETUP_LINK, &l))
        die(to);
}

/* --- exposure --------------------------------------------------------------- */

static int sensor, exposure = 1600, gain_code = 128;

static void set_ctrl(uint32_t id, int value)
{
    struct v4l2_control c = { id, value };
    if (ioctl(sensor, VIDIOC_S_CTRL, &c))
        die("VIDIOC_S_CTRL");
}

/* Top 8 bits of a 10-bit pixel: packed RAW10 keeps them in four of every five bytes. */
static inline int pixel8(const uint8_t *row, int x)
{
    return row[x / 4 * 5 + x % 4];
}

/* How bright a frame is, from its greens, and how much of it has blown out. */
static void measure(const uint8_t *f, int stride, float *mean, float *clipped)
{
    long sum = 0, bright = 0, n = 0;
    for (int y = 1; y < H; y += 8) {
        const uint8_t *row = f + (size_t)y * stride;
        for (int x = 0; x < W; x += 8) {        /* odd rows start on green */
            int v = pixel8(row, x);
            sum += v;
            bright += v > 240;
            n++;
        }
    }
    *mean = ((float)sum / n * 4 - BLACK) / (WHITE - BLACK);
    *clipped = (float)bright / n;
}

static float gain_of(int code)
{
    return 256.0f / (256 - code);
}

/* One step towards TARGET. 1 when it's there, or can go no further. */
static int adjust(float mean, float clipped)
{
    float total = exposure * gain_of(gain_code), ratio = TARGET / (mean > 0.002f ? mean : 0.002f);
    if (clipped > 0.03f && ratio > 0.7f)
        ratio = 0.7f;                           /* keep the highlights */
    if (ratio > 0.87f && ratio < 1.15f)
        return 1;
    ratio = ratio > 4 ? 4 : ratio < 0.25f ? 0.25f : ratio;
    float want = total * ratio;
    int e = want > EXPOSURE_MAX ? EXPOSURE_MAX : want < 4 ? 4 : (int)want;
    float g = want / e;
    int code = g <= 1 ? 0 : (int)(256 - 256 / g);
    code = code > GAIN_CODE_MAX ? GAIN_CODE_MAX : code;
    if (e == exposure && code == gain_code)
        return 1;
    exposure = e;
    gain_code = code;
    set_ctrl(V4L2_CID_EXPOSURE, e);
    set_ctrl(V4L2_CID_ANALOGUE_GAIN, code);
    return 0;
}

/* --- the picture ------------------------------------------------------------- */

static inline float pixel10(const uint8_t *row, int x)
{
    const uint8_t *p = row + x / 4 * 5;
    int v = p[x % 4] << 2 | (p[4] >> (x % 4 * 2) & 3);
    float f = (float)(v - BLACK) / (WHITE - BLACK);
    return f < 0 ? 0 : f;
}

static void save_ppm(const uint8_t *frame, int stride, const char *path)
{
    int w = W / 2, h = H / 2;
    float *rgb = malloc(sizeof(float) * 3 * w * h);
    uint8_t *out = malloc(3 * w);
    uint8_t curve[1024];
    double sr = 0, sg = 0, sb = 0;
    long n = 0;
    if (!rgb || !out)
        die("memory");
    for (int y = 0; y < h; y++) {
        const uint8_t *top = frame + (size_t)y * 2 * stride, *low = top + stride;
        float dy = (y - h / 2.0f) / (h / 2.0f);
        for (int x = 0; x < w; x++) {
            float dx = (x - w / 2.0f) / (w / 2.0f), r2 = (dx * dx + dy * dy) / 2;
            float shade = 1 + SHADING * r2, *p = rgb + (y * w + x) * 3;
            p[0] = pixel10(top, x * 2) * shade;
            p[1] = (pixel10(top, x * 2 + 1) + pixel10(low, x * 2)) / 2 * shade;
            p[2] = pixel10(low, x * 2 + 1) * shade;
            if (p[1] < 0.8f && p[1] > 0.01f) {      /* neither clipped nor just noise */
                sr += p[0];
                sg += p[1];
                sb += p[2];
                n++;
            }
        }
    }
    float rg = n && sr > 0 ? (float)(sg / sr) : 1, bg = n && sb > 0 ? (float)(sg / sb) : 1;
    printf("white balance: red x%.2f, blue x%.2f\n", rg, bg);
    for (int i = 0; i < 1024; i++)
        curve[i] = (uint8_t)(pow(i / 1023.0, 1 / 2.2) * 255 + 0.5);
    FILE *o = fopen(path, "wb");
    if (!o)
        die(path);
    fprintf(o, "P6\n%d %d\n255\n", w, h);
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            float *p = rgb + (y * w + x) * 3;
            float r = p[0] * rg, g = p[1], b = p[2] * bg, l = 0.3f * r + 0.59f * g + 0.11f * b;
            float c[3] = { l + (r - l) * SATURATION, l + (g - l) * SATURATION, l + (b - l) * SATURATION };
            for (int k = 0; k < 3; k++) {
                float v = c[k] < 0 ? 0 : c[k] > 1 ? 1 : c[k];
                out[x * 3 + k] = curve[(int)(v * 1023)];
            }
        }
        fwrite(out, 3, w, o);
    }
    fclose(o);
    free(rgb);
    free(out);
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: camgrab NAME\n");
        return 2;
    }
    const char *node = getenv("CAM_MEDIA");     /* a reloaded driver gets a new number */
    char path[256];
    media = open(node ? node : "/dev/media0", O_RDWR);
    if (media < 0)
        die("media device");
    link_pads("msm_csiphy0", 1, "msm_csid0", 0);
    link_pads("msm_csid0", 1, "msm_ispif0", 0);
    link_pads("msm_ispif0", 1, "msm_vfe0_rdi0", 0);
    set_fmt("imx135", 0);
    set_fmt("msm_csiphy0", 0);
    set_fmt("msm_csiphy0", 1);
    set_fmt("msm_csid0", 0);
    set_fmt("msm_csid0", 1);
    set_fmt("msm_ispif0", 0);
    set_fmt("msm_ispif0", 1);
    set_fmt("msm_vfe0_rdi0", 0);
    set_fmt("msm_vfe0_rdi0", 1);

    struct media_entity_desc se, ve;
    find("imx135", &se);
    sensor = open_node(&se);
    set_ctrl(V4L2_CID_EXPOSURE, exposure);
    set_ctrl(V4L2_CID_ANALOGUE_GAIN, gain_code);
    find("msm_vfe0_video0", &ve);
    int v = open_node(&ve);

    struct v4l2_format f;
    memset(&f, 0, sizeof(f));
    f.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    f.fmt.pix_mp.width = W;
    f.fmt.pix_mp.height = H;
    f.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_SRGGB10P;
    f.fmt.pix_mp.field = V4L2_FIELD_NONE;
    f.fmt.pix_mp.num_planes = 1;
    if (ioctl(v, VIDIOC_S_FMT, &f))
        die("VIDIOC_S_FMT");
    int stride = (int)f.fmt.pix_mp.plane_fmt[0].bytesperline;

    struct v4l2_requestbuffers req;
    memset(&req, 0, sizeof(req));
    req.count = BUFS;
    req.type = f.type;
    req.memory = V4L2_MEMORY_MMAP;
    if (ioctl(v, VIDIOC_REQBUFS, &req))
        die("VIDIOC_REQBUFS");
    void *mem[BUFS];
    for (unsigned i = 0; i < req.count; i++) {
        struct v4l2_buffer b;
        struct v4l2_plane p;
        memset(&b, 0, sizeof(b));
        memset(&p, 0, sizeof(p));
        b.type = f.type;
        b.memory = V4L2_MEMORY_MMAP;
        b.index = i;
        b.m.planes = &p;
        b.length = 1;
        if (ioctl(v, VIDIOC_QUERYBUF, &b))
            die("VIDIOC_QUERYBUF");
        mem[i] = mmap(0, p.length, PROT_READ | PROT_WRITE, MAP_SHARED, v, p.m.mem_offset);
        if (mem[i] == MAP_FAILED)
            die("mmap");
        if (ioctl(v, VIDIOC_QBUF, &b))
            die("VIDIOC_QBUF");
    }
    int type = f.type;
    if (ioctl(v, VIDIOC_STREAMON, &type))
        die("VIDIOC_STREAMON");

    int settled = 0, wait = 4;
    for (int n = 0;; n++) {
        struct pollfd pf = { v, POLLIN, 0 };
        if (poll(&pf, 1, 4000) <= 0) {
            printf("no frame after 4 s (%d so far)\n", n);
            ioctl(v, VIDIOC_STREAMOFF, &type);
            return 1;
        }
        struct v4l2_buffer b;
        struct v4l2_plane p;
        memset(&b, 0, sizeof(b));
        memset(&p, 0, sizeof(p));
        b.type = f.type;
        b.memory = V4L2_MEMORY_MMAP;
        b.m.planes = &p;
        b.length = 1;
        if (ioctl(v, VIDIOC_DQBUF, &b))
            die("VIDIOC_DQBUF");
        if (wait > 0) {
            wait--;                             /* a change takes a couple of frames */
        } else if (settled || n > 40) {
            snprintf(path, sizeof(path), "%s.raw", argv[1]);
            FILE *o = fopen(path, "wb");
            if (!o)
                die(path);
            fwrite(mem[b.index], 1, (size_t)stride * H, o);
            fclose(o);
            snprintf(path, sizeof(path), "%s.ppm", argv[1]);
            save_ppm(mem[b.index], stride, path);
            printf("saved %s, frame %d, exposure %d lines, gain %.1fx\n", path, n, exposure, gain_of(gain_code));
            break;
        } else {
            float mean, clipped;
            measure(mem[b.index], stride, &mean, &clipped);
            printf("frame %d: brightness %.3f, %.1f%% clipped, exposure %d, gain %.1fx\n", n, mean, clipped * 100,
                   exposure, gain_of(gain_code));
            settled = adjust(mean, clipped);
            wait = settled ? 1 : 3;
        }
        if (ioctl(v, VIDIOC_QBUF, &b))
            die("VIDIOC_QBUF");
    }
    ioctl(v, VIDIOC_STREAMOFF, &type);
    return 0;
}
