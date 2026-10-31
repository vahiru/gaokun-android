// SPDX-License-Identifier: GPL-2.0
/*
 * gk3pend - prototype M-Pencil daemon for the HUAWEI MateBook E Go (gaokun3).
 *
 * Inputs
 *   /dev/gk3_pen   slave frames from gk3_pen_slave.ko (struct pen_rec, 352 B)
 *   /dev/hidrawN   pen MCU (USB 12d1:10b8) interface 0, ReportID 0x55 'U':
 *                  55 freq1 freq2, then four u16 LE pressure slots
 *                  (slot0==slot1, slot2==slot3; slot3 is the newest sample).
 *                  Found by its report descriptor, not by number.
 *   usbfs          the same MCU's interface 1 (vendor class, bulk 0x02/0x85,
 *                  64-byte packets): side-touch, attach and connection events.
 *                  It only talks after the host queries 0x7101 / 0x7701, and
 *                  every event must be ACKed (0x8001) or the next ones queue
 *                  up behind it.  Several events can arrive packed in one
 *                  transfer.  Protocol: EGoTouchRev penevt/BTMCU_PROTOCOL.md.
 *
 * Output: one uinput single-touch pen (BTN_TOOL_PEN / BTN_TOOL_RUBBER,
 * BTN_TOUCH, BTN_STYLUS, ABS_X/Y, ABS_PRESSURE, INPUT_PROP_DIRECT).  Android
 * turns that into TOOL_TYPE_STYLUS / TOOL_TYPE_ERASER with pressure, and
 * HOVER_* while BTN_TOUCH is 0.
 *
 * Coordinates are the touchscreen's raw space in 1/16 steps (SUBPX):
 * X 0..1599 px from the slave's row, Y 0..2559 px from its column, reported
 * as 0..25599 and 0..40959.  The pixel mapping is hx-algo.c's
 * (x = col_q8 / 6, y = 5 * row_q8 / 32) followed by the DT's
 * touchscreen-swapped-x-y.  The slave numbers its columns the other way
 * round from the master (seen on the device: left/right came out mirrored),
 * so the column is flipped by default; -fx/-fy toggle each axis.
 *
 * Position and pressure: EGoTouchRev's HPP3 steps (solver.h).  Pressure
 * slots are played back the factory way by default (-pm incell): the gaokun
 * ASA table sets GetPressInMapOrder (+0xa30) = 2 = incell, so each packet's
 * four slots (old, old, new, new) feed four successive frames instead of
 * jumping straight to the newest (-pm direct, EGoTouchRev's default).
 *
 * Battery: the level (event 0x08, percent) and charging state (0x09), sent
 * at the handshake, on wake-up and when charging starts or stops, go to the
 * driver with a 3-byte write ('B', level, charging) to /dev/gk3_pen, which
 * shows them as the stylus's battery (power supply m-pencil); each change
 * is logged.  So are the pen's model id (0x00) and firmware version (0x03),
 * which the controller sends when it comes online (after a USB reset).
 *
 * Side touch: the pen reports a double-tap on its side as event 0x2F = 1
 * (single taps and long presses report nothing).  Windows uses it to switch
 * between pen and eraser; -side eraser (default) does the same, -side button
 * sends a BTN_STYLUS click instead, -side none ignores it.  A switch asked for
 * while the tip is down waits for the lift, so a stroke never changes tool.
 *
 * Tilt: TiltProcess (TX1 tip vs TX2 ring electrode offset -> asin per axis)
 * reported as ABS_TILT_X/Y in degrees (resolution 0: Android reads degrees);
 * CoorReviseProcess then moves the tip back by a per-axis amount per degree.
 * The constants are measured on this unit, see solver.h.  -tx/-ty negate an
 * axis, -nt turns tilt and the revision off.  -rec FILE appends every frame
 * with the pen in range (struct pen_rec + pressure, touch, tilt x/y) for
 * offline calibration with tilt_cal.c.
 *
 * While the pen controller reports (from the moment the pen is picked up),
 * gk3pend writes to /dev/gk3_pen at most every 200 ms, which keeps the driver
 * out of its idle polling or ends it before the pen reaches the screen.
 *
 * Builds against bionic or glibc, or statically with the kernel's nolibc
 * (which defines NOLIBC and provides the libc part itself).
 */
#ifndef NOLIBC
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>
#endif
#include <linux/hidraw.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <linux/usbdevice_fs.h>

#define FRAME_LEN	336
#define PEAK_MIN	200	/* EGoTouchRev HPP3 seed threshold is 199 */
#define PRESS_MAX_AGE	200	/* ms: U reports come every ~10-17 ms, with gaps of 50+ */
#define RELEASE_DELAY	30	/* ms without a valid frame before leaving range */
#define STALE_MS	150	/* no frame at all for this long: leave range too */

/*
 * ABS_X/Y count 1/SUBPX display pixels.  Android scales the raw range to the
 * display in floating point, so the sub-pixel part reaches the app; in whole
 * pixels a slow stroke (1-2 px a frame) came out as a staircase.
 */
#define SUBPX		16
#define PX_MM		0.104	/* display pixel pitch: 2560 px over 266 mm */

#define MCU_EP_OUT	0x02
#define MCU_EP_IN	0x85
#define MCU_IFNUM	1
#define MCU_URBS	2
#define MCU_RETRY_MS	2000
#define HID_RETRY_MS	2000

enum { SIDE_NONE, SIDE_ERASER, SIDE_BUTTON };

struct pen_rec {
	unsigned long long t_ns;
	unsigned int seq;
	unsigned int flags;
	unsigned char d[FRAME_LEN];
};

struct rec_out {			/* -rec record, 368 B */
	struct pen_rec r;
	int pressure, touch, tilt_x, tilt_y;
};

struct mcu {
	int fd;
	int seen_77;			/* 0x77 replies since open: the first one gets a second 0x7701 */
	long long retry_at;
	struct usbdevfs_urb urb[MCU_URBS];
	unsigned char buf[MCU_URBS][64];
};

static int verbose, flip_x, flip_y = 1;
static int press_tau_ms = 4;	/* -pt N: pressure smoothing time constant */
static int side_mode = SIDE_ERASER;
static int press_incell = 1;	/* -pm incell|direct */
static int tilt_on = 1, tilt_sx = 1, tilt_sy = 1;	/* -nt, -tx, -ty */
static int rec_fd = -1;					/* -rec */
static int region_on = 1; /* -lr restores legacy peak selection */
static double screen_x_scale = 1.0; /* -sx: unit-specific landscape calibration */

static double screen_x_calibrate(double x)
{
	return 1279.875 + screen_x_scale * (x - 1279.875);
}

static int col_map;	/* the column pitch in solver.h: BOE panel only */

/*
 * Within a cell of an edge the position across that edge is extrapolated
 * (solver.h edge_pos) and jitters by a few px a frame: low-pass that axis
 * there (alpha 0.25 per frame, ~13 ms at 227 Hz).  Measured on edge strokes:
 * frame-to-frame wobble p99 18 -> 5 px, the same as before the edge change,
 * while 87% of the outermost-cell frames land within 10 px of the edge.
 */
#define EDGE_ZONE_X	40	/* one cell: 1600 / 40 */
#define EDGE_ZONE_Y	43	/* 2560 / 60 */

static double edge_smooth(double *s, double v, int zone, int max, int reset)
{
	if (reset || (v > zone && v < max - zone)) {
		*s = v;
		return v;
	}
	*s += (v - *s) * 0.25;
	return *s;
}

/*
 * Position filter.  The solved position jitters by a few tenths of a pixel
 * per frame, and a slow stroke moves only 1-2 px a frame, so a zoomed-in
 * line zigzags.  One Euro filter (Casiez et al., CHI 2012) on both axes with
 * one cutoff from the 2-D speed: mincutoff + beta x speed (mm/s), the speed
 * low-passed at dcutoff.  It restarts when the pen enters range and at every
 * touch-down, so a stroke starts where the tip lands.  -pf MIN,BETA,DCUT in
 * mHz, mHz per mm/s and mHz (as himax-spi's track_filter_*); -pf 0 is off.
 */
static int pf_mincutoff = 1000, pf_beta = 125, pf_dcutoff = 5000;

struct euro {
	double x, y;		/* filtered position, px */
	double vx, vy;		/* filtered speed, px/s */
	unsigned long long t_ns;
};

static double euro_alpha(double dt, int mhz)
{
	return 1.0 / (1.0 + 1000.0 / (2 * 3.141592653589793 * mhz * dt));
}

static void euro_filter(struct euro *e, double *x, double *y, unsigned long long t_ns, int restart)
{
	double dt = (double)(t_ns - e->t_ns) / 1e9, a, fc;

	if (restart || pf_mincutoff <= 0 || dt <= 0 || dt > 0.1) {
		e->x = *x;
		e->y = *y;
		e->vx = e->vy = 0;
		e->t_ns = t_ns;
		return;
	}
	e->t_ns = t_ns;
	a = euro_alpha(dt, pf_dcutoff);
	e->vx += a * ((*x - e->x) / dt - e->vx);
	e->vy += a * ((*y - e->y) / dt - e->vy);
	fc = pf_mincutoff + pf_beta * __builtin_sqrt(e->vx * e->vx + e->vy * e->vy) * PX_MM;
	a = euro_alpha(dt, (int)fc);
	e->x += a * (*x - e->x);
	e->y += a * (*y - e->y);
	*x = e->x;
	*y = e->y;
}

static long long clock_ms(int clock)
{
	struct timespec ts;

	clock_gettime(clock, &ts);
	return ts.tv_sec * 1000LL + ts.tv_nsec / 1000000;
}

static long long now_ms(void)
{
	return clock_ms(CLOCK_MONOTONIC);
}

/* grows by the time spent in system suspend, which monotonic time skips */
static long long suspended_ms(void)
{
	return clock_ms(CLOCK_BOOTTIME) - clock_ms(CLOCK_MONOTONIC);
}

/* gaokun3 ships with a BOE or a CSOT panel; the DSI panel node says which */
static int panel_is_boe(void)
{
	char b[8] = { 0 };
	int fd = open("/sys/firmware/devicetree/base/soc@0/display-subsystem@ae00000/"
		      "dsi@ae94000/panel@0/compatible", O_RDONLY);

	if (fd < 0)
		return 0;
	read(fd, b, sizeof(b) - 1);
	close(fd);
	return !strncmp(b, "boe,", 4);
}

static int word(const unsigned char *d, int i)
{
	return (short)(d[4 + 2 * i] | (d[5 + 2 * i] << 8));
}

#include "solver.h"

static void emit(int fd, int type, int code, int value)
{
	struct input_event ev;

	memset(&ev, 0, sizeof(ev));
	ev.type = type;
	ev.code = code;
	ev.value = value;
	if (write(fd, &ev, sizeof(ev)) != sizeof(ev))
		printf("uinput write failed: %d\n", errno);
}

static void abs_setup(int fd, int code, int max, int res)
{
	struct uinput_abs_setup a;

	memset(&a, 0, sizeof(a));
	a.code = code;
	a.absinfo.maximum = max;
	a.absinfo.resolution = res;
	ioctl(fd, UI_ABS_SETUP, &a);
}

static int uinput_create(void)
{
	struct uinput_setup u;
	int fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);

	if (fd < 0)
		return -1;
	ioctl(fd, UI_SET_EVBIT, EV_KEY);
	ioctl(fd, UI_SET_EVBIT, EV_ABS);
	ioctl(fd, UI_SET_KEYBIT, BTN_TOOL_PEN);
	ioctl(fd, UI_SET_KEYBIT, BTN_TOOL_RUBBER);
	ioctl(fd, UI_SET_KEYBIT, BTN_TOUCH);
	ioctl(fd, UI_SET_KEYBIT, BTN_STYLUS);
	ioctl(fd, UI_SET_ABSBIT, ABS_X);
	ioctl(fd, UI_SET_ABSBIT, ABS_Y);
	ioctl(fd, UI_SET_ABSBIT, ABS_PRESSURE);
	if (tilt_on) {
		ioctl(fd, UI_SET_ABSBIT, ABS_TILT_X);
		ioctl(fd, UI_SET_ABSBIT, ABS_TILT_Y);
	}
	ioctl(fd, UI_SET_PROPBIT, INPUT_PROP_DIRECT);
	abs_setup(fd, ABS_X, 1600 * SUBPX - 1, (int)(SUBPX / PX_MM));
	abs_setup(fd, ABS_Y, 2560 * SUBPX - 1, (int)(SUBPX / PX_MM));
	abs_setup(fd, ABS_PRESSURE, 4095, 0);
	if (tilt_on) {
		struct uinput_abs_setup a;

		/* degrees, symmetric; resolution 0 so Android takes the unit as degrees */
		for (int code = ABS_TILT_X; code <= ABS_TILT_Y; code++) {
			memset(&a, 0, sizeof(a));
			a.code = code;
			a.absinfo.minimum = -90;
			a.absinfo.maximum = 90;
			ioctl(fd, UI_ABS_SETUP, &a);
		}
	}

	memset(&u, 0, sizeof(u));
	u.id.bustype = BUS_VIRTUAL;	/* not USB/BT => Android treats it as internal */
	u.id.vendor = 0x12d1;
	u.id.product = 0xcd54;		/* NOT 10b8: the keyboard switch inhibits that */
	u.id.version = 1;
	strcpy(u.name, "gk3 M-Pencil");
	if (ioctl(fd, UI_DEV_SETUP, &u) < 0 || ioctl(fd, UI_DEV_CREATE, 0) < 0) {
		close(fd);
		return -1;
	}
	return fd;
}

/* ---- pressure: the hidraw node whose descriptor starts the 'U' collection ---- */

static int open_pressure_hidraw(void)
{
	static struct hidraw_report_descriptor rd;

	for (int i = 0; i < 16; i++) {
		char path[16] = "/dev/hidraw";
		int n = 11, fd, sz = 0;

		if (i >= 10)
			path[n++] = '0' + i / 10;
		path[n++] = '0' + i % 10;
		path[n] = 0;
		fd = open(path, O_RDONLY | O_NONBLOCK);
		if (fd < 0)
			continue;
		/* UsagePage 0xff0a ... ReportID 0x55, as on interface 0 of 12d1:10b8 */
		if (ioctl(fd, HIDIOCGRDESCSIZE, &sz) == 0 && sz >= 9 && sz <= HID_MAX_DESCRIPTOR_SIZE) {
			rd.size = sz;
			if (ioctl(fd, HIDIOCGRDESC, &rd) == 0 &&
			    rd.value[0] == 0x06 && rd.value[1] == 0x0a && rd.value[2] == 0xff) {
				for (int k = 0; k + 1 < sz && k < 16; k++)
					if (rd.value[k] == 0x85 && rd.value[k + 1] == 0x55) {
						printf("pressure: %s\n", path);
						return fd;
					}
			}
		}
		close(fd);
	}
	return -1;
}

/* ---- pen MCU event channel (interface 1) over usbfs ---- */

/* BTMCU_PROTOCOL.md "ACK 对照表", Confirmed rows only; -1: no ACK */
static int ack_code(unsigned char evt)
{
	switch (evt) {
	case 0x2F: return 0x0B;
	case 0x70: return 0x00;
	case 0x71: return 0x01;
	case 0x72: return 0x02;
	case 0x73: return 0x0D;
	case 0x74: return 0x03;
	case 0x75: return 0x04;
	case 0x76: return 0x05;
	case 0x77: return 0x06;
	case 0x78: return 0x07;
	case 0x79: return 0x08;
	case 0x7B: return 0x0A;
	case 0x7C: return 0x0C;
	case 0x7F: return 0x09;
	default:   return -1;
	}
}

static void put3(char *p, int n)
{
	p[0] = '0' + n / 100;
	p[1] = '0' + n / 10 % 10;
	p[2] = '0' + n % 10;
}

/* usbfs node of 12d1:10b8: read() on it starts with the device descriptor */
static int mcu_find(void)
{
	char path[] = "/dev/bus/usb/000/000";
	unsigned char d[18];

	for (int bus = 1; bus <= 8; bus++)
		for (int dev = 1; dev <= 32; dev++) {
			int fd;

			put3(path + 13, bus);
			put3(path + 17, dev);
			fd = open(path, O_RDWR);
			if (fd < 0)
				continue;
			if (read(fd, d, sizeof(d)) >= 12 &&
			    d[8] == 0xd1 && d[9] == 0x12 && d[10] == 0xb8 && d[11] == 0x10) {
				printf("mcu: %s\n", path);
				return fd;
			}
			close(fd);
		}
	return -1;
}

static int mcu_out(struct mcu *m, const unsigned char *p, int n)
{
	unsigned char b[64];
	struct usbdevfs_bulktransfer bt;

	memcpy(b, p, n);
	bt.ep = MCU_EP_OUT;
	bt.len = n;
	bt.timeout = 100;
	bt.data = b;
	return ioctl(m->fd, USBDEVFS_BULK, &bt);
}

static int mcu_submit(struct mcu *m, int i)
{
	struct usbdevfs_urb *u = &m->urb[i];

	memset(u, 0, sizeof(*u));
	u->type = USBDEVFS_URB_TYPE_BULK;
	u->endpoint = MCU_EP_IN;
	u->buffer = m->buf[i];
	u->buffer_length = sizeof(m->buf[i]);
	return ioctl(m->fd, USBDEVFS_SUBMITURB, u);
}

/* closing the usbfs fd kills pending URBs and releases the interface */
static void mcu_close(struct mcu *m, long long t)
{
	if (m->fd >= 0)
		close(m->fd);
	m->fd = -1;
	m->retry_at = t + MCU_RETRY_MS;
}

/*
 * The factory handshake (EGoTouchRev BTMCU_PROTOCOL.md, from an API Monitor
 * capture): 0x7101, 0x7701, and a second 0x7701 after the 0x77 reply.  The
 * factory also sends 0x7D01 (32 bytes of init params) after the first 0x7B;
 * answering every 0x7B with it makes the MCU report 0x7B again, ~20 times a
 * second, so it is not sent here.
 */
static const unsigned char q7101[] = { 0x07, 0x00, 0x02, 0x00, 0x01, 0x71, 0x11, 0x00 };
static const unsigned char q7701[] = { 0x07, 0x00, 0x02, 0x00, 0x01, 0x77, 0x11, 0x00 };

static void mcu_open(struct mcu *m, long long t)
{
	int ifnum = MCU_IFNUM;

	m->seen_77 = 0;
	m->fd = mcu_find();
	if (m->fd < 0) {
		m->retry_at = t + MCU_RETRY_MS;
		return;
	}
	if (ioctl(m->fd, USBDEVFS_CLAIMINTERFACE, &ifnum) < 0 ||
	    mcu_submit(m, 0) < 0 || mcu_submit(m, 1) < 0) {
		printf("mcu: claim/submit failed: %d\n", errno);
		mcu_close(m, t);
		return;
	}
	/* the MCU stays silent until queried (EGoTouchRev: THP_Service does this on start) */
	mcu_out(m, q7101, sizeof(q7101));
	mcu_out(m, q7701, sizeof(q7701));
}

/* PEN_MODULE (event 0x00) model ids, from EGoTouchRev btmcu/PenModuleModelId.h */
static const char *pen_model(unsigned int id)
{
	switch (id) {
	case 0x00011a: return "CD52 (HPP2: not handled)";
	case 0x00011b: return "CD54";
	case 0x01011b: return "CD54R";
	case 0x443002: return "CD54S";
	}
	return "unknown";
}

/*
 * Walk the events packed in one transfer (02 00 07 00 01 EVT 11 LEN payload),
 * ACK each, and return how many side double-taps (0x2F = 1) there were.
 * eraser_set gets 0/1 from an explicit ERASER_TOGGLE (0x7F), else stays -1;
 * bat[0] and bat[1] get the battery level (BATTERY_STATUS 0x08, percent) and
 * charging state (CHARGING_STATUS 0x09) when reported, else stay.
 */
static int mcu_events(struct mcu *m, const unsigned char *b, int n, long long t, int *eraser_set,
		      int *bat)
{
	int taps = 0;

	for (int off = 0; off + 8 <= n; ) {
		const unsigned char *e = b + off;
		int len = e[7], code;

		if (e[0] != 0x02 || e[2] != 0x07 || e[4] != 0x01 || off + 8 + len > n)
			break;	/* not an event, or cut off at the end of the packet */
		code = ack_code(e[5]);
		if (code >= 0) {
			unsigned char ack[9] = { 0x07, 0x01, 0x02, 0x00, 0x01, 0x80, 0x11, 0x20,
						 (unsigned char)code };

			mcu_out(m, ack, sizeof(ack));
		}
		if (e[5] == 0x77 && m->seen_77++ == 0) {
			mcu_out(m, q7701, sizeof(q7701));
			if (verbose)
				printf("%lld mcu: second 0x7701\n", t);
		}
		if (e[5] == 0x2F && len && e[8] == 1)
			taps++;
		if (e[5] == 0x7F && len)
			*eraser_set = e[8] != 0;
		if (e[5] == 0x08 && len && e[8] <= 100)
			bat[0] = e[8];
		if (e[5] == 0x09 && len && e[8] <= 1)
			bat[1] = e[8];
		if (e[5] == 0x00 && len >= 1 && len <= 4) {	/* model id, little-endian */
			unsigned int id = 0;

			for (int i = 0; i < len; i++)
				id |= (unsigned int)e[8 + i] << (8 * i);
			printf("%lld pen model 0x%06x %s\n", t, id, pen_model(id));
		}
		if (e[5] == 0x03 && len) {			/* firmware version, ASCII */
			char s[64];
			int k;

			for (k = 0; k < len && k < (int)sizeof(s) - 1; k++)
				s[k] = e[8 + k] >= 0x20 && e[8 + k] < 0x7f ? e[8 + k] : '.';
			s[k] = 0;
			printf("%lld pen firmware \"%s\"\n", t, s);
		}
		if (verbose)
			printf("%lld mcu event 0x%02x = %02x\n", t, e[5], len ? e[8] : 0);
		off += 8 + len;
	}
	return taps;
}

int main(int argc, char **argv)
{
	struct pollfd pf[3];
	struct pen_rec rec;
	struct mcu mcu = { .fd = -1 };
	struct tilt_state ts;
	int tilt_x = 0, tilt_y = 0;
	long tsum_x = 0, tsum_y = 0, tn = 0;	/* -v: mean tilt of a stroke */
	unsigned char rep[64];
	int pen, hid, ui;
	int pressure = 0, in_range = 0, touching = 0, x = 0, y = 0;	/* x, y in 1/SUBPX px */
	int slots[4] = { 0 }, play = 4;	/* newest packet, next slot to play */
	int eraser = 0, tool = BTN_TOOL_PEN;
	int bat[2] = { -1, -1 }, bat_sent[2] = { -1, -1 };	/* level %, charging */
	long long press_t = 0, release_at = 0, valid_t = 0, out_t = 0, hint_t = 0, hid_retry_at = 0;
	long long slept = suspended_ms();
	double press_out = 0;	/* smoothed, mapped pressure */
	double edge_x = 0, edge_y = 0;	/* edge_smooth state */
	struct euro euro = { 0 };
	unsigned long frames = 0, reports = 0;

#ifndef NOLIBC
	setvbuf(stdout, NULL, _IOLBF, 0);	/* the log is a file or kmsg, not a tty */
#endif
	memset(&ts, 0, sizeof(ts));
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "-v"))
			verbose = 1;
		else if (!strcmp(argv[i], "-fx"))
			flip_x = !flip_x;
		else if (!strcmp(argv[i], "-fy"))
			flip_y = !flip_y;
		else if (!strcmp(argv[i], "-pt") && i + 1 < argc)
			press_tau_ms = atoi(argv[++i]);
		else if (!strcmp(argv[i], "-pf") && i + 1 < argc) {
			const char *s = argv[++i];

			pf_mincutoff = atoi(s);
			if ((s = strchr(s, ',')) != NULL) {
				pf_beta = atoi(++s);
				if ((s = strchr(s, ',')) != NULL)
					pf_dcutoff = atoi(++s);
			}
		}
		else if (!strcmp(argv[i], "-pr"))
			region_on = 1;
		else if (!strcmp(argv[i], "-lr"))
			region_on = 0;
		else if (!strcmp(argv[i], "-sx") && i + 1 < argc) {
			double scale = atof(argv[++i]);
			if (scale > 0.5 && scale < 1.5)
				screen_x_scale = scale;
		}
		else if (!strcmp(argv[i], "-rec") && i + 1 < argc)
			rec_fd = open(argv[++i], O_WRONLY | O_CREAT | O_APPEND, 0644);
		else if (!strcmp(argv[i], "-nt"))
			tilt_on = 0;
		else if (!strcmp(argv[i], "-tx"))
			tilt_sx = -tilt_sx;
		else if (!strcmp(argv[i], "-ty"))
			tilt_sy = -tilt_sy;
		else if (!strcmp(argv[i], "-pm") && i + 1 < argc)
			press_incell = strcmp(argv[++i], "direct") != 0;
		else if (!strcmp(argv[i], "-side") && i + 1 < argc) {
			i++;
			side_mode = !strcmp(argv[i], "button") ? SIDE_BUTTON :
				    !strcmp(argv[i], "none") ? SIDE_NONE : SIDE_ERASER;
		}
	}

	col_map = panel_is_boe();

	/* ueventd creates /dev/gk3_pen a moment after insmod: wait up to 10 s */
	for (int tries = 0; tries < 100; tries++) {
		pen = open("/dev/gk3_pen", O_RDWR | O_NONBLOCK);
		if (pen >= 0 || errno != ENOENT)
			break;
		usleep(100000);
	}
	hid = open_pressure_hidraw();
	ui = pen >= 0 && hid >= 0 ? uinput_create() : -1;
	if (pen < 0 || hid < 0 || ui < 0) {
		printf("open failed: gk3_pen %d pressure hidraw %d uinput %d (errno %d)\n",
		       pen, hid, ui, errno);
		return 1;
	}
	if (side_mode != SIDE_NONE)
		mcu_open(&mcu, now_ms());
	printf("gk3pend: running, flip x %d y %d, column pitch %s, filter %d,%d,%d, pressure %s tau %d ms, "
	       "tilt %s, side %s, mcu %s\n",
	       flip_x, flip_y, col_map ? "boe" : "even", pf_mincutoff, pf_beta, pf_dcutoff,
	       press_incell ? "incell" : "direct", press_tau_ms,
	       !tilt_on ? "off" : tilt_sx * tilt_sy > 0 ? (tilt_sx > 0 ? "on" : "on -tx -ty") :
	       tilt_sx > 0 ? "on -ty" : "on -tx",
	       side_mode == SIDE_ERASER ? "eraser" : side_mode == SIDE_BUTTON ? "button" : "none",
	       mcu.fd >= 0 ? "open" : "not found");

	pf[0].fd = pen;
	pf[0].events = POLLIN;
	pf[1].fd = hid;
	pf[1].events = POLLIN;
	pf[2].events = POLLOUT;		/* usbfs: a completed URB is waiting */

	for (;;) {
		int frame_valid = -1;	/* -1: no new frame this round */
		int reopened = 0;	/* pf[2].revents below belong to the old fd */
		long long t;

		pf[2].fd = mcu.fd;	/* negative fds are ignored by poll() */
		if (poll(pf, 3, in_range ? RELEASE_DELAY : 1000) < 0) {
			if (errno == EINTR)
				continue;
			printf("poll: %d\n", errno);
			return 1;
		}
		t = now_ms();

		/*
		 * Back from system suspend: gk3_pen_slave has the pen controller
		 * reset during resume (USB_QUIRK_RESET_RESUME), after which it
		 * sends no events until queried again.  Pressure needs nothing
		 * (usbhid survives the reset); the event channel is reopened.
		 */
		if (suspended_ms() - slept > 1000) {
			slept = suspended_ms();
			printf("%lld resumed: reopening mcu\n", t);
			if (mcu.fd >= 0) {
				mcu_close(&mcu, t);
				mcu_open(&mcu, t);
				reopened = 1;
			}
		}

		/* /dev/gk3_pen only fails when the driver goes away: let the service restart us */
		if (pf[0].revents & (POLLERR | POLLHUP | POLLNVAL)) {
			printf("gk3_pen: gone\n");
			return 1;
		}

		if (pf[1].revents) {
			int n, got = 0;

			while ((n = read(hid, rep, sizeof(rep))) > 0) {
				got = 1;
				if (n >= 11 && rep[0] == 0x55) {
					for (int k = 0; k < 4; k++)
						slots[k] = rep[3 + 2 * k] | (rep[4 + 2 * k] << 8);
					pressure = slots[3];
					play = 0;
					press_t = t;
					reports++;
				}
			}
			if ((n < 0 && errno != EAGAIN) || (pf[1].revents & (POLLERR | POLLHUP | POLLNVAL))) {
				/*
				 * The pen controller went away (USB reset or
				 * re-enumeration, e.g. around a resume): drop the
				 * stale pressure and look for its node again.
				 */
				printf("pressure: gone (%d)\n", n < 0 ? errno : 0);
				close(hid);
				hid = pf[1].fd = -1;
				pressure = 0;
				hid_retry_at = t + HID_RETRY_MS;
			} else if (got && t - hint_t >= 200) {
				/*
				 * Pressure reports flow as soon as the pen is picked
				 * up, pressure 0 until it touches: tell the driver the
				 * pen is in use so it leaves (or stays out of) its idle
				 * polling before the pen reaches the screen.  Old
				 * drivers have no write; the error is harmless.
				 */
				hint_t = t;
				write(pen, "u", 1);
			}
		}
		if (hid < 0 && t >= hid_retry_at) {
			hid = pf[1].fd = open_pressure_hidraw();
			if (hid < 0)
				hid_retry_at = t + HID_RETRY_MS;
		}

		if (mcu.fd >= 0 && pf[2].revents) {
			struct usbdevfs_urb *u;
			int taps = 0, eraser_set = -1;

			while (ioctl(mcu.fd, USBDEVFS_REAPURBNDELAY, &u) == 0) {
				/*
				 * A reset of the controller, as on every resume (even one
				 * too short for the check above), kills the URBs and drops
				 * our claim on the interface, and the controller then needs
				 * the handshake again: reopen rather than resubmit.
				 */
				if (u->status != 0) {
					printf("%lld mcu: urb status %d, reopening\n", t, u->status);
					mcu_close(&mcu, t);
					mcu_open(&mcu, t);
					reopened = 1;
					break;
				}
				taps += mcu_events(&mcu, u->buffer, u->actual_length, t, &eraser_set, bat);
				if (mcu_submit(&mcu, (int)(u - mcu.urb)) < 0) {
					printf("mcu: gone (%d)\n", errno);
					mcu_close(&mcu, t);
					break;
				}
			}
			/*
			 * A hangup on the fd we polled; after a reopen in this round
			 * it says nothing about the new one, which would otherwise be
			 * closed again and wait out MCU_RETRY_MS for nothing.
			 */
			if (!reopened && mcu.fd >= 0 && (pf[2].revents & (POLLERR | POLLHUP))) {
				printf("mcu: hangup\n");
				mcu_close(&mcu, t);
			}
			if (bat[0] != bat_sent[0] || bat[1] != bat_sent[1]) {
				unsigned char msg[3] = { 'B', bat[0] < 0 ? 0xff : bat[0],
							 bat[1] < 0 ? 0xff : bat[1] };

				write(pen, msg, sizeof(msg));
				bat_sent[0] = bat[0];
				bat_sent[1] = bat[1];
				printf("%lld battery %d%%, charging %d\n", t, bat[0], bat[1]);
			}
			if (side_mode == SIDE_ERASER) {
				if (taps & 1)
					eraser = !eraser;
				if (eraser_set >= 0)
					eraser = eraser_set;
				if (taps || eraser_set >= 0)
					printf("%lld side: %s\n", t, eraser ? "eraser" : "pen");
			} else if (side_mode == SIDE_BUTTON && taps && in_range) {
				emit(ui, EV_KEY, BTN_STYLUS, 1);
				emit(ui, EV_SYN, SYN_REPORT, 0);
				emit(ui, EV_KEY, BTN_STYLUS, 0);
				emit(ui, EV_SYN, SYN_REPORT, 0);
			}
		}
		if (mcu.fd < 0 && side_mode != SIDE_NONE && t >= mcu.retry_at)
			mcu_open(&mcu, t);

		if (pf[0].revents & POLLIN && read(pen, &rec, sizeof(rec)) == sizeof(rec)) {
			int ar = word(rec.d, 0) & 0xff, ac = word(rec.d, 1) & 0xff;
			int peak = word(rec.d, 2 + 40);	/* 9x9 centre cell = the anchor */

			frames++;
			frame_valid = rec.d[0] == 0x5a && rec.d[1] == 0xa5 &&
				      !(ar == 0xff && ac == 0xff) && peak >= PEAK_MIN;
			if (frame_valid) {
				int g[81], g2[81], lc, lr, col, row, k, c2, r2, s2;
				double fx, fy;
				int a2r = word(rec.d, 83) & 0xff, a2c = word(rec.d, 84) & 0xff;
				int press = pressure > 0 && t - press_t <= PRESS_MAX_AGE;

				for (k = 0; k < 81; k++) {
					g[k] = word(rec.d, 2 + k);
					g2[k] = word(rec.d, 85 + k);
				}
				if (region_on)
					solve_local_region(g, ar, ac, &lc, &lr, 1);
				else
					solve_local(g, ar, ac, &lc, &lr);
				/* window centre (cell 4) sits on the anchor cell */
				col = lc + (ac - 4) * UNIT;
				row = lr + (ar - 4) * UNIT;
				if (tilt_on) {
					/* a frame without a usable TX2 keeps the last tilt */
					if (!(a2r == 0xff && a2c == 0xff) &&
					    solve_tx2(g2, a2r, a2c, &c2, &r2, &s2))
						tilt_update(&ts, col, row, c2, r2, sum3x3(g, 4, 4), s2, press);
					tilt_revise(&ts, press, &col, &row);
				}
				/* sensor sign (diff = TX1 - TX2) -> raw axes: X is rows, Y is cols */
				tilt_x = (flip_x ? ts.out_r : -ts.out_r) * tilt_sx;
				tilt_y = (flip_y ? ts.out_c : -ts.out_c) * tilt_sy;
				if (rec_fd >= 0) {
					struct rec_out o = { rec, pressure, press, tilt_x, tilt_y };

					write(rec_fd, &o, sizeof(o));
				}
				/* uneven column pitch (solver.h); after the tilt, which works
				 * on the TX1 - TX2 offset in sensor columns */
				if (col_map)
					col = col_pitch_map(col);
				if (flip_x)
					row = 40 * UNIT - row;
				if (flip_y)
					col = 60 * UNIT - col;
				fx = row * 5.0 / 128;	/* 40 cells -> 1600 px */
				fy = col / 24.0;	/* 60 cells -> 2560 px */
				euro_filter(&euro, &fx, &fy, rec.t_ns,
					    !in_range || t - valid_t > 20 || (press && !touching));
				if (fx < 0) fx = 0;
				if (fx > 1599) fx = 1599;
				if (fy < 0) fy = 0;
				if (fy > 2559) fy = 2559;
				fx = edge_smooth(&edge_x, fx, EDGE_ZONE_X, 1599, t - valid_t > 20);
				fy = edge_smooth(&edge_y, fy, EDGE_ZONE_Y, 2559, t - valid_t > 20);
				/* Raw Y becomes horizontal in landscape. Identity unless
				 * this unit has a measured screen calibration. */
				fy = screen_x_calibrate(fy);
				if (fy < 0) fy = 0;
				if (fy > 2559) fy = 2559;
				x = (int)(fx * SUBPX + 0.5);
				y = (int)(fy * SUBPX + 0.5);
			}
		}

		if (frame_valid == 1) {
			/*
			 * The newest sample decides contact (EGoTouchRev lookaheadHoverGate),
			 * so a lift is never delayed; incell then plays the slots in order,
			 * falling back to the newest once they are used up or when the
			 * oldest is 0 (touch-down packet).
			 */
			int touch = pressure > 0 && t - press_t <= PRESS_MAX_AGE;
			int raw = press_incell && play < 4 && slots[0] ? slots[play] : pressure;
			int mapped = touch ? map_pressure(raw > 0 ? raw : pressure) : 0;

			if (play < 4)
				play++;

			/*
			 * The MCU reports pressure every 10-17 ms but we place the pen
			 * every ~4.4 ms, so the raw value moves in steps.  Low-pass it
			 * with a time constant (EGoTouchRev: alpha 0.5 per frame); like
			 * the original, never on touch-down or lift, which stay instant.
			 */
			if (mapped && press_out > 0 && press_tau_ms > 0) {
				double dt = t - out_t;

				if (dt < 1)
					dt = 1;
				press_out += (mapped - press_out) * dt / (press_tau_ms + dt);
			} else {
				press_out = mapped;
			}
			out_t = t;

			release_at = 0;
			valid_t = t;
			if (!in_range) {
				tool = eraser ? BTN_TOOL_RUBBER : BTN_TOOL_PEN;
				emit(ui, EV_KEY, tool, 1);
				in_range = 1;
				if (verbose)
					printf("%lld enter range (%s)\n", t, eraser ? "eraser" : "pen");
			}
			emit(ui, EV_ABS, ABS_X, x);
			emit(ui, EV_ABS, ABS_Y, y);
			if (tilt_on) {
				emit(ui, EV_ABS, ABS_TILT_X, tilt_x);
				emit(ui, EV_ABS, ABS_TILT_Y, tilt_y);
			}
			emit(ui, EV_ABS, ABS_PRESSURE, touch ? (press_out >= 1 ? (int)press_out : 1) : 0);
			if (touch != touching) {
				emit(ui, EV_KEY, BTN_TOUCH, touch);
				touching = touch;
				if (verbose) {
					printf("%lld %s at %d,%d p=%d", t, touch ? "down" : "up", x / SUBPX, y / SUBPX,
					       pressure);
					if (!touch && tn)
						printf(" tilt x %ld y %ld (%ld frames)",
						       tsum_x / tn, tsum_y / tn, tn);
					printf("\n");
				}
				tsum_x = tsum_y = tn = 0;
			}
			if (touch) {
				tsum_x += tilt_x;
				tsum_y += tilt_y;
				tn++;
			}
			emit(ui, EV_SYN, SYN_REPORT, 0);
		} else if (frame_valid == 0 && in_range && !release_at) {
			release_at = t + RELEASE_DELAY;
		}

		/* switch pen <-> eraser only while hovering, never mid-stroke */
		if (in_range && !touching && tool != (eraser ? BTN_TOOL_RUBBER : BTN_TOOL_PEN)) {
			emit(ui, EV_KEY, tool, 0);
			tool = eraser ? BTN_TOOL_RUBBER : BTN_TOOL_PEN;
			emit(ui, EV_KEY, tool, 1);
			emit(ui, EV_SYN, SYN_REPORT, 0);
		}

		if (in_range && !release_at && t - valid_t > STALE_MS)
			release_at = t;
		if (in_range && release_at && t >= release_at) {
			emit(ui, EV_ABS, ABS_PRESSURE, 0);
			emit(ui, EV_KEY, BTN_TOUCH, 0);
			emit(ui, EV_KEY, tool, 0);
			emit(ui, EV_SYN, SYN_REPORT, 0);
			if (verbose && touching && tn)
				printf("%lld lost while down, tilt x %ld y %ld (%ld frames)\n",
				       t, tsum_x / tn, tsum_y / tn, tn);
			tsum_x = tsum_y = tn = 0;
			in_range = touching = 0;
			release_at = 0;
			memset(&ts, 0, sizeof(ts));	/* no stale tilt into the next approach */
			if (verbose)
				printf("%lld leave range (frames %lu, U %lu)\n", t, frames, reports);
		}
	}
	return 0;
}
