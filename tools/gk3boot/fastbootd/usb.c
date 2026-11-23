/* gk3-fastbootd：USB 传输（FunctionFS），fastboot-design §4.2.1、§4.7。
 *
 * 描述符照抄设备端上游 refs/lineage-system-core/fastboot/device/usb_client.cpp:31-286：
 *   接口 0xff/0x42/0x03（主机端判据 fastboot/fastboot.cpp:244），两个 bulk 端点（v2 描述符两端同号 1，
 *   写 v2 失败就退回 v1，v1 里 IN 是 2）；FS 64 / HS 512 / SS 1024 字节包；接口字符串 "fastbootd"。
 *   端点文件：ep0 控制、ep1 = OUT（主机 → 设备）、ep2 = IN。
 * gadget：执行端里由 /init 建（usb_gadget/gk3fb、挂 functionfs、看到 ep1 才绑 UDC，README §13.3），本进程走
 *   --usb-nosetup：只打开 $GK3_FFS/ep0 写描述符与字符串，不碰 configfs 的 UDC、不碰 role。
 *   不在 /init 之下单独跑时（开发 / 别的 initramfs）也可以 --usb 让本进程自己建（以下几行是那条路径）：
 *   g1，18D1:4EE0（refs/aosp-build/target/product/base_vendor.mk:33-35 的 fastboot PID），一个 ffs.fastboot 函数，
 *   functionfs 挂到 /dev/usb-ffs/fastboot，描述符写进 ep0 之后才写 UDC（顺序反了 UDC 绑不上）。
 *   序列号 gaokun3（与 adb gadget 一致，init.gaokun3.usb.rc；U5 未定）。
 * role：/sys/class/usb_role/<udc>-role-switch/role 已经是 device 就不写，不是才写一次 —— 与
 *   Android 的 USB 端口 HAL（device/huawei/gaokun3/usb/Port.h）同一个文件。【绝不】unbind / rebind dwc3（patch 0012:13，#52）。
 * 同步 read/write（不用 aio）：fastboot 是单通道、主机驱动的同步协议，每次读满一个命令或一段数据。
 * ep0 由单独的线程读事件；SETUP 一律 stall（主机端 fastboot 不发控制请求）。
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "fbd.h"

#ifndef __linux__
fb_transport *fb_usb_new(const char *udc, bool setup_gadget, const char *ffs_dir)
{
    (void)udc; (void)setup_gadget; (void)ffs_dir;
    fb_log("usb: FunctionFS transport needs Linux");
    return NULL;
}
#else
#include <endian.h>
#include <linux/usb/ch9.h>
#include <linux/usb/functionfs.h>
#include <pthread.h>
#include <sys/mount.h>

#define FFS_DEFAULT "/dev/usb-ffs/fastboot"
static char ffs_root[200] = FFS_DEFAULT;   /* /init 建 gadget 时由 GK3_FFS 指定（--usb-nosetup 路径） */
#define GADGET    "/sys/kernel/config/usb_gadget/g1"
#define IO_CHUNK  (1u << 20)

struct func_desc {
    struct usb_interface_descriptor intf;
    struct usb_endpoint_descriptor_no_audio source;
    struct usb_endpoint_descriptor_no_audio sink;
} __attribute__((packed));

struct ss_func_desc {
    struct usb_interface_descriptor intf;
    struct usb_endpoint_descriptor_no_audio source;
    struct usb_ss_ep_comp_descriptor source_comp;
    struct usb_endpoint_descriptor_no_audio sink;
    struct usb_ss_ep_comp_descriptor sink_comp;
} __attribute__((packed));

struct desc_v2 {
    struct usb_functionfs_descs_head_v2 header;
    __le32 fs_count, hs_count, ss_count;
    struct func_desc fs, hs;
    struct ss_func_desc ss;
} __attribute__((packed));

struct desc_v1 {
    struct {
        __le32 magic, length, fs_count, hs_count;
    } __attribute__((packed)) header;
    struct func_desc fs, hs;
} __attribute__((packed));

/* 与上游 usb_client.cpp 的结构体逐字节同形：FS/HS 接口 9 + 端点 7×2；SS 再加两个 6 字节 companion */
_Static_assert(sizeof(struct func_desc) == 23, "func_desc");
_Static_assert(sizeof(struct ss_func_desc) == 35, "ss_func_desc");
_Static_assert(sizeof(struct desc_v2) == 12 + 12 + 23 + 23 + 35, "desc_v2");
_Static_assert(sizeof(struct desc_v1) == 16 + 23 + 23, "desc_v1");

#define STR_IFACE "fastbootd"
static struct {
    struct usb_functionfs_strings_head header;
    struct {
        __le16 code;
        char str1[sizeof(STR_IFACE)];
    } __attribute__((packed)) lang0;
} __attribute__((packed)) strings;   /* glibc 的 htole32 不是常量表达式 ⇒ 在 fb_usb_new 里填 */

static void fill_intf(struct usb_interface_descriptor *i)
{
    i->bLength = USB_DT_INTERFACE_SIZE;
    i->bDescriptorType = USB_DT_INTERFACE;
    i->bInterfaceNumber = 0;
    i->bNumEndpoints = 2;
    i->bInterfaceClass = USB_CLASS_VENDOR_SPEC;
    i->bInterfaceSubClass = 66;
    i->bInterfaceProtocol = 3;
    i->iInterface = 1;
}

static void fill_ep(struct usb_endpoint_descriptor_no_audio *e, uint8_t addr, uint16_t mps)
{
    e->bLength = sizeof(*e);
    e->bDescriptorType = USB_DT_ENDPOINT;
    e->bEndpointAddress = addr;
    e->bmAttributes = USB_ENDPOINT_XFER_BULK;
    e->wMaxPacketSize = htole16(mps);
}

static void fill_fd(struct func_desc *f, uint16_t mps, uint8_t in_num)
{
    fill_intf(&f->intf);
    fill_ep(&f->source, 1 | USB_DIR_OUT, mps);
    fill_ep(&f->sink, in_num | USB_DIR_IN, mps);
}

typedef struct {
    int ep0, out, in;
    pthread_t ep0_thr;
    volatile int enabled;
} usbx;

static int write_file(const char *path, const char *val)
{
    int fd = open(path, O_WRONLY | O_CLOEXEC);
    ssize_t n;
    if (fd < 0)
        return -1;
    n = write(fd, val, strlen(val));
    close(fd);
    return n == (ssize_t)strlen(val) ? 0 : -1;
}

static void read_file(const char *path, char *out, size_t n)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    ssize_t r;
    out[0] = 0;
    if (fd < 0)
        return;
    r = read(fd, out, n - 1);
    close(fd);
    out[r > 0 ? r : 0] = 0;
    out[strcspn(out, "\n")] = 0;
}

static int mkdirs(const char *p)
{
    char b[512];
    snprintf(b, sizeof(b), "%s", p);
    for (char *s = b + 1; *s; s++)
        if (*s == '/') {
            *s = 0;
            mkdir(b, 0755);
            *s = '/';
        }
    return mkdir(b, 0755) == 0 || errno == EEXIST ? 0 : -1;
}

/* 有 role switch 而且不是 device 才写一次，然后等 UDC 出现（dwc3 的角色切换是异步的）。 */
static void ensure_device_role(const char *udc)
{
    char p[256], v[64];
    snprintf(p, sizeof(p), "/sys/class/usb_role/%s-role-switch/role", udc);
    read_file(p, v, sizeof(v));
    if (!v[0]) {
        fb_log("usb: no role switch %s (leaving role alone)", p);
        return;
    }
    if (!strcmp(v, "device"))
        return;
    fb_log("usb: role is '%s', switching to device once", v);
    if (write_file(p, "device"))
        fb_log("usb: writing %s failed: %s", p, strerror(errno));
    for (int i = 0; i < 60; i++) {
        struct stat st;
        snprintf(p, sizeof(p), "/sys/class/udc/%s", udc);
        if (!stat(p, &st))
            return;
        usleep(100000);
    }
    fb_log("usb: UDC %s did not appear after the role switch", udc);
}

static int setup_gadget(void)
{
    struct stat st;
    if (stat("/sys/kernel/config/usb_gadget", &st)) {
        mkdirs("/sys/kernel/config");
        if (mount("configfs", "/sys/kernel/config", "configfs", 0, NULL) && errno != EBUSY) {
            fb_log("usb: mount configfs: %s", strerror(errno));
            return -1;
        }
    }
    if (mkdirs(GADGET) || mkdirs(GADGET "/strings/0x409") || mkdirs(GADGET "/configs/b.1/strings/0x409") ||
        mkdirs(GADGET "/functions/ffs.fastboot")) {
        fb_log("usb: cannot create gadget %s: %s", GADGET, strerror(errno));
        return -1;
    }
    write_file(GADGET "/idVendor", "0x18D1");
    write_file(GADGET "/idProduct", "0x4EE0");
    write_file(GADGET "/bcdDevice", "0x0100");
    write_file(GADGET "/bcdUSB", "0x0200");
    write_file(GADGET "/strings/0x409/serialnumber", G.serial);
    write_file(GADGET "/strings/0x409/manufacturer", "HUAWEI");
    write_file(GADGET "/strings/0x409/product", "MateBookEGo fastboot");
    write_file(GADGET "/configs/b.1/strings/0x409/configuration", "fastboot");
    write_file(GADGET "/configs/b.1/MaxPower", "500");
    if (symlink(GADGET "/functions/ffs.fastboot", GADGET "/configs/b.1/f1") && errno != EEXIST) {
        fb_log("usb: link function: %s", strerror(errno));
        return -1;
    }
    char ep0[256];
    snprintf(ep0, sizeof(ep0), "%s/ep0", ffs_root);
    mkdirs(ffs_root);
    if (stat(ep0, &st) && mount("fastboot", ffs_root, "functionfs", 0, "no_disconnect=1")) {
        fb_log("usb: mount functionfs: %s", strerror(errno));
        return -1;
    }
    return 0;
}

static void *ep0_thread(void *arg)
{
    usbx *x = arg;
    struct usb_functionfs_event ev[4];
    static const char *const names[] = {"BIND", "UNBIND", "ENABLE", "DISABLE", "SETUP", "SUSPEND", "RESUME"};
    for (;;) {
        ssize_t n = read(x->ep0, ev, sizeof(ev));
        if (n < 0) {
            if (errno == EINTR)
                continue;
            fb_log("usb: ep0 read: %s", strerror(errno));
            sleep(1);
            continue;
        }
        for (ssize_t i = 0; i < n / (ssize_t)sizeof(ev[0]); i++) {
            unsigned t = ev[i].type;
            if (t != FUNCTIONFS_SETUP)
                fb_log("usb: event %s", t < 7 ? names[t] : "?");
            if (t == FUNCTIONFS_ENABLE)
                x->enabled = 1;
            else if (t == FUNCTIONFS_DISABLE || t == FUNCTIONFS_UNBIND)
                x->enabled = 0;
            else if (t == FUNCTIONFS_SETUP) {
                /* 反方向做一次 I/O = stall（Documentation/usb/functionfs.rst） */
                char c;
                if (ev[i].u.setup.bRequestType & USB_DIR_IN) {
                    if (read(x->ep0, &c, 0) < 0) { }
                } else {
                    if (write(x->ep0, &c, 0) < 0) { }
                }
            }
        }
    }
    return NULL;
}

static int open_eps(usbx *x)
{
    char p[256];
    snprintf(p, sizeof(p), "%s/ep1", ffs_root);
    x->out = open(p, O_RDONLY | O_CLOEXEC);
    snprintf(p, sizeof(p), "%s/ep2", ffs_root);
    x->in = open(p, O_WRONLY | O_CLOEXEC);
    if (x->out < 0 || x->in < 0) {
        fb_log("usb: open bulk endpoints: %s", strerror(errno));
        if (x->out >= 0)
            close(x->out);
        if (x->in >= 0)
            close(x->in);
        x->out = x->in = -1;
        return -1;
    }
    return 0;
}

static int u_open(fb_transport *t)
{
    usbx *x = t->priv;
    while (x->out < 0 && open_eps(x))
        sleep(1);
    return 0;
}

static long u_read_cmd(fb_transport *t, char *buf, size_t max)
{
    usbx *x = t->priv;
    for (;;) {
        ssize_t n = read(x->out, buf, max);
        if (n < 0 && errno == EINTR)
            continue;
        if (n == 0)
            continue;   /* 零长度包：协议说忽略 */
        if (n < 0) {
            fb_log("usb: bulk-out read: %s", strerror(errno));
            return -1;
        }
        return n;
    }
}

static int u_read_data(fb_transport *t, void *buf, size_t len)
{
    usbx *x = t->priv;
    uint8_t *b = buf;
    while (len) {
        size_t want = len > IO_CHUNK ? IO_CHUNK : len;
        ssize_t n = read(x->out, b, want);
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0) {
            fb_log("usb: bulk-out read (data): %s", strerror(errno));
            return -1;
        }
        b += n;
        len -= (size_t)n;
    }
    return 0;
}

static int u_write(fb_transport *t, const void *buf, size_t len)
{
    usbx *x = t->priv;
    const uint8_t *b = buf;
    while (len) {
        ssize_t n = write(x->in, b, len);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0) {
            fb_log("usb: bulk-in write: %s", strerror(errno));
            return -1;
        }
        b += n;
        len -= (size_t)n;
    }
    return 0;
}

static void u_close(fb_transport *t)
{
    usbx *x = t->priv;
    /* 断开（拔线 / 主机复位）后重开端点；ep0 与描述符保持，gadget 不动 */
    if (x->out >= 0)
        close(x->out);
    if (x->in >= 0)
        close(x->in);
    x->out = x->in = -1;
}

fb_transport *fb_usb_new(const char *udc, bool setup, const char *ffs_dir)
{
    fb_transport *t = calloc(1, sizeof(*t));
    usbx *x = calloc(1, sizeof(*x));
    struct desc_v2 v2;
    struct desc_v1 v1;
    char cur[64], ep0[256];

    if (!t || !x)
        return NULL;
    if (ffs_dir && ffs_dir[0])
        snprintf(ffs_root, sizeof(ffs_root), "%s", ffs_dir);
    snprintf(ep0, sizeof(ep0), "%s/ep0", ffs_root);
    x->out = x->in = -1;
    if (setup) {
        ensure_device_role(udc);
        if (setup_gadget())
            return NULL;
    }
    x->ep0 = open(ep0, O_RDWR | O_CLOEXEC);
    if (x->ep0 < 0) {
        fb_log("usb: open %s: %s", ep0, strerror(errno));
        return NULL;
    }
    memset(&v2, 0, sizeof(v2));
    v2.header.magic = htole32(FUNCTIONFS_DESCRIPTORS_MAGIC_V2);
    v2.header.length = htole32(sizeof(v2));
    v2.header.flags = htole32(FUNCTIONFS_HAS_FS_DESC | FUNCTIONFS_HAS_HS_DESC | FUNCTIONFS_HAS_SS_DESC);
    v2.fs_count = htole32(3);
    v2.hs_count = htole32(3);
    v2.ss_count = htole32(5);
    fill_fd(&v2.fs, 64, 1);
    fill_fd(&v2.hs, 512, 1);
    fill_intf(&v2.ss.intf);
    fill_ep(&v2.ss.source, 1 | USB_DIR_OUT, 1024);
    fill_ep(&v2.ss.sink, 1 | USB_DIR_IN, 1024);
    v2.ss.source_comp.bLength = v2.ss.sink_comp.bLength = sizeof(v2.ss.source_comp);
    v2.ss.source_comp.bDescriptorType = v2.ss.sink_comp.bDescriptorType = USB_DT_SS_ENDPOINT_COMP;
    v2.ss.source_comp.bMaxBurst = v2.ss.sink_comp.bMaxBurst = 15;
    if (write(x->ep0, &v2, sizeof(v2)) < 0) {
        memset(&v1, 0, sizeof(v1));
        v1.header.magic = htole32(FUNCTIONFS_DESCRIPTORS_MAGIC);
        v1.header.length = htole32(sizeof(v1));
        v1.header.fs_count = htole32(3);
        v1.header.hs_count = htole32(3);
        fill_fd(&v1.fs, 64, 2);
        fill_fd(&v1.hs, 512, 2);
        if (write(x->ep0, &v1, sizeof(v1)) < 0) {
            fb_log("usb: write descriptors: %s", strerror(errno));
            return NULL;
        }
    }
    strings.header.magic = htole32(FUNCTIONFS_STRINGS_MAGIC);
    strings.header.length = htole32(sizeof(strings));
    strings.header.str_count = htole32(1);
    strings.header.lang_count = htole32(1);
    strings.lang0.code = htole16(0x0409);
    memcpy(strings.lang0.str1, STR_IFACE, sizeof(STR_IFACE));
    if (write(x->ep0, &strings, sizeof(strings)) < 0) {
        fb_log("usb: write strings: %s", strerror(errno));
        return NULL;
    }
    pthread_create(&x->ep0_thr, NULL, ep0_thread, x);
    if (setup) {
        read_file(GADGET "/UDC", cur, sizeof(cur));
        if (strcmp(cur, udc) && write_file(GADGET "/UDC", udc))
            fb_log("usb: bind UDC %s: %s (will keep waiting for the host anyway)", udc, strerror(errno));
        else
            fb_log("usb: gadget g1 18D1:4EE0 bound to %s, serial %s", udc, G.serial);
    }
    t->name = "usb";
    t->open_session = u_open;
    t->read_cmd = u_read_cmd;
    t->read_data = u_read_data;
    t->write = u_write;
    t->close_session = u_close;
    t->priv = x;
    return t;
}
#endif
