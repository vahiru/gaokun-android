// SPDX-License-Identifier: Apache-2.0
/* Fixed gaokun3 initialization-only diagnostic; never captures image data.
 * Stop the Android HAL before use. No timeout may reset an active secure call. */
#include "../../device/huawei/gaokun3/fingerprint/gaokun3_fpcmd.h"
#include <errno.h>
#include <fcntl.h>
#include <linux/gpio.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

static int level(int fd, unsigned value) {
    struct gpio_v2_line_values v = {.mask = 1};
    if (ioctl(fd, GPIO_V2_LINE_GET_VALUES_IOCTL, &v)) return -1;
    if (v.bits != value) { errno = EIO; return -1; }
    return 0;
}
int main(void) {
    setbuf(stdout, NULL);
    int result = 1, loaded = 0, gpio = -1;
    int chip = open("/dev/gpiochip4", O_RDONLY | O_CLOEXEC);
    int fp = open("/dev/gaokun3_fpcmd", O_RDWR | O_CLOEXEC);
    if (chip < 0 || fp < 0) { perror("open fixed devices"); goto done; }
    struct gpiochip_info ci = {0};
    struct gpio_v2_line_info info = {.offset = 185};
    if (ioctl(chip, GPIO_GET_CHIPINFO_IOCTL, &ci) ||
        strcmp(ci.label, "f100000.pinctrl") || ci.lines != 230 ||
        ioctl(chip, GPIO_V2_GET_LINEINFO_IOCTL, &info) || info.consumer[0]) {
        fprintf(stderr, "Fixed GPIO identity/ownership check failed\n"); goto done;
    }
    signal(SIGINT, SIG_IGN); signal(SIGTERM, SIG_IGN); signal(SIGHUP, SIG_IGN);
    if (ioctl(fp, GAOKUN3_FPCMD_IOC_LOAD)) { perror("TA load"); goto done; }
    loaded = 1; puts("TA load OK");
    struct gpio_v2_line_request req = {0};
    req.offsets[0] = 185; req.num_lines = 1;
    strcpy(req.consumer, "gaokun3-init-probe");
    req.config.flags = GPIO_V2_LINE_FLAG_OUTPUT | GPIO_V2_LINE_FLAG_BIAS_PULL_UP;
    req.config.num_attrs = 1;
    req.config.attrs[0].attr.id = GPIO_V2_LINE_ATTR_ID_OUTPUT_VALUES;
    req.config.attrs[0].attr.values = 1; req.config.attrs[0].mask = 1;
    if (ioctl(chip, GPIO_V2_GET_LINE_IOCTL, &req)) { perror("GPIO acquire"); goto done; }
    gpio = req.fd;
    struct gpio_v2_line_values v = {.mask = 1};
    if (level(gpio, 1) || usleep(10000) ||
        ioctl(gpio, GPIO_V2_LINE_SET_VALUES_IOCTL, &v) || level(gpio, 0) ||
        usleep(400000)) { perror("GPIO pulse/readback"); goto done; }
    puts("GPIO high10ms -> low400ms; pull-up held; readback OK");
    if (ioctl(fp, GAOKUN3_FPCMD_IOC_INIT)) { perror("Sensor INIT"); goto done; }
    puts("Sensor INIT OK");
    if (ioctl(fp, GAOKUN3_FPCMD_IOC_PROBE)) { perror("Sensor PROBE"); goto done; }
    puts("Sensor PROBE OK (no image capture)"); result = 0;
done:
    if (loaded && ioctl(fp, GAOKUN3_FPCMD_IOC_UNLOAD)) {
        perror("TA shutdown failed; preserving GPIO until reboot; do not kill");
        for (;;) pause();
    }
    if (loaded) puts("TA shutdown OK");
    if (gpio >= 0) {
        struct gpio_v2_line_config restore = {
            .flags = GPIO_V2_LINE_FLAG_INPUT | GPIO_V2_LINE_FLAG_BIAS_PULL_DOWN};
        if (ioctl(gpio, GPIO_V2_LINE_SET_CONFIG_IOCTL, &restore)) {
            perror("GPIO restore"); result = 1;
        }
        close(gpio);
    }
    if (fp >= 0) close(fp);
    if (chip >= 0) close(chip);
    return result;
}
