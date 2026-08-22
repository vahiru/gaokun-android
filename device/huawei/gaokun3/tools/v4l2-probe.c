/*
 * v4l2-probe -- 列出 /dev/videoN 两个方向上支持的像素格式。
 *
 * 为什么需要它：Venus 的编解码能力是固件在 HFI sys_init 时报上来的，
 * 内核既不打日志也不放进 sysfs（/sys/kernel/debug/venus 里只有 fw_level）。
 * 而 v4l2_codec2 的每个组件都由一条 ro.vendor.v4l2_codec2.*.supported.*
 * 属性把守，设错了要么少了硬解、要么声明出根本不存在的编解码器。
 * 所以先实测再设属性。
 *
 * ⚠️ 源码里刻意不写任何反斜杠转义（本仓多次被中间层折叠坑过），
 *    换行一律用 puts。
 *
 * 交叉编译：aarch64-linux-gnu-gcc -static -O2 -o v4l2-probe v4l2-probe.c
 */
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/videodev2.h>

static void try_sizes(int fd, unsigned type, unsigned pixfmt);

static void dump(int fd, unsigned type, const char *label)
{
    struct v4l2_fmtdesc f;
    unsigned i;

    printf("  %s:", label);
    puts("");
    for (i = 0; ; i++) {
        memset(&f, 0, sizeof(f));
        f.index = i;
        f.type = type;
        if (ioctl(fd, VIDIOC_ENUM_FMT, &f) != 0)
            break;
        printf("    %c%c%c%c  %s",
               (char)(f.pixelformat & 0xff),
               (char)((f.pixelformat >> 8) & 0xff),
               (char)((f.pixelformat >> 16) & 0xff),
               (char)((f.pixelformat >> 24) & 0xff),
               f.description);
        if (f.flags & V4L2_FMT_FLAG_COMPRESSED)
            printf("   [compressed]");
        /* 分辨率范围也要实测：xml 里写大了，应用会把硬件啃不动的流交给它 */
        {
            struct v4l2_frmsizeenum fs;
            memset(&fs, 0, sizeof(fs));
            fs.index = 0;
            fs.pixel_format = f.pixelformat;
            if (ioctl(fd, VIDIOC_ENUM_FRAMESIZES, &fs) == 0) {
                if (fs.type == V4L2_FRMSIZE_TYPE_STEPWISE ||
                    fs.type == V4L2_FRMSIZE_TYPE_CONTINUOUS)
                    printf("   %ux%u..%ux%u step %ux%u",
                           fs.stepwise.min_width, fs.stepwise.min_height,
                           fs.stepwise.max_width, fs.stepwise.max_height,
                           fs.stepwise.step_width, fs.stepwise.step_height);
                else
                    printf("   %ux%u", fs.discrete.width, fs.discrete.height);
            }
        }
        puts("");
        if (f.flags & V4L2_FMT_FLAG_COMPRESSED)
            try_sizes(fd, type, f.pixelformat);
    }
    if (i == 0)
        puts("    (无)");
}

/*
 * ENUM_FRAMESIZES 报的是驱动的【总包络】（本机 128x128..8192x8192），
 * 不是每个编解码的真实上限。让驱动自己钳位才是实测：TRY_FMT 一个大尺寸，
 * 看它改回什么。
 */
static void try_sizes(int fd, unsigned type, unsigned pixfmt)
{
    static const unsigned probes[][2] = {
        {1920, 1080}, {3840, 2160}, {4096, 2160}, {7680, 4320}, {8192, 8192}
    };
    unsigned i;

    for (i = 0; i < sizeof(probes) / sizeof(probes[0]); i++) {
        struct v4l2_format fmt;
        memset(&fmt, 0, sizeof(fmt));
        fmt.type = type;
        fmt.fmt.pix_mp.pixelformat = pixfmt;
        fmt.fmt.pix_mp.width = probes[i][0];
        fmt.fmt.pix_mp.height = probes[i][1];
        fmt.fmt.pix_mp.num_planes = 1;
        if (ioctl(fd, VIDIOC_TRY_FMT, &fmt) != 0)
            printf("      %ux%u -> 失败", probes[i][0], probes[i][1]);
        else
            printf("      %ux%u -> %ux%u", probes[i][0], probes[i][1],
                   fmt.fmt.pix_mp.width, fmt.fmt.pix_mp.height);
        puts("");
    }
}

int main(int argc, char **argv)
{
    int i;

    for (i = 1; i < argc; i++) {
        struct v4l2_capability cap;
        int fd = open(argv[i], O_RDWR);

        printf("== %s ==", argv[i]);
        puts("");
        if (fd < 0) {
            perror("    open");
            continue;
        }
        memset(&cap, 0, sizeof(cap));
        if (ioctl(fd, VIDIOC_QUERYCAP, &cap) == 0) {
            printf("  driver=%s card=%s bus=%s", cap.driver, cap.card, cap.bus_info);
            puts("");
        }
        /* 解码器：OUTPUT 是压缩流进、CAPTURE 是裸帧出；编码器相反 */
        dump(fd, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE, "OUTPUT (送进去的格式)");
        dump(fd, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, "CAPTURE (取出来的格式)");
        close(fd);
    }
    return 0;
}
