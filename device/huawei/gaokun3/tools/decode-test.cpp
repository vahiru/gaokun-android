/*
 * gaokun3-decode-test -- 用 NDK MediaCodec 真解一段视频，证明硬解在干活。
 *
 * 为什么需要它：本机【没有任何能放视频的应用】（gallery3d 是精简版，连
 * MovieActivity 都没有），而 "c2.v4l2.* 出现在 MediaCodecList" 只证明组件
 * 能被实例化，不证明它能解码。这个工具补上那一步。
 *
 * 输出走 AImageReader 提供的 ANativeWindow —— 那才是应用实际用的 surface
 * 路径；用 null surface 的字节缓冲模式测出来的结论不能代表真实场景。
 *
 * ⚠️ 源码里刻意不写反斜杠转义（本仓多次被中间层折叠坑过），换行一律 puts。
 *
 * 用法: gaokun3-decode-test <file.mp4> [期望的组件名]
 *   返回 0 = 解出至少 1 帧；非 0 = 失败（并打印卡在哪一步）
 */
#include <android/binder_process.h>
#include <media/NdkMediaExtractor.h>
#include <media/NdkMediaCodec.h>
#include <media/NdkMediaFormat.h>
#include <media/NdkImage.h>
#include <media/NdkImageReader.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>

static void line(void) { puts(""); }

int main(int argc, char **argv)
{
    if (argc < 2) {
        puts("用法: gaokun3-decode-test <file.mp4> [期望组件名]");
        return 2;
    }

    // ★ 必须先起 binder 线程池。Codec2 服务会【回调】本进程里的
    //   图形缓冲分配器（IGBA）去申请输出块；没有线程接收入站事务，
    //   分配就报 igba::allocate transaction failed: -129
    //   （EX_TRANSACTION_FAILED），然后 VideoFramePool 拿不到块、整个解码挂住。
    //   征兆很早就打在 logcat 里了："Thread Pool max thread count is 0"、
    //   "there are no threads (yet?) listening to incoming transactions"。
    ABinderProcess_setThreadPoolMaxThreadCount(2);
    ABinderProcess_startThreadPool();
    const char *path = argv[1];
    const char *want = (argc > 2) ? argv[2] : NULL;

    int fd = open(path, O_RDONLY);
    if (fd < 0) { printf("打不开 %s", path); line(); return 1; }

    AMediaExtractor *ex = AMediaExtractor_new();
    off_t size = lseek(fd, 0, SEEK_END);
    lseek(fd, 0, SEEK_SET);
    if (AMediaExtractor_setDataSourceFd(ex, fd, 0, size) != AMEDIA_OK) {
        puts("setDataSourceFd 失败"); return 1;
    }

    int nt = AMediaExtractor_getTrackCount(ex);
    int vt = -1;
    const char *mime = NULL;
    AMediaFormat *fmt = NULL;
    for (int i = 0; i < nt; i++) {
        AMediaFormat *f = AMediaExtractor_getTrackFormat(ex, i);
        const char *m = NULL;
        if (AMediaFormat_getString(f, AMEDIAFORMAT_KEY_MIME, &m) && m &&
            strncmp(m, "video/", 6) == 0) {
            vt = i; mime = m; fmt = f;
            break;
        }
        AMediaFormat_delete(f);
    }
    if (vt < 0) { puts("没找到视频轨"); return 1; }

    int32_t w = 0, h = 0;
    AMediaFormat_getInt32(fmt, AMEDIAFORMAT_KEY_WIDTH, &w);
    AMediaFormat_getInt32(fmt, AMEDIAFORMAT_KEY_HEIGHT, &h);
    printf("视频轨 %d: %s  %dx%d", vt, mime, w, h); line();

    AMediaExtractor_selectTrack(ex, vt);

    /* 真实路径：输出到一个 surface（ImageReader 提供），不是字节缓冲 */
    AImageReader *reader = NULL;
    if (AImageReader_new(w, h, AIMAGE_FORMAT_YUV_420_888, 8, &reader) != AMEDIA_OK) {
        puts("AImageReader_new 失败"); return 1;
    }
    ANativeWindow *win = NULL;
    if (AImageReader_getWindow(reader, &win) != AMEDIA_OK) {
        puts("AImageReader_getWindow 失败"); return 1;
    }

    AMediaCodec *codec = want ? AMediaCodec_createCodecByName(want)
                              : AMediaCodec_createDecoderByType(mime);
    if (!codec) { printf("创建解码器失败（%s）", want ? want : mime); line(); return 1; }

    char *name = NULL;
    if (AMediaCodec_getName(codec, &name) == AMEDIA_OK && name) {
        printf("★ 实际用的组件: %s", name); line();
    }

    if (AMediaCodec_configure(codec, fmt, win, NULL, 0) != AMEDIA_OK) {
        puts("configure 失败"); return 1;
    }
    if (AMediaCodec_start(codec) != AMEDIA_OK) { puts("start 失败"); return 1; }

    int frames = 0, eos = 0, spins = 0;
    while (frames < 30 && spins < 2000) {
        spins++;
        if (!eos) {
            ssize_t ii = AMediaCodec_dequeueInputBuffer(codec, 10000);
            if (ii >= 0) {
                size_t cap = 0;
                uint8_t *buf = AMediaCodec_getInputBuffer(codec, ii, &cap);
                ssize_t n = AMediaExtractor_readSampleData(ex, buf, cap);
                if (n < 0) {
                    AMediaCodec_queueInputBuffer(codec, ii, 0, 0, 0, AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM);
                    eos = 1;
                } else {
                    int64_t pts = AMediaExtractor_getSampleTime(ex);
                    AMediaCodec_queueInputBuffer(codec, ii, 0, n, pts, 0);
                    AMediaExtractor_advance(ex);
                }
            }
        }
        AMediaCodecBufferInfo info;
        ssize_t oi = AMediaCodec_dequeueOutputBuffer(codec, &info, 10000);
        if (oi >= 0) {
            frames++;
            AMediaCodec_releaseOutputBuffer(codec, oi, true);
            if (info.flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM) break;
        }
    }

    printf("解出帧数: %d（自旋 %d 次）", frames, spins); line();
    AMediaCodec_stop(codec);
    AMediaCodec_delete(codec);
    AImageReader_delete(reader);
    AMediaExtractor_delete(ex);
    close(fd);

    if (frames > 0) { puts("★ 通过：真的解出了帧"); return 0; }
    puts("失败：一帧都没解出来");
    return 1;
}
