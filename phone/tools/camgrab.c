/* LiteKern Go, phone: one raw frame from the rear camera, to prove the pipeline.
 *
 *   camgrab OUT.raw [frames to skip]
 *
 * Links imx135 -> csiphy0 -> csid0 -> ispif0 -> vfe0_rdi0, sets RAW10 1280x720 on
 * each, and writes a packed SBGGR10 frame. Prints what the kernel said about it. */
#include <errno.h>
#include <fcntl.h>
#include <linux/media.h>
#include <linux/v4l2-subdev.h>
#include <linux/videodev2.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/sysmacros.h>
#include <unistd.h>

#define W 1280
#define H 720
#define CODE MEDIA_BUS_FMT_SBGGR10_1X10
#define BUFS 4

static int media;

static void die(const char *what)
{
    fprintf(stderr, "camgrab: %s: %s\n", what, strerror(errno));
    exit(1);
}

static int find(const char *prefix, struct media_entity_desc *out)
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
            return 0;
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

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: camgrab OUT.raw [frames to skip]\n");
        return 2;
    }
    int skip = argc > 2 ? atoi(argv[2]) : 8;
    const char *node = getenv("CAM_MEDIA");     /* a reloaded driver gets a new number */
    media = open(node ? node : "/dev/media0", O_RDWR);
    if (media < 0)
        die("/dev/media0");
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

    struct media_entity_desc ve;
    find("msm_vfe0_video0", &ve);
    int v = open_node(&ve);
    struct v4l2_format f;
    memset(&f, 0, sizeof(f));
    f.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    f.fmt.pix_mp.width = W;
    f.fmt.pix_mp.height = H;
    f.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_SBGGR10P;
    f.fmt.pix_mp.field = V4L2_FIELD_NONE;
    f.fmt.pix_mp.num_planes = 1;
    if (ioctl(v, VIDIOC_S_FMT, &f))
        die("VIDIOC_S_FMT");
    printf("format %ux%u stride %u size %u\n", f.fmt.pix_mp.width, f.fmt.pix_mp.height,
           f.fmt.pix_mp.plane_fmt[0].bytesperline, f.fmt.pix_mp.plane_fmt[0].sizeimage);

    struct v4l2_requestbuffers req;
    memset(&req, 0, sizeof(req));
    req.count = BUFS;
    req.type = f.type;
    req.memory = V4L2_MEMORY_MMAP;
    if (ioctl(v, VIDIOC_REQBUFS, &req))
        die("VIDIOC_REQBUFS");
    void *mem[BUFS];
    size_t len[BUFS];
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
        len[i] = p.length;
        mem[i] = mmap(0, p.length, PROT_READ | PROT_WRITE, MAP_SHARED, v, p.m.mem_offset);
        if (mem[i] == MAP_FAILED)
            die("mmap");
        if (ioctl(v, VIDIOC_QBUF, &b))
            die("VIDIOC_QBUF");
    }
    int type = f.type;
    if (ioctl(v, VIDIOC_STREAMON, &type))
        die("VIDIOC_STREAMON");
    printf("streaming\n");
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
        printf("frame %d: seq %u, %u bytes, flags %x\n", n, b.sequence, p.bytesused, b.flags);
        if (n >= skip) {
            FILE *o = fopen(argv[1], "wb");
            if (!o)
                die(argv[1]);
            fwrite(mem[b.index], 1, p.bytesused ? p.bytesused : len[b.index], o);
            fclose(o);
            break;
        }
        if (ioctl(v, VIDIOC_QBUF, &b))
            die("VIDIOC_QBUF");
    }
    ioctl(v, VIDIOC_STREAMOFF, &type);
    printf("saved %s\n", argv[1]);
    return 0;
}
