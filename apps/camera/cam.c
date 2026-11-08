/* Camera: the pipeline imx135 -> csiphy0 -> csid0 -> ispif0 -> vfe0_rdi0, set up
 * through the media device, with frames taken as they come and auto-exposure on
 * the sensor's own controls. Nothing here waits: the app polls from its timer. */
#include "cam.h"
#include "sdk/kern86.h"

#include <fcntl.h>
#include <linux/media.h>
#include <linux/v4l2-subdev.h>
#include <linux/videodev2.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#define CODE MEDIA_BUS_FMT_SRGGB10_1X10
#define BUFS 4
#define EXPOSURE_MAX 2620
#define GAIN_CODE_MAX 192               /* 4x: past that it's mostly noise */
#define TARGET 0.14f                    /* the mean brightness we aim for, linear */

static int media = -1, sensor = -1, video = -1, stride;
static void *mem[BUFS];
static uint32_t mem_len[BUFS];
static unsigned nbufs;
static int exposure = 1600, gain_code = 128;

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

static int set_fmt(const char *name, unsigned pad)
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
    f.format.width = CAM_W;
    f.format.height = CAM_H;
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

static int pipeline(void)
{
    static const char *const nodes[] = { "msm_csiphy0", "msm_csid0", "msm_ispif0", "msm_vfe0_rdi0" };
    struct media_entity_desc e;
    if (link_pads("msm_csiphy0", 1, "msm_csid0", 0) || link_pads("msm_csid0", 1, "msm_ispif0", 0) ||
        link_pads("msm_ispif0", 1, "msm_vfe0_rdi0", 0) || set_fmt("imx135", 0))
        return -1;
    for (unsigned i = 0; i < 4; i++)
        if (set_fmt(nodes[i], 0) || set_fmt(nodes[i], 1))
            return -1;
    if (find("imx135", &e) || (sensor = open_node(&e)) < 0)
        return -1;
    if (find("msm_vfe0_video0", &e) || (video = open_node(&e)) < 0)
        return -1;
    return 0;
}

int cam_open(void)
{
    for (int i = 0; i < 4 && media < 0; i++) {
        char path[32];
        struct media_entity_desc e;
        snprintf(path, sizeof(path), "/dev/media%d", i);
        media = open(path, O_RDWR);
        if (media >= 0 && find("imx135", &e)) {
            close(media);
            media = -1;
        }
    }
    if (media < 0 || pipeline())
        return -1;

    struct v4l2_format f;
    memset(&f, 0, sizeof(f));
    f.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    f.fmt.pix_mp.width = CAM_W;
    f.fmt.pix_mp.height = CAM_H;
    f.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_SRGGB10P;
    f.fmt.pix_mp.field = V4L2_FIELD_NONE;
    f.fmt.pix_mp.num_planes = 1;
    if (ioctl(video, VIDIOC_S_FMT, &f))
        return -1;
    stride = (int)f.fmt.pix_mp.plane_fmt[0].bytesperline;

    struct v4l2_requestbuffers req;
    memset(&req, 0, sizeof(req));
    req.count = BUFS;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    req.memory = V4L2_MEMORY_MMAP;
    if (ioctl(video, VIDIOC_REQBUFS, &req) || req.count > BUFS)
        return -1;
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
    set_ctrl(V4L2_CID_EXPOSURE, exposure);
    set_ctrl(V4L2_CID_ANALOGUE_GAIN, gain_code);
    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    return ioctl(video, VIDIOC_STREAMON, &type);
}

void cam_close(void)
{
    if (video >= 0) {
        int type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        ioctl(video, VIDIOC_STREAMOFF, &type);
        for (unsigned i = 0; i < nbufs; i++)
            munmap(mem[i], mem_len[i]);
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
    return stride;
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
    *slot = (int)b.index;
    return mem[b.index];
}

void cam_release(int slot)
{
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

static float gain_of(int code)
{
    return 256.0f / (float)(256 - code);
}

void cam_expose(const uint8_t *frame)
{
    long sum = 0, bright = 0, n = 0;
    for (int y = 1; y < CAM_H; y += 8) {
        const uint8_t *row = frame + (size_t)y * (size_t)stride;
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
    int e = want > EXPOSURE_MAX ? EXPOSURE_MAX : want < 4 ? 4 : (int)want;
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
