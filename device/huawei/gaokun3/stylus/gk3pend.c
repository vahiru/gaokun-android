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
 * Pairing: a pen that is not paired with this tablet asks to pair when it is
 * put on the charging dock (event 0x15); gk3pend accepts the request with the
 * packets Huawei PC Manager sends on Windows (see q2e00 / q1501).
 *
 * Init params: when the controller reports the pen connected (event 0x71 =
 * 1, which the reply to each handshake also carries while a pen is connected)
 * gk3pend sends it this tablet's pen parameters (0x7D01), as Huawei's THP
 * service does on Windows (see q7d01).
 *
 * Connection: for the M-Pencil page in Settings gk3pend publishes whether a
 * pen is connected and its generation, and deletes the pairing when the page
 * asks (see q0000 / q1500 and pen_conn_update, unpair_check).
 *
 * Side touch: the pen reports a double-tap on its side as event 0x2F = 1
 * (single taps and long presses report nothing).  Windows uses it to switch
 * between pen and eraser; -side eraser (default) does the same, -side button
 * sends a BTN_STYLUS click instead, -side none ignores it (the controller is
 * still opened, for the battery).  A switch asked for while the tip is down
 * waits for the lift, so a stroke never changes tool.
 *
 * Settings (parts/StylusSettingsActivity.java): the filter's beta and what a
 * double-tap does come from vendor.gaokun3.pen_beta and vendor.gaokun3.pen_side,
 * which init copies from the persist.sys.gaokun3 ones the page writes
 * (gk3pend.rc).  They are read at start and again after any property changes,
 * so a change applies from the next frame; bionic builds only.  -pf with a
 * beta, or -side, on the command line wins.
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
#ifdef __BIONIC__
#include <sys/system_properties.h>
#endif
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
#define PAIR_ACCEPT_MS	1500	/* pairing: 0x2E, then this long before 0x1501 */
#define PAIR_MAX	3	/* pairing: requests answered per PAIR_WINDOW_MS */
#define PAIR_WINDOW_MS	60000
#define INIT_PARAM_GAP_MS	2000	/* init params: repeats within this are dropped, see q7d01 */
#define UNPAIRED_HOLD_MS	5000	/* unpaired-pen check off this long after a (re)open or resume */

static long long unpaired_hold;	/* unpaired-pen check off until then, see unpaired_frame() */
static long long nopress_ms;	/* writing at contact strength without pressure reports, ms */

/* the pressure path was lost, (re)opened or woke up: missing reports mean nothing for a while */
static void unpaired_pause(long long t)
{
	unpaired_hold = t + UNPAIRED_HOLD_MS;
	nopress_ms = 0;
}

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
	long long accept_at;		/* when to accept a pairing request (0x1501); 0: none due */
	long long pair_window;		/* start of the current PAIR_WINDOW_MS */
	int pair_n;			/* requests answered in it */
	unsigned int pair_model;	/* model id of the last request answered */
	long long init_sent;		/* when init params (q7d01) were last sent */
	int conn;			/* event 0x71: 1 a pen is connected, 0 none, -1 not reported yet */
	unsigned int model;		/* model id from event 0x00; 0: not reported yet */
	struct usbdevfs_urb urb[MCU_URBS];
	unsigned char buf[MCU_URBS][64];
};

static int verbose, flip_x, flip_y = 1;
static int press_tau_ms = 4;	/* -pt N: pressure smoothing time constant */
static int side_mode = -1;	/* -side; unset: from the settings, else SIDE_ERASER */
static int press_incell = 1;	/* -pm incell|direct */
static int tilt_on = 1, tilt_sx = 1, tilt_sy = 1;	/* -nt, -tx, -ty */
static int region_on = -1;	/* -pr strong peak region, -lr legacy; unset: strong except on BOE */
static int rec_fd = -1;					/* -rec */
static int col_map;	/* the column pitch in solver.h: PANEL_BOE, PANEL_CSOT, else even */

/*
 * The even sensor columns are read about 1 ms after the odd ones.  Y comes from
 * the columns next to the peak and X from the peak's own column, so on a moving
 * pen both swing with the column pair, in opposite senses, which only shows on
 * diagonals.  Fitted on free strokes: X reads x_us sin(pi col) late and Y -y_us
 * sin(pi col), col in cells.  Sampling is even at 240 Hz, but the time stamps
 * only say when the driver read a frame: they alternate 3.0 / 5.35 ms, and the
 * frame after a gap was sampled one slot after the last one and read late, so
 * the gap belongs to the step into the next frame.  If seq jumps the frames in
 * between really are gone and it belongs to this step.  Velocity is counted in
 * sample slots on that basis.  A stroke's first frames can be old data, so a
 * restart keeps the whole gap for the next step, which leaves them uncorrected.
 * Fitted on a BOE panel; one CSOT unit measured the same.  The legacy peak
 * region (solver.h) usually spans the whole row, so X also takes in the
 * neighbouring columns, which are read at the other time.  The strong region
 * is the peak column alone in about two thirds of frames and two columns
 * otherwise, and X reads about twice as late there: 600 instead of 294,
 * fitted on both panels.  Y changes little (517 and 494 fitted) and keeps 531.
 */
#define SLOT_NS		4166667ULL

static int pos_x_us = 294, pos_y_us = 531, sk_given;	/* -sk x,y; x is 600 with the strong region */

/* sin(pi * col / 1024) without libm: Bhaskara's fit, within 0.2 % */
static double sin_cell(int col)
{
	int p = col & 2047, q = p & 1023;
	double c = (double)q * (1024 - q);
	double s = 16 * c / (5.0 * 1024 * 1024 - 4 * c);

	return p < 1024 ? s : -s;
}

static void pos_skew(double *fx, double *fy, int col, unsigned long long t, unsigned int seq,
		     int restart, int skipped)
{
	static double hx[2], hy[2];
	static long hs[2], pend, carry;
	static unsigned long long last;
	static unsigned int lseq;
	static int n;
	double x = *fx, y = *fy;
	long slot = 0, k = (long)((t - last + SLOT_NS / 2) / SLOT_NS), step;
	int lost = !restart && seq - lseq > 1;

	if (k < 1)
		k = 1;
	step = 1 + pend + (lost ? k - 1 : 0);
	pend = lost ? 0 : k - 1;
	last = t;
	lseq = seq;
	if (skipped) {		/* only the time counts */
		carry += step;
		return;
	}
	if (restart)
		n = 0;
	if (n) {
		int j = n > 1;
		double ms, sn = sin_cell(col);

		slot = hs[0] + step + carry;
		ms = (double)(slot - hs[j]) * SLOT_NS / 1e6;
		*fx = x - (x - hx[j]) / ms * pos_x_us / 1000.0 * sn;
		*fy = y + (y - hy[j]) / ms * pos_y_us / 1000.0 * sn;
	}
	carry = 0;
	hx[1] = hx[0];
	hy[1] = hy[0];
	hs[1] = hs[0];
	hx[0] = x;
	hy[0] = y;
	hs[0] = slot;
	if (n < 2)
		n++;
}

/*
 * Now and then a pad keeps last frame's reading, some pads far more often than
 * others.  Harmless on a slow pen; on a fast one about three such frames in
 * ten land over 0.3 mm off the path and draw a kink, so leave them out.  Corners
 * repeat by chance more often and matter less.
 */
static int stale_on = 1;	/* -ns turns it off */

static int stale_frame(const int *g, int ar, int ac)
{
	static int prev[81], par = -100, pac = -100, held;
	int dr = ar - par, dc = ac - pac, i, j, hit = 0;

	if (dr >= -3 && dr <= 3 && dc >= -3 && dc <= 3)
		for (i = 3; i < 6; i++)
			for (j = 3; j < 6; j++) {
				int x = i + dr, y = j + dc;

				if (x >= 0 && x < 9 && y >= 0 && y < 9 &&
				    g[i * 9 + j] >= (i == 4 || j == 4 ? 100 : 300) &&
				    g[i * 9 + j] == prev[x * 9 + y])
					hit = 1;
			}
	memcpy(prev, g, sizeof(prev));
	par = ar;
	pac = ac;
	/* never two in a row: a pad stuck at one value must not stop the pen */
	hit = hit && !held;
	held = hit;
	return hit;
}

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
 * -sk X,Y sets the column timing correction in us (0,0 is off), -ns keeps
 * frames with a repeated reading (see pos_skew, stale_frame above).
 */
static int pf_mincutoff = 1000, pf_beta = -1, pf_dcutoff = 5000;	/* beta unset: from the settings, else 500 */
static int beta_given, side_given;	/* on the command line, so the settings stay out */

/* The settings (see the top of this file).  Returns 1 when one changed. */
static int read_settings(void)
{
	int beta = 500, side = SIDE_ERASER, changed;
#ifdef __BIONIC__
	static unsigned int serial;
	unsigned int s = __system_property_area_serial();
	char v[PROP_VALUE_MAX];

	if (s == serial && pf_beta >= 0 && side_mode >= 0)
		return 0;
	serial = s;
	if (__system_property_get("vendor.gaokun3.pen_beta", v) > 0 && atoi(v) >= 50 && atoi(v) <= 5000)
		beta = atoi(v);
	if (__system_property_get("vendor.gaokun3.pen_side", v) > 0)
		side = !strcmp(v, "button") ? SIDE_BUTTON : !strcmp(v, "none") ? SIDE_NONE : SIDE_ERASER;
#endif
	if (beta_given)
		beta = pf_beta;
	if (side_given)
		side = side_mode;
	changed = beta != pf_beta || side != side_mode;
	pf_beta = beta;
	side_mode = side;
	return changed;
}

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
enum { PANEL_UNKNOWN, PANEL_BOE, PANEL_CSOT };

static int panel_kind(void)
{
	char b[8] = { 0 };
	int fd = open("/sys/firmware/devicetree/base/soc@0/display-subsystem@ae00000/"
		      "dsi@ae94000/panel@0/compatible", O_RDONLY);

	if (fd < 0)
		return PANEL_UNKNOWN;
	read(fd, b, sizeof(b) - 1);
	close(fd);
	return !strncmp(b, "boe,", 4) ? PANEL_BOE : !strncmp(b, "csot,", 5) ? PANEL_CSOT : PANEL_UNKNOWN;
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
	unpaired_pause(t);
	/*
	 * After a reopen (a resume, a USB error) the request is stale: the MCU may
	 * have reset, and monotonic time stood still during a suspend.
	 */
	if (m->accept_at) {
		m->accept_at = 0;
		printf("%lld pen pairing request dropped: controller closed\n", t);
	}
}

/*
 * The factory handshake (EGoTouchRev BTMCU_PROTOCOL.md, from an API Monitor
 * capture): 0x7101, 0x7701, and a second 0x7701 after the 0x77 reply.  The
 * init params (q7d01) follow when the pen is reported connected, not on 0x7B:
 * the pen reports a 0x7B after every 0x7D01 (and after screen status changes),
 * so answering 0x7B with it loops at ~20 per second.
 */
static const unsigned char q7101[] = { 0x07, 0x00, 0x02, 0x00, 0x01, 0x71, 0x11, 0x00 };
static const unsigned char q7701[] = { 0x07, 0x00, 0x02, 0x00, 0x01, 0x77, 0x11, 0x00 };

/*
 * Pairing a pen that is not paired with this tablet.  Put on the charging dock,
 * it makes the MCU report a pairing request (0x15, payload: the pen's model
 * id) and wait for the host.  Huawei PC Manager (McuDevCo.dll) answers 0x2E at
 * once and shows a "pen found" dialog; on the user's OK it sends 0x1501
 * (0x1500 deletes the pairing, 0x1502 cancels), and the MCU reports the
 * result (0x16: 1 paired, 0 failed) before the pen connects as usual.  The
 * dialog closes without a 0x1501 when the MCU reports 0x29.  PC Manager only
 * connects model ids it knows, and before connecting it reports the pen
 * connected so far as gone, so one pen is connected at a time.  Here docking
 * a known model is the user's OK: the request is accepted PAIR_ACCEPT_MS after
 * the 0x2E unless 0x29 or a pairing result (0x16) comes first.  At most
 * PAIR_MAX requests are answered per PAIR_WINDOW_MS, in case the MCU keeps
 * asking after a failed pairing.  The MCU has not been seen to answer the
 * 0x2E itself.  Byte 1 stays 0 in these, as PC Manager sends them.
 */
static const unsigned char q2e00[] = { 0x07, 0x00, 0x02, 0x00, 0x01, 0x2E, 0x11, 0x00 };
static const unsigned char q1501[] = { 0x07, 0x00, 0x02, 0x00, 0x01, 0x15, 0x11, 0x01, 0x01 };

/*
 * Init params.  The pen keeps the last ones a host sent it, and every Huawei
 * device has its own (one table per product in the THP service's TSAPrmt.dll;
 * all gaokun panels share this one).  The controller passes 0x7D01 on to the
 * pen.  A pen that last got another device's values, or other values for this
 * one, times its tip signal so that the second panel scan of each pair (the
 * slave delivers two frames per display frame) reads only every other column
 * near the pen: that frame's position falls to the cell centre and the ink
 * zigzags by up to a column.  This lasts across tablet power-off, reboot and
 * re-pairing until a host sends values again.  Huawei's THP service on
 * Windows sends these on every pen connection ("hpp3 on"), so Windows always
 * sets them right.  Payload: 15 bytes LE, X Y A B C DisplayCycle (u16) N type
 * last (u8), from the Gaokun table "3333,3333,2e7,412,258,411a,0f,1"; its
 * token "0f" makes N and type (0F 00), and the service then sets offsets 13
 * and 14 (type, last) to 1.  Type 0, from the string alone, leaves the pen
 * zigzagging.  Rest of the 32-byte payload zero.  Sent on every 0x71 = 1
 * (a reconnect while the system slept only shows up in the reply to the
 * handshake after resume); INIT_PARAM_GAP_MS only drops repeats after a
 * successful send with no reopen or disconnect in between, so a failed send
 * is retried on the next 0x71 = 1.
 */
static const unsigned char q7d01[40] = {
	0x07, 0x01, 0x02, 0x00, 0x01, 0x7D, 0x11, 0x20,
	0x33, 0x33, 0x33, 0x33, 0xE7, 0x02, 0x12, 0x04,
	0x58, 0x02, 0x1A, 0x41, 0x0F, 0x01, 0x01,
};

/*
 * Connection, for the settings page.  The controller reports the pen's model
 * id (0x00) when a pen docks or connects, but not in its reply to the
 * handshake, so a pen connected before gk3pend started or across a suspend
 * would stay unknown.  PC Manager asks with an empty 0x00 whenever it has no
 * model id (McuDevCo [IsPenBaseInfoComplete]); gk3pend asks after each
 * handshake.  q1500 deletes the pairing, as PC Manager's [PenDeletePair]: the
 * controller then reports 0x71 = 0, and the pen only hovers until it is
 * docked and paired again.
 */
static const unsigned char q0000[] = { 0x07, 0x00, 0x02, 0x00, 0x01, 0x00, 0x11, 0x00 };
static const unsigned char q1500[] = { 0x07, 0x00, 0x02, 0x00, 0x01, 0x15, 0x11, 0x01, 0x00 };

static void pen_conn_update(const struct mcu *m);

static void mcu_open(struct mcu *m, long long t)
{
	int ifnum = MCU_IFNUM;

	m->seen_77 = 0;
	m->fd = mcu_find();
	if (m->fd < 0) {
		m->retry_at = t + MCU_RETRY_MS;
	} else if (ioctl(m->fd, USBDEVFS_CLAIMINTERFACE, &ifnum) < 0 ||
		   mcu_submit(m, 0) < 0 || mcu_submit(m, 1) < 0) {
		printf("mcu: claim/submit failed: %d\n", errno);
		mcu_close(m, t);
	}
	if (m->fd < 0) {
		/* no controller, no pen: the page stops offering it (a close alone keeps the state) */
		m->conn = -1;
		pen_conn_update(m);
		return;
	}
	/* every handshake may send the init params again (q7d01) */
	m->init_sent = -INIT_PARAM_GAP_MS;
	/* the MCU stays silent until queried (EGoTouchRev: THP_Service does this on start) */
	mcu_out(m, q7101, sizeof(q7101));
	mcu_out(m, q7701, sizeof(q7701));
	mcu_out(m, q0000, sizeof(q0000));
	unpaired_pause(t);
}

/* PEN_MODULE (event 0x00) model ids, from EGoTouchRev btmcu/PenModuleModelId.h */
static const char *pen_model(unsigned int id)
{
	switch (id) {
	case 0x00011a: return "CD52 (HPP2: not handled)";
	case 0x00011b: return "CD54";
	case 0x01011b: return "CD54R";
	case 0x443002: return "CD54S";
	case 0x533002:		/* the other M-Pencil 3 ids in PC Manager's table */
	case 0x543002:
	case 0x553002: return "M-Pencil 3";
	}
	return "unknown";
}

/*
 * Models a pairing request is accepted for: the 2nd and 3rd generation pens
 * (HPP3) of the table above, and the other M-Pencil 3 ids in PC Manager's
 * table (McuDevCo.dll: 0x533002, 0x543002, 0x553002).
 */
static int pen_pairable(unsigned int id)
{
	switch (id) {
	case 0x00011b:
	case 0x01011b:
	case 0x443002:
	case 0x533002:
	case 0x543002:
	case 0x553002:
		return 1;
	}
	return 0;
}

/* pen generation for the Parts notifications: 2, 3, or 0 if not known */
static int pen_generation(unsigned int id)
{
	return id == 0x00011b || id == 0x01011b ? 2 : pen_pairable(id) ? 3 : 0;
}

/*
 * Pen state for Parts, which polls these vendor.gaokun3.* properties
 * (StylusNotifier while the screen is on, StylusSettingsActivity while the
 * M-Pencil page is shown):
 *   pen_unpaired  1 while a pen is writing whose pressure does not reach us
 *                 (see UNPAIRED_* below), else 0
 *   pen_paired    "<ms>,<generation>" for each completed pairing (0x16 = 1);
 *                 the time keeps two pairings in one boot apart
 *   pen_nib_loose "<ms>" from when writing shows a loose nib (see NIB_*
 *                 below), a new value each time; 0 once it reads healthy again
 *   pen_conn      0 while no pen is connected (0x71 = 0, or not reported
 *                 yet), else the connected pen's generation: 2 or 3, 1 for
 *                 a model id that is not known or not reported yet
 */
static void pen_state(const char *name, const char *value)
{
#ifdef __BIONIC__
	char key[PROP_NAME_MAX];

	snprintf(key, sizeof(key), "vendor.gaokun3.%s", name);
	if (__system_property_set(key, value))
		printf("setprop %s failed\n", key);
#else
	(void)name;
	(void)value;
#endif
}

static void pen_state_paired(long long t, int generation)
{
#ifdef __BIONIC__
	char v[32];

	snprintf(v, sizeof(v), "%lld,%d", t, generation);
	pen_state("pen_paired", v);
#else
	(void)t;
	(void)generation;
#endif
}

/* pen_conn (see pen_state), set when it changes */
static void pen_conn_update(const struct mcu *m)
{
	static char last[4];
	char v[4];
	int gen = pen_generation(m->model);

	snprintf(v, sizeof(v), "%d", m->conn != 1 ? 0 : gen ? gen : 1);
	if (strcmp(v, last)) {
		strcpy(last, v);
		pen_state("pen_conn", v);
	}
}

/*
 * Disconnect, from the settings page: Parts sets sys.gaokun3.pen_unpair to 1
 * or 2, the other value each time, and stylus/gk3pend.rc copies it to
 * vendor.gaokun3.pen_unpair.  A change from the value last seen (at start,
 * the value then counts as seen) sends q1500.  The properties are not
 * persistent, so nothing is replayed at boot; a request while the controller
 * is closed for a reopen is dropped (the page still shows the pen, so it can
 * be asked again), and while it cannot be opened the page shows no pen.
 */
static void unpair_check(struct mcu *m, long long t)
{
#ifdef __BIONIC__
	static unsigned int serial;
	static char seen[PROP_VALUE_MAX] = "\1";
	unsigned int s = __system_property_area_serial();
	char v[PROP_VALUE_MAX] = "";

	if (s == serial)
		return;
	serial = s;
	__system_property_get("vendor.gaokun3.pen_unpair", v);
	if (seen[0] == 1 || !strcmp(v, seen)) {	/* first look: the current value is old */
		strcpy(seen, v);
		return;
	}
	strcpy(seen, v);
	if (m->fd < 0)
		printf("%lld pen disconnect dropped: controller closed\n", t);
	else if (mcu_out(m, q1500, sizeof(q1500)) < 0)
		printf("%lld pen disconnect failed: %d\n", t, errno);
	else
		printf("%lld pen disconnect: pairing deleted\n", t);
#else
	(void)m;
	(void)t;
#endif
}

/*
 * A pen writing on the screen while no pressure report arrives: one not paired
 * with this tablet, or connected to another device.  Its own signal still
 * reaches the touch slave, its pressure does not.  A paired pen sends pressure
 * reports (0 while hovering) from the moment it is picked up, so no report
 * for UNPAIRED_QUIET_MS together with UNPAIRED_MS of frames at contact
 * strength (TX1 anchor peak at least UNPAIRED_PEAK: hovering stays below
 * about 2500, contact is above about 3000) is taken as writing with an
 * unpaired pen.  With a second pen paired, its reports go on for about 20 s
 * after it is put down, so the check fires only once it has gone quiet.  Off
 * while the pressure node or the MCU is closed and for UNPAIRED_HOLD_MS after
 * either was (re)opened or the system resumed, when missing reports mean
 * nothing.  Cleared by a pressure report, a completed pairing, a resume, or
 * UNPAIRED_KEEP_MS (monotonic) without such frames.  A pen with a flat
 * battery sends no signal at all.
 */
#define UNPAIRED_PEAK		3000
#define UNPAIRED_QUIET_MS	2000
#define UNPAIRED_MS		2000
#define UNPAIRED_KEEP_MS	600000
static int unpaired;
static long long nopress_last;

static void unpaired_clear(long long t, const char *why)
{
	nopress_ms = 0;
	if (unpaired) {
		unpaired = 0;
		pen_state("pen_unpaired", "0");
		printf("%lld unpaired pen hint off: %s\n", t, why);
	}
}

/* a valid frame; last_report is when the last pressure report arrived */
static void unpaired_frame(long long t, int peak, long long last_report)
{
	if (peak < UNPAIRED_PEAK || t - last_report < UNPAIRED_QUIET_MS)
		return;
	if (t - nopress_last <= 50)	/* frames ~4 ms apart while the pen is down */
		nopress_ms += t - nopress_last;
	nopress_last = t;
	if (!unpaired && nopress_ms >= UNPAIRED_MS) {
		unpaired = 1;
		pen_state("pen_unpaired", "1");
		printf("%lld unpaired pen hint on: writing without pressure reports\n", t);
	}
}

/*
 * A loose nib.  The tip electrode reaches the glass through the nib: screwed
 * loose, its TX1 anchor peak in contact falls to a quarter or less (median
 * ~3300 -> 400-1000 on a CD54R) while the ring electrode's TX2 stays (~1000),
 * and the position noise triples.  Pressure still works, so the pen seems
 * fine apart from the jitter.  On a CD54R the nib reads healthy up to a full
 * turn out; at about 1 1/4 turns TX1 drops within a stroke and the ink goes
 * wrong.  Healthy strokes (BOE and CSOT, 2nd and 3rd generation) read TX1
 * 2000-9000 at TX2/TX1 0.08-0.6; a few single ones go below 1500 or above 1,
 * never both.  A frame in contact counts as weak with TX1 under NIB_PEAK and
 * TX2 over NIB_RATIO percent of it, a stroke of at least NIB_STROKE_MIN
 * frames as loose when most of its frames are.  The bar is low because a nib that
 * loose also drops frames under PEAK_MIN, which breaks strokes into short
 * pieces.  A loose nib still seats now and then for a stroke or two, so
 * NIB_SET loose strokes among the last NIB_WIN raise pen_nib_loose and
 * NIB_CLEAR or fewer clear it.  On the recordings at hand this flags a loose
 * nib on the 6th or 7th stroke after the drop, clears it on the 7th stroke
 * after tightening, and judges none of 1347 healthy strokes loose.
 * Not reset on resume: only writing clears it, or a restart of gk3pend, which
 * sets pen_nib_loose to 0 and starts over (a nib still loose is flagged again
 * with a new value after NIB_WIN strokes).
 */
#define NIB_PEAK	1500
#define NIB_RATIO	80
#define NIB_STROKE_MIN	10	/* frames, ~40-50 ms */
#define NIB_GAP_MS	100	/* no valid frame for longer than this ends a stroke */
#define NIB_WIN		8
#define NIB_SET		6
#define NIB_CLEAR	1
static struct {
	int frames, weak;	/* the stroke going on */
	unsigned int last;	/* one bit per stroke, newest in bit 0 */
	int strokes, on;
} nib;

static void nib_stroke_end(long long t)
{
	int loose;

	if (nib.frames >= NIB_STROKE_MIN) {
		nib.last = (nib.last << 1 | (2 * nib.weak > nib.frames)) & ((1u << NIB_WIN) - 1);
		if (nib.strokes < NIB_WIN)
			nib.strokes++;
		loose = __builtin_popcount(nib.last);
		if (!nib.on && nib.strokes == NIB_WIN && loose >= NIB_SET) {
			char v[24];

			nib.on = 1;
			snprintf(v, sizeof(v), "%lld", t);
			pen_state("pen_nib_loose", v);
			printf("%lld loose nib hint on: %d of the last %d strokes weak at the tip\n",
			       t, loose, NIB_WIN);
		} else if (nib.on && loose <= NIB_CLEAR) {
			nib.on = 0;
			pen_state("pen_nib_loose", "0");
			printf("%lld loose nib hint off\n", t);
		}
	}
	nib.frames = nib.weak = 0;
}

/* a valid frame in contact: TX1 and TX2 anchor peaks, tx2 0 without a TX2 */
static void nib_frame(int tx1, int tx2)
{
	nib.frames++;
	nib.weak += tx1 < NIB_PEAK && tx2 * 100 > tx1 * NIB_RATIO;
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
		if (e[5] == 0x71 && len) {
			m->conn = e[8] == 1;
			pen_conn_update(m);
		}
		if (e[5] == 0x71 && len && e[8] == 0)
			m->init_sent = -INIT_PARAM_GAP_MS;	/* the next connection gets them */
		if (e[5] == 0x71 && len && e[8] == 1 && t - m->init_sent >= INIT_PARAM_GAP_MS) {
			if (mcu_out(m, q7d01, sizeof(q7d01)) < 0) {
				printf("%lld pen connected: init params send failed: %d\n", t, errno);
			} else {
				m->init_sent = t;
				printf("%lld pen connected: init params sent\n", t);
			}
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
			m->model = id;
			pen_conn_update(m);
			printf("%lld pen model 0x%06x %s\n", t, id, pen_model(id));
		}
		if (e[5] == 0x15 && !m->accept_at) {	/* pairing request */
			unsigned int id = 0;
			const char *what = "accepting";

			for (int i = 0; i < len && i < 4; i++)
				id |= (unsigned int)e[8 + i] << (8 * i);
			if (t - m->pair_window >= PAIR_WINDOW_MS) {
				m->pair_window = t;
				m->pair_n = 0;
			}
			if (!pen_pairable(id)) {
				what = "not a supported pen, ignored";
			} else if (m->pair_n >= PAIR_MAX) {
				what = "too many requests, ignored for now";
			} else {
				int r = mcu_out(m, q2e00, sizeof(q2e00));

				m->pair_n++;
				m->pair_model = id;
				m->accept_at = t + PAIR_ACCEPT_MS;
				if (r < 0)
					what = "accepting (0x2E send failed)";
			}
			printf("%lld pen pairing request, model 0x%06x %s: %s\n", t, id, pen_model(id), what);
		}
		/* 0x16 counts as PC Manager reads it: a payload of 0 or 1 */
		if ((e[5] == 0x29 || (e[5] == 0x16 && len && e[8] < 2)) && m->accept_at) {
			m->accept_at = 0;
			printf("%lld pen pairing acceptance cancelled by event 0x%02x\n", t, e[5]);
		}
		if (e[5] == 0x16 && len) {
			printf("%lld pen pairing %s\n", t,
			       e[8] == 1 ? "done" : e[8] == 0 ? "failed" : "result unknown");
			if (e[8] == 1) {
				/* hint off first: Parts reads pen_paired before pen_unpaired */
				unpaired_clear(t, "pen paired");
				pen_state_paired(t, pen_generation(m->pair_model));
			}
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
	struct mcu mcu = { .fd = -1, .init_sent = -INIT_PARAM_GAP_MS, .conn = -1 };
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
		else if (!strcmp(argv[i], "-rec") && i + 1 < argc)
			rec_fd = open(argv[++i], O_WRONLY | O_CREAT | O_APPEND, 0644);
		else if (!strcmp(argv[i], "-nt"))
			tilt_on = 0;
		else if (!strcmp(argv[i], "-ns"))
			stale_on = 0;
		else if (!strcmp(argv[i], "-sk") && i + 1 < argc) {
			const char *sk = argv[++i];

			pos_x_us = atoi(sk);
			if ((sk = strchr(sk, ',')) != NULL)
				pos_y_us = atoi(sk + 1);
			sk_given = 1;
		}
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
		else if (!strcmp(argv[i], "-pr"))
			region_on = 1;
		else if (!strcmp(argv[i], "-lr"))
			region_on = 0;
	}

	beta_given = pf_beta >= 0;
	side_given = side_mode >= 0;
	read_settings();
	col_map = panel_kind();
	pitch = col_map == PANEL_CSOT ? pitch_csot : pitch_boe;

	pen_state("pen_unpaired", "0");	/* an earlier instance may have left 1 */
	pen_state("pen_nib_loose", "0");
	pen_state("pen_conn", "0");
	unpair_check(&mcu, 0);		/* the first look takes the current request as old */

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
	mcu_open(&mcu, now_ms());
	unpaired_pause(now_ms());
	/* on BOE the strong region bends gentle curves further off the path */
	if (region_on < 0)
		region_on = col_map != PANEL_BOE;
	if (region_on && !sk_given)
		pos_x_us = 600;
	printf("gk3pend: running, flip x %d y %d, column pitch %s, peak region %s, filter %d,%d,%d, "
	       "pos skew %d,%d us, stale frames %s, pressure %s tau %d ms, tilt %s, side %s, mcu %s\n",
	       flip_x, flip_y, col_map == PANEL_BOE ? "boe" : col_map == PANEL_CSOT ? "csot" : "even",
	       region_on ? "strong" : "legacy",
	       pf_mincutoff, pf_beta, pf_dcutoff,
	       pos_x_us, pos_y_us, stale_on ? "skipped" : "kept",
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
		int timeout = in_range ? RELEASE_DELAY : 1000;
		long long t;

		if (mcu.accept_at) {	/* wake up for a pairing acceptance that falls due */
			long long d = mcu.accept_at - now_ms();

			if (d < timeout)
				timeout = d > 0 ? (int)d : 0;
		}
		pf[2].fd = mcu.fd;	/* negative fds are ignored by poll() */
		if (poll(pf, 3, timeout) < 0) {
			if (errno == EINTR)
				continue;
			printf("poll: %d\n", errno);
			return 1;
		}
		t = now_ms();
		if (read_settings()) {
			if (side_mode != SIDE_ERASER)
				eraser = 0;	/* back to the pen: now if hovering, at the lift if the tip is down */
			printf("%lld settings: filter beta %d, side %s\n", t, pf_beta,
			       side_mode == SIDE_ERASER ? "eraser" : side_mode == SIDE_BUTTON ? "button" : "none");
		}

		/*
		 * Back from system suspend: the pen controller holds its reports
		 * per interface until the host talks to it there.  usbhid does
		 * that for pressure (HID_QUIRK_IDLE_ON_RESUME); the event channel
		 * is ours, and the OUT of the handshake after reopening frees it.
		 */
		if (suspended_ms() - slept > 1000) {
			slept = suspended_ms();
			printf("%lld resumed: reopening mcu\n", t);
			unpaired_clear(t, "system resumed");
			unpaired_pause(t);
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
					if (unpaired || nopress_ms)
						unpaired_clear(t, "pressure reports");
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
				unpaired_pause(t);
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
			else
				unpaired_pause(t);
		}

		if (mcu.fd >= 0 && pf[2].revents) {
			struct usbdevfs_urb *u;
			int taps = 0, eraser_set = -1;

			while (ioctl(mcu.fd, USBDEVFS_REAPURBNDELAY, &u) == 0) {
				/*
				 * A system suspend, even one too short for the check above,
				 * kills the URBs, and the controller then needs the handshake
				 * again: reopen rather than resubmit.
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
		if (mcu.fd < 0 && t >= mcu.retry_at)
			mcu_open(&mcu, t);
		unpair_check(&mcu, t);
		if (unpaired && t - nopress_last > UNPAIRED_KEEP_MS)
			unpaired_clear(t, "no such writing for a while");
		if (mcu.accept_at && t >= mcu.accept_at) {
			mcu.accept_at = 0;
			if (mcu.fd >= 0) {
				int r = mcu_out(&mcu, q1501, sizeof(q1501));

				if (r < 0)
					printf("%lld pen pairing accept failed: %d\n", t, errno);
				else
					printf("%lld pen pairing accepted\n", t);
			}
		}

		if (pf[0].revents & POLLIN && read(pen, &rec, sizeof(rec)) == sizeof(rec)) {
			int ar = word(rec.d, 0) & 0xff, ac = word(rec.d, 1) & 0xff;
			int peak = word(rec.d, 2 + 40);	/* 9x9 centre cell = the anchor */

			frames++;
			frame_valid = rec.d[0] == 0x5a && rec.d[1] == 0xa5 &&
				      !(ar == 0xff && ac == 0xff) && peak >= PEAK_MIN;
			if (frame_valid) {
				int g[81], g2[81], lc, lr, col, row, col0, k, c2, r2, s2, restart, skip;
				double fx, fy;
				int a2r = word(rec.d, 83) & 0xff, a2c = word(rec.d, 84) & 0xff;
				int press = pressure > 0 && t - press_t <= PRESS_MAX_AGE;

				if (hid >= 0 && mcu.fd >= 0 && t >= unpaired_hold)
					unpaired_frame(t, peak, press_t);

				for (k = 0; k < 81; k++) {
					g[k] = word(rec.d, 2 + k);
					g2[k] = word(rec.d, 85 + k);
				}
				/* loose nib: a stroke ends at a lift or a gap in the frames */
				if (nib.frames && (!press || t - valid_t > NIB_GAP_MS))
					nib_stroke_end(t);
				if (press)
					nib_frame(peak, a2r == 0xff && a2c == 0xff ? 0 : g2[40]);
				restart = !in_range || t - valid_t > 20 || (press && !touching);
				skip = stale_on && stale_frame(g, ar, ac) && !restart;
				if (region_on)
					solve_local_region(g, ar, ac, &lc, &lr, 1);
				else
					solve_local(g, ar, ac, &lc, &lr);
				/* window centre (cell 4) sits on the anchor cell */
				col = lc + (ac - 4) * UNIT;
				row = lr + (ar - 4) * UNIT;
				col0 = col;
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
				if (pos_x_us || pos_y_us)
					pos_skew(&fx, &fy, col0, rec.t_ns, rec.seq, restart, skip);
				if (skip)
					goto position_done;
				euro_filter(&euro, &fx, &fy, rec.t_ns, restart);
				if (fx < 0) fx = 0;
				if (fx > 1599) fx = 1599;
				if (fy < 0) fy = 0;
				if (fy > 2559) fy = 2559;
				fx = edge_smooth(&edge_x, fx, EDGE_ZONE_X, 1599, t - valid_t > 20);
				fy = edge_smooth(&edge_y, fy, EDGE_ZONE_Y, 2559, t - valid_t > 20);
				x = (int)(fx * SUBPX + 0.5);
				y = (int)(fy * SUBPX + 0.5);
position_done:
				;
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
