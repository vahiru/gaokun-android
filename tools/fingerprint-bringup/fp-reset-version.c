/* SPDX-License-Identifier: GPL-2.0 */
/* Fixed gaokun3 GPIO185 reset + version probe. No arbitrary GPIO or command.
 * Holds the line until the synchronous probe returns; a hung secure call needs
 * reboot, not a timer that resets hardware underneath it. */
#include <errno.h>
#include <fcntl.h>
#include <linux/gpio.h>
#include <stdio.h>
#include <signal.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

/* GPIO v2 flags do not report firmware-inherited pinctrl bias. Verify the
 * controller's actual debugfs baseline before replacing that bias explicitly. */
static int inherited_pulldown(void)
{
 FILE *f = fopen("/sys/kernel/debug/gpio", "r");
 char line[512]; int in_chip = 0, found = 0;
 if (!f) return 0;
 while (fgets(line, sizeof(line), f)) {
  if (!strncmp(line, "gpiochip", 8))
   in_chip = strstr(line, "gpiochip4:") && strstr(line, "f100000.pinctrl");
  if (in_chip && strstr(line, " gpio185 :") && strstr(line, "in  low") &&
      strstr(line, "func0 2mA pull down")) found++;
 }
 fclose(f); return found == 1;
}

static int check_level(int fd, unsigned long long expected)
{
 struct gpio_v2_line_values v = {.mask = 1};
 if (ioctl(fd, GPIO_V2_LINE_GET_VALUES_IOCTL, &v)) return -1;
 if (v.bits != expected) {errno = EIO; return -1;}
 return 0;
}

static int secure_idle(void)
{
 char state[4] = {0};
 int fd = open("/sys/module/qcom_qseecom_fpversion/parameters/secure_pending", O_RDONLY | O_CLOEXEC);
 ssize_t n = fd < 0 ? -1 : read(fd, state, sizeof(state));
 if (fd >= 0) close(fd);
 return n == 2 && state[0] == 'N' && state[1] == '\n';
}

static int sleep_ms(long ms)
{
 struct timespec t = {ms / 1000, (ms % 1000) * 1000000};
 while (nanosleep(&t, &t)) if (errno != EINTR) return -1;
 return 0;
}
int main(int argc, char **argv)
{
 struct gpiochip_info chip = {0};
 struct gpio_v2_line_info info = {.offset = 185};
 struct gpio_v2_line_request req = {0};
 struct gpio_v2_line_values value = {.mask = 1, .bits = 0};
 struct gpio_v2_line_config restore = {0};
 int fd, probe = -1, rc = 1, save = 0;
 unsigned long long original;
 const unsigned long long expected = GPIO_V2_LINE_FLAG_INPUT | GPIO_V2_LINE_FLAG_BIAS_PULL_DOWN;
 setbuf(stdout, NULL);
 if (argc != 2 || (strcmp(argv[1], "--inspect") && strcmp(argv[1], "--reset-and-probe"))) {
  fprintf(stderr, "usage: %s --inspect|--reset-and-probe\n", argv[0]); return 2;
 }
 fd = open("/dev/gpiochip4", O_RDONLY | O_CLOEXEC);
 if (fd < 0) {perror("gpiochip4"); return 1;}
 if (ioctl(fd, GPIO_GET_CHIPINFO_IOCTL, &chip) ||
     ioctl(fd, GPIO_V2_GET_LINEINFO_IOCTL, &info)) {perror("gpio info"); goto out;}
 printf("chip=%s label=%s lines=%u offset=%u flags=0x%llx consumer=%s\n",
        chip.name, chip.label, chip.lines, info.offset, (unsigned long long)info.flags, info.consumer);
 if (strcmp(chip.label, "f100000.pinctrl") || chip.lines != 230 ||
     (info.flags != expected && info.flags != GPIO_V2_LINE_FLAG_INPUT) ||
     info.num_attrs != 0 || info.consumer[0] || !inherited_pulldown()) {
  fprintf(stderr, "Unexpected GPIO identity/configuration; refusing mutation\n"); goto out;
 }
 if (!strcmp(argv[1], "--inspect")) {rc = 0; goto out;}
 /* Open the already-loaded single-shot probe before acquiring or changing GPIO. */
 probe = open("/sys/kernel/debug/gaokun3_fpversion/probe", O_WRONLY | O_CLOEXEC);
 if (probe < 0) {perror("version probe"); goto out;}
 /* Do not reset an MCU whose earlier secure call has not shut down. */
 if (!secure_idle()) {
  fprintf(stderr, "Secure state not idle before reset; refusing GPIO mutation\n"); goto out;
 }
 /* Install this before owning GPIO, including the time spent inside write().
  * Ignoring signals only after an uncertain write returns leaves a hung SMC
  * vulnerable to closing the GPIO FD on SIGINT/TERM/HUP. SIGKILL remains an
  * unavoidable external limitation: do not forcibly terminate the helper. */
 if (signal(SIGINT, SIG_IGN) == SIG_ERR || signal(SIGTERM, SIG_IGN) == SIG_ERR ||
     signal(SIGHUP, SIG_IGN) == SIG_ERR) {
  perror("install GPIO hold signal policy"); goto out;
 }
 original = expected; /* Explicitly restore the verified inherited pull-down. */
 req.offsets[0] = 185; req.num_lines = 1;
 strcpy(req.consumer, "gaokun3-fp-reset-version");
 req.config.flags = GPIO_V2_LINE_FLAG_OUTPUT | GPIO_V2_LINE_FLAG_BIAS_PULL_UP;
 req.config.num_attrs = 1;
 req.config.attrs[0].attr.id = GPIO_V2_LINE_ATTR_ID_OUTPUT_VALUES;
 req.config.attrs[0].attr.values = 1;
 req.config.attrs[0].mask = 1;
 if (ioctl(fd, GPIO_V2_GET_LINE_IOCTL, &req)) {perror("request GPIO185"); goto out;}
 printf("GPIO185 acquired output=1 pull-up\n");
 if (check_level(req.fd, 1) || sleep_ms(10) ||
     ioctl(req.fd, GPIO_V2_LINE_SET_VALUES_IOCTL, &value) ||
     check_level(req.fd, 0) || sleep_ms(400)) {
  perror("reset pulse"); goto release;
 }
 printf("GPIO185 output=0; 400ms wait complete; running fixed version probe\n");
 {
  ssize_t written = write(probe, "probe\n", 6);
  if (written != 6) {
   save = written < 0 ? errno : EIO;
   fprintf(stderr, "probe failed: %s\n", strerror(save));
  } else rc = 0;
 }
 if (!secure_idle()) {
  fprintf(stderr, "Secure state uncertain; retaining GPIO185 output0 and FD until reboot. Do not kill this process.\n");
  for (;;) pause();
 }
release:
 restore.flags = original;
 if (ioctl(req.fd, GPIO_V2_LINE_SET_CONFIG_IOCTL, &restore)) {
  perror("RESTORE GPIO185 FAILED"); rc = 1;
 }
 close(req.fd);
 memset(&info, 0, sizeof(info)); info.offset = 185;
 if (ioctl(fd, GPIO_V2_GET_LINEINFO_IOCTL, &info) || info.consumer[0] ||
     (info.flags != expected && info.flags != GPIO_V2_LINE_FLAG_INPUT) ||
     info.num_attrs || !inherited_pulldown()) {
  fprintf(stderr, "GPIO185 restoration readback FAILED\n"); rc = 1;
 } else printf("GPIO185 released; original input/low/pull-down verified\n");
out:
 if (probe >= 0) close(probe);
 close(fd);
 return rc;
}
