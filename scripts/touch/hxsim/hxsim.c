// SPDX-License-Identifier: GPL-2.0
/*
 * hxsim - replay a gk3trec recording through the driver's own hx-algo.c.
 *
 * build.sh compiles this file together with hx-algo.c from a kernel tree,
 * so the replay always runs the algorithm that tree would ship.
 *
 * Usage: hxsim [-r [-F]] [-p params.txt] [-s name=value]... FILE
 *   -r  feed frame_raw through hx_preprocess_frame() (the full pipeline,
 *       CMF and IIR included) instead of the recorded processed grid.  Each
 *       frame line then counts the cells that differ from the grid the
 *       driver produced on the device, which shows whether the replay is
 *       faithful.
 *   -F  with -r, take the firmware's finger flag from the recorded grid: an
 *       all-zero grid counts as flag off.  On the device such a frame is
 *       blanked, which also removes the last frame of a lift; without -F the
 *       replay keeps the centroid jumps of those frames and overstates how
 *       many taps move under a rule that does not look at the signal.
 *   -p  apply name=value tunables, e.g. a dump of the files in
 *       /sys/bus/spi/devices/spi0.0/algo/
 *   -s  apply one tunable (after -p)
 *   Without -F baseline_fw_finger is replayed as 0: recordings do not carry
 *   the firmware's finger flag.
 *
 * Output, one block per frame:
 *   F t_ns idx stable report_on mism
 *   Z area signal_sum palm rule min_r max_r min_c max_c
 *   K r c z zone_area                       peaks that survived the filters
 *   C x y area signal_sum zone_area edge    contacts (grid output space)
 *   T slot tid rep X Y x y debounce missed age tool
 *                                           active tracks; X/Y as the
 *                                           kernel reports them, x/y in
 *                                           the grid output space; tool
 *                                           0 = finger, 2 = MT_TOOL_PALM
 *
 * The hand-map counters go to stderr at the end.
 *
 * The frame handler of himax-spi-core.c (touch_active, report_on) and the
 * input core's tracking-id assignment are reproduced here, because a
 * contact that flickers off for one frame comes back as a new touch.
 *
 * The recorder drops idle grids, but the driver handles a frame every
 * 8.3 ms whether or not anything touches the panel, and the tracker and
 * the hand map count frames.  A gap of more than IDLE_GAP_NS between two
 * recorded grids is therefore replayed as that many empty frames (not
 * printed); shorter gaps are grids the recorder missed while something was
 * down, and are left alone.
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hx-algo.h"

#define REC_FRAME	1
#define REC_FRAME_RAW	2

#define FRAME_NS	8333333LL
#define IDLE_GAP_NS	30000000LL
#define IDLE_MAX	1200		/* 10 s; everything has expired by then */

/* touchscreen-size-x = 2560, touchscreen-inverted-x, touchscreen-swapped-x-y */
#define TS_MAX_X	2559

static struct hx_algo algo;
static int slot_tid[HIMAX_MAX_TOUCH];
static int next_tid;

enum kind { K_BOOL, K_U8, K_U16, K_S16, K_S32 };

struct param {
	const char *name;
	void *ptr;
	enum kind kind;
	long max;	/* < 0: no limit */
};

/* Every tunable under algo/ in himax-spi-core.c (stats and contacts_log aside). */
#define P(f, k) { #f, &algo.f, k, -1 }
/* the driver's sysfs rejects values above these (hx-algo.h) */
#define PM(f, k, m) { #f, &algo.f, k, m }
static struct param params[] = {
	P(cmf_enabled, K_BOOL), P(cmf_exclusion, K_S16), P(cmf_max_correction, K_S16),
	P(iir_enabled, K_BOOL), P(iir_decay_weight, K_U16), P(iir_decay_step, K_U16),
	P(iir_noise_floor, K_S16), P(iir_gate_floor, K_S16), P(iir_gate_ratio_q8, K_U8),
	P(macro_threshold, K_S16), P(peak_threshold, K_S16), P(iso_nbr_ratio_q8, K_U16),
	P(edge_min_area, K_U8), P(palm_enabled, K_BOOL), P(palm_zone_scan, K_BOOL),
	P(palm_contact_area, K_U16), P(palm_area_threshold, K_U8),
	P(palm_signal_threshold, K_S32), P(palm_density_low, K_S16),
	P(hand_enabled, K_BOOL), PM(hand_margin, K_U8, HX_HAND_MARGIN_MAX),
	PM(hand_hold_frames, K_U8, HX_HAND_HOLD_MAX),
	PM(hand_land_frames, K_U8, HX_HAND_LAND_FRAMES_MAX),
	PM(hand_land_dist, K_U8, HX_HAND_LAND_DIST_MAX),
	P(pressure_enabled, K_BOOL), P(edge_comp_enabled, K_BOOL),
	P(edge_boost_pct, K_S16), P(edge_push_q8, K_S16), P(edge_blend_q8, K_S16),
	P(track_dist2_max, K_S32), P(track_lost_frames, K_U8),
	P(track_fw_lost_frames, K_U8), P(debounce_base, K_U8),
	P(track_smoothing, K_BOOL), P(track_active_guard, K_BOOL),
	P(track_start_debounce, K_U8), P(track_jump_dist2, K_S32),
	P(track_tap_radius, K_S32), P(track_tap_speed, K_S32),
	P(track_tap_hard, K_S32), P(track_tap_fall, K_S32), P(track_tap_snap, K_BOOL),
	P(track_filter_mincutoff, K_S32),
	P(track_filter_beta, K_S32), P(track_filter_dcutoff, K_S32),
	PM(baseline_mode, K_U8, 2), P(baseline_fw_finger, K_BOOL),
	P(baseline_gate, K_S16), PM(cmf_mode, K_U8, 3),
	P(peak_edge_threshold, K_S16), P(peak_drift_limit, K_S16),
	PM(peak_merge_radius, K_U8, 5), P(min_area, K_U8),
	PM(edge_comp_mode, K_U8, 1), PM(edge_peak_mode, K_U8, 1),
	P(zone_split, K_BOOL), P(ghost_row_filter, K_BOOL),
	P(debounce_strong, K_S32), P(track_min_signal, K_S32),
};

static int set_param(const char *assign)
{
	const char *eq = strchr(assign, '=');
	size_t i, n;
	long v;

	if (!eq)
		return -1;
	n = eq - assign;
	v = strtol(eq + 1, NULL, 0);
	for (i = 0; i < sizeof(params) / sizeof(params[0]); i++) {
		struct param *p = &params[i];

		if (strlen(p->name) != n || strncmp(p->name, assign, n))
			continue;
		if (p->max >= 0 && (v < 0 || v > p->max)) {
			fprintf(stderr, "hxsim: %s must be 0..%ld, as the driver enforces\n",
				p->name, p->max);
			return -1;
		}
		switch (p->kind) {
		case K_BOOL: *(bool *)p->ptr = v != 0; break;
		case K_U8:   *(u8 *)p->ptr = v; break;
		case K_U16:  *(u16 *)p->ptr = v; break;
		case K_S16:  *(s16 *)p->ptr = v; break;
		case K_S32:  *(s32 *)p->ptr = v; break;
		}
		return 0;
	}
	fprintf(stderr, "hxsim: unknown tunable \"%.*s\"\n", (int)n, assign);
	return -1;
}

static void load_params(const char *path)
{
	char line[256];
	FILE *f = fopen(path, "r");

	if (!f) {
		perror(path);
		exit(1);
	}
	while (fgets(line, sizeof(line), f)) {
		line[strcspn(line, "\r\n")] = 0;
		if (line[0] && line[0] != '#' && set_param(line))
			exit(1);
	}
	fclose(f);
}

/* One pass of himax_ts_thread() after the frame has been prepared. */
static void run_frame(long long t_ns, long idx, int mism, bool quiet)
{
	struct input_mt_pos pos[HIMAX_MAX_TOUCH];
	bool report_on = true;
	int cnt, stable, i;
	s32 rx, ry;

	hx_detect_macro_zones(&algo);
	hx_reject_palms(&algo);
	hx_hand_update(&algo);
	hx_detect_peaks(&algo);
	hx_hand_filter(&algo);
	hx_expand_and_resolve(&algo, pos, &cnt);
	hx_track_contacts(&algo, pos, cnt);
	stable = hx_count_stable_tracks(&algo);

	if (stable > 0 && !algo.touch_active) {
		algo.touch_start_frames++;
		if (algo.touch_start_frames < algo.track_start_debounce)
			report_on = false;
		else
			algo.touch_active = true;
	} else if (stable == 0) {
		algo.touch_start_frames = 0;
		algo.touch_active = false;
	}

	if (quiet) {
		/* replayed idle frame: keep the tracking-id state, print nothing */
		for (i = 0; i < HIMAX_MAX_TOUCH; i++)
			if (!(report_on && algo.tracks[i].active && !algo.tracks[i].debounce))
				slot_tid[i] = -1;
		return;
	}

	printf("F %lld %ld %d %d %d\n", t_ns, idx, stable, report_on, mism);
	for (i = 0; i < algo.zone_count; i++) {
		struct hx_macro_zone *z = &algo.zones[i];

		printf("Z %u %d %d %u %u %u %u %u\n", z->area, z->signal_sum,
		       z->is_palm, z->palm_rule, z->min_r, z->max_r, z->min_c, z->max_c);
	}
	for (i = 0; i < algo.peak_count; i++) {
		struct hx_peak *p = &algo.peaks[i];

		printf("K %u %u %d %u\n", p->r, p->c, p->z, p->zone_area);
	}
	for (i = 0; i < algo.contact_count; i++) {
		struct hx_contact *c = &algo.contacts[i];

		printf("C %d %d %u %d %u %d\n", pos[i].x, pos[i].y, c->area,
		       c->signal_sum, c->zone_area, c->is_edge);
	}
	for (i = 0; i < HIMAX_MAX_TOUCH; i++) {
		struct hx_track *t = &algo.tracks[i];
		bool on = report_on && t->active && t->debounce == 0;

		/* input_mt_report_slot_state(): a slot coming back on gets a new id */
		if (!on)
			slot_tid[i] = -1;
		else if (slot_tid[i] < 0)
			slot_tid[i] = next_tid++;
		if (!t->active)
			continue;
		/* touchscreen_report_pos(): invert x, then swap x and y */
		hx_track_report_pos(&algo, t, &rx, &ry);
		printf("T %d %d %d %d %d %d %d %u %u %u %d\n", i, on ? slot_tid[i] : -1,
		       on, ry, TS_MAX_X - rx, t->x, t->y, t->debounce,
		       t->missed, t->age, t->palm ? 2 : 0);
	}
}

/* Replay the empty frames the recorder skipped before a grid at t_ns. */
static int flag_grid;

static void fill_idle(long long *last_t, long long t_ns, bool raw_mode)
{
	static u16 idle_raw[HX_ROWS * HX_COLS];
	long long n, k;

	if (*last_t && t_ns - *last_t > IDLE_GAP_NS) {
		n = (t_ns - *last_t) / FRAME_NS - 1;
		if (n > IDLE_MAX)
			n = IDLE_MAX;
		for (k = 0; k < HX_ROWS * HX_COLS; k++)
			idle_raw[k] = 0x7ffe;
		for (k = 1; k <= n; k++) {
			if (raw_mode) {
				algo.fw_finger = false;
				hx_preprocess_frame(&algo, idle_raw);
			} else
				memset(algo.frame, 0, sizeof(algo.frame));
			run_frame(*last_t + k * FRAME_NS, -1, -1, true);
		}
	}
	*last_t = t_ns;
}

int main(int argc, char **argv)
{
	const char *path = NULL, *pfile = NULL;
	const char *sets[64];
	int nsets = 0, raw_mode = 0, i;
	static s16 grid[HX_ROWS * HX_COLS];
	static u16 rawbuf[HX_ROWS * HX_COLS];
	long long grid_t = 0, last_t = 0;
	long idx = 0;
	unsigned char *data;
	size_t len, off;
	FILE *f;

	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "-r"))
			raw_mode = 1;
		else if (!strcmp(argv[i], "-F"))
			flag_grid = 1;
		else if (!strcmp(argv[i], "-p") && i + 1 < argc)
			pfile = argv[++i];
		else if (!strcmp(argv[i], "-s") && i + 1 < argc && nsets < 64)
			sets[nsets++] = argv[++i];
		else if (!path)
			path = argv[i];
		else {
			fprintf(stderr, "usage: hxsim [-r [-F]] [-p params] [-s name=value]... FILE\n");
			return 2;
		}
	}
	if (!path) {
		fprintf(stderr, "usage: hxsim [-r [-F]] [-p params] [-s name=value]... FILE\n");
		return 2;
	}

	hx_algo_init_defaults(&algo);
	if (pfile)
		load_params(pfile);
	for (i = 0; i < nsets; i++)
		if (set_param(sets[i]))
			return 1;
	/*
	 * Recordings carry no firmware finger flag: fw_finger stays false,
	 * and with baseline_fw_finger every frame would read as untouched
	 * and come out empty.  Replay the baseline without the flag, unless
	 * -F derives it from the recorded grids.
	 */
	if (flag_grid && !raw_mode) {
		fprintf(stderr, "hxsim: -F needs -r\n");
		return 2;
	}
	if (algo.baseline_fw_finger && !flag_grid) {
		fprintf(stderr, "hxsim: no finger flag in recordings, replaying with baseline_fw_finger 0\n");
		algo.baseline_fw_finger = false;
	}
	for (i = 0; i < HIMAX_MAX_TOUCH; i++)
		slot_tid[i] = -1;

	f = fopen(path, "rb");
	if (!f) {
		perror(path);
		return 1;
	}
	fseek(f, 0, SEEK_END);
	len = ftell(f);
	fseek(f, 0, SEEK_SET);
	data = malloc(len);
	if (!data || fread(data, 1, len, f) != len) {
		fprintf(stderr, "hxsim: cannot read %s\n", path);
		return 1;
	}
	fclose(f);
	if (len < 16 || memcmp(data, "GK3TREC1", 8)) {
		fprintf(stderr, "hxsim: %s is not a gk3trec file\n", path);
		return 1;
	}

	for (off = 16; off + 16 <= len;) {
		unsigned short type, rlen;
		long long t_ns;

		memcpy(&type, data + off, 2);
		memcpy(&rlen, data + off + 2, 2);
		memcpy(&t_ns, data + off + 8, 8);
		off += 16;
		if (off + rlen > len)
			break;

		if (type == REC_FRAME && rlen == sizeof(grid)) {
			memcpy(grid, data + off, rlen);
			grid_t = t_ns;
			if (!raw_mode) {
				fill_idle(&last_t, t_ns, false);
				memcpy(algo.frame, grid, sizeof(grid));
				run_frame(t_ns, idx++, -1, false);
			}
		} else if (type == REC_FRAME_RAW && rlen == sizeof(grid) && raw_mode) {
			const s16 *raw = (const s16 *)(data + off);
			int mism = 0, k;

			fill_idle(&last_t, grid_t, true);
			/* hx_prepare_frame_baseline() subtracts 0x7ffe; add it back */
			for (k = 0; k < HX_ROWS * HX_COLS; k++)
				rawbuf[k] = (u16)(raw[k] + 0x7ffe);
			if (flag_grid) {
				algo.fw_finger = false;
				for (k = 0; k < HX_ROWS * HX_COLS && !algo.fw_finger; k++)
					algo.fw_finger = grid[k] != 0;
			}
			hx_preprocess_frame(&algo, rawbuf);
			for (k = 0; k < HX_ROWS * HX_COLS; k++)
				mism += algo.frame[k / HX_COLS][k % HX_COLS] != grid[k];
			run_frame(grid_t, idx++, mism, false);
		}
		off += rlen;
	}
	free(data);
	fprintf(stderr, "hxsim: hand_lands %u hand_peaks %u hand_cancel_map %u "
		"hand_cancel_land %u tracks_new %u\n", algo.stats.hand_lands,
		algo.stats.hand_peaks, algo.stats.hand_cancel_map,
		algo.stats.hand_cancel_land, algo.stats.tracks_new);
	return 0;
}
