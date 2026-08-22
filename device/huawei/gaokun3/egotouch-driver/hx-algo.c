// SPDX-License-Identifier: GPL-2.0
/*
 * Himax HX83121A touch algorithm implementation.
 *
 * This file contains the pure signal-processing pipeline:
 *   Phase 1: preprocessing  (baseline subtraction, CMF, IIR)
 *   Phase 2: touch solving  (macro-zone BFS, palm rejection, peak detection)
 *   Phase 3: tracking       (Hungarian/greedy distance matching)
 *
 * No SPI, no IRQ, no input_dev — all driver glue lives in himax-spi-core.c.
 */

#ifdef HX_ALGO_HOST_TEST
#include "../tests/host-compat.h"
#else
#include <linux/kernel.h>
#include <linux/limits.h>
#include <linux/math64.h>
#include <linux/string.h>
#endif

#include "hx-algo.h"

#define HX_BASELINE_FRACTION_BITS 8

/*
 * Default tracking constants — exposed through hx_algo fields so they can
 * be overridden at runtime via sysfs without reloading the module.
 */
#define HIMAX_TRACK_MATCH_DIST2   (420 * 420)
#define HIMAX_TRACK_LOST_FRAMES   4
#define HIMAX_NEW_TOUCH_DEBOUNCE  1

/* ======================================================================== */
/* Initialisation                                                            */
/* ======================================================================== */

void hx_algo_init_defaults(struct hx_algo *algo)
{
	/* Values track the current Windows solver defaults.  The baseline is
	 * adaptive per cell; using one immutable 0x7ffe value was a major source
	 * of weak-frame loss after temperature/VCOM/common-mode drift. */
	algo->baseline_enabled              = true;
	algo->baseline_initial              = 0x7fee;
	algo->baseline_noise_deadband       = 90;
	algo->baseline_positive_deadband    = 14;
	algo->baseline_negative_deadband    = 13;
	algo->baseline_peak_threshold       = 305;
	algo->baseline_release_hold_frames  = 60;
	algo->baseline_positive_alpha_shift = 7;
	algo->baseline_negative_alpha_shift = 5;
	algo->baseline_noise_alpha_shift    = 6;
	algo->baseline_positive_max_step    = 20;
	algo->baseline_negative_max_step    = 20;
	algo->baseline_background_alpha_shift = 3;
	algo->baseline_no_finger_alpha_shift = 3;
	algo->baseline_recovery_alpha_shift = 2;
	algo->baseline_background_max_step = 512;
	algo->baseline_no_finger_max_step = 512;
	algo->baseline_recovery_max_step = 256;
	algo->baseline_recovery_max_frames = 30;
	algo->baseline_noise_tracking = true;
	algo->cmf_enabled        = true;
	algo->cmf_exclusion      = 2000;
	algo->cmf_max_correction = 2000;
	/* v1.1.2 removed GridIIR from the active pipeline.  Keep the compatible
	 * sysfs implementation available, but do not enable it by default.
	 */
	algo->iir_enabled        = false;
	algo->iir_decay_weight   = 200;
	algo->iir_decay_step     = 80;
	algo->iir_noise_floor    = 5;
	algo->iir_gate_floor     = 200;
	algo->iir_gate_ratio_q8  = 26;
	algo->macro_threshold    = 280;
	algo->peak_threshold     = 280;
	algo->peak_local_radius = 1;
	algo->peak_z8_enabled = true;
	algo->peak_saddle_enabled = true;
	algo->peak_saddle_radius = 2;
	algo->peak_saddle_drop = 80;
	algo->peak_signal_threshold_limit = 1000;
	algo->peak_edge_threshold = 300;
	algo->peak_macro_min_area = 3;
	algo->palm_enabled       = true;
	algo->palm_area_threshold    = 50;
	algo->palm_signal_threshold  = 80000;
	algo->palm_density_low       = 400;
	algo->palm_box_enabled = true;
	algo->palm_box_expand_rows = 9;
	algo->palm_box_expand_cols = 10;
	algo->palm_box_match_distance = 6;
	algo->palm_box_max_hold = 0;
	algo->zone_cleanup_enabled = true;
	algo->zone_max_radius = 3;
	algo->zone_threshold_numer = 0x40;
	algo->zone_threshold_shift = 7;
	algo->pressure_enabled   = false;
	algo->edge_comp_enabled = true;
	algo->edge_boost_pct   = 50;   /* 50% signal boost on border pixels  */
	algo->edge_push_q8     = 128;  /* push up to 0.5 grid cells outward  */
	algo->edge_blend_q8    = 512;  /* blend over 2 grid cells from edge  */
	algo->edge_reject_enabled = true;
	algo->edge_reject_margin = 24;
	algo->edge_reject_min_signal = 500;
	algo->track_dist2_max   = HIMAX_TRACK_MATCH_DIST2;
	algo->track_lost_frames = HIMAX_TRACK_LOST_FRAMES;
	algo->debounce_base     = HIMAX_NEW_TOUCH_DEBOUNCE;
	algo->track_smoothing   = true;
	algo->track_active_guard   = true;
	algo->track_start_debounce = 1;
	algo->track_jump_dist2     = 0;  /* disabled by default */
	algo->hungarian_enabled = true;
	algo->debounce_weak_extra = 1;
	algo->debounce_edge_extra = 1;
	algo->debounce_strong_signal = 3000;
	algo->ghost_enabled = true;
	algo->ghost_row_distance = 32;
	algo->ghost_weak_ratio_q8 = 96;
	algo->ghost_min_col_distance = 300;
	algo->euro_enabled = true;
	algo->euro_alpha_min_q8 = 64;
	algo->euro_alpha_max_q8 = 224;
	algo->euro_speed_threshold = 24;
	algo->gesture_drag_distance = 80;
	algo->gesture_long_press_frames = 45;
}

void hx_algo_reset_runtime(struct hx_algo *algo)
{
	memset(algo->frame, 0, sizeof(algo->frame));
	memset(algo->iir_history, 0, sizeof(algo->iir_history));
	memset(algo->visited, 0, sizeof(algo->visited));
	memset(algo->zone_map, 0, sizeof(algo->zone_map));
	memset(algo->bfs_queue, 0, sizeof(algo->bfs_queue));
	memset(algo->zone_arena, 0, sizeof(algo->zone_arena));
	memset(algo->zones, 0, sizeof(algo->zones));
	memset(algo->peaks, 0, sizeof(algo->peaks));
	memset(algo->contacts, 0, sizeof(algo->contacts));
	memset(algo->baseline_q8, 0, sizeof(algo->baseline_q8));
	memset(algo->baseline_release_hold, 0,
	       sizeof(algo->baseline_release_hold));
	memset(algo->baseline_hist, 0, sizeof(algo->baseline_hist));
	memset(algo->palm_boxes, 0, sizeof(algo->palm_boxes));
	memset(algo->tracks, 0, sizeof(algo->tracks));
	memset(algo->assign_cost, 0, sizeof(algo->assign_cost));
	memset(algo->assign_u, 0, sizeof(algo->assign_u));
	memset(algo->assign_v, 0, sizeof(algo->assign_v));
	memset(algo->assign_p, 0, sizeof(algo->assign_p));
	memset(algo->assign_way, 0, sizeof(algo->assign_way));
	algo->iir_initialized = false;
	algo->baseline_initialized = false;
	algo->baseline_prev_had_signal = false;
	algo->baseline_had_freeze = false;
	algo->baseline_recovery_frames = 0;
	algo->diag_frame_seq = 0;
	algo->diag_common_diff = 0;
	algo->diag_frame_max = 0;
	algo->diag_has_signal = 0;
	algo->diag_zones = 0;
	algo->diag_peaks = 0;
	algo->diag_contacts_pre_filter = 0;
	algo->diag_contacts_post_filter = 0;
	algo->diag_active_tracks = 0;
	algo->diag_reported_tracks = 0;
	algo->zone_arena_used = 0;
	algo->zone_count = 0;
	algo->peak_count = 0;
	algo->contact_count = 0;
	algo->palm_box_count = 0;
	algo->touch_active = false;
	algo->touch_start_frames = 0;
	algo->gesture_state = HX_GESTURE_IDLE;
	algo->gesture_frames = 0;
	algo->gesture_start_x = 0;
	algo->gesture_start_y = 0;
}

/* ======================================================================== */
/* Phase 1A — baseline subtraction                                          */
/* ======================================================================== */

static s32 hx_baseline_step_q8(s32 delta, u8 shift, s16 max_step)
{
	s32 limit_q8 = max_t(s32, max_step, 0) *
			(1 << HX_BASELINE_FRACTION_BITS);
	s32 step_q8;

	if (!delta)
		return 0;
	step_q8 = delta * (1 << HX_BASELINE_FRACTION_BITS);
	step_q8 >>= min_t(u8, shift, 30);
	return clamp_t(s32, step_q8, -limit_q8, limit_q8);
}

static void hx_prepare_frame_baseline(struct hx_algo *algo, const u16 *raw,
				      enum hx_finger_state finger_state)
{
	s64 common_sum = 0;
	s32 common_diff;
	int common_bin = 0;
	int common_count = 0;
	int cumulative = 0;
	bool has_signal = false;
	bool recovery;
	bool found_freeze = false;
	int r, c;

	if (!algo->baseline_initialized) {
		for (r = 0; r < HX_PIXELS; r++)
			algo->baseline_q8[r] = (s32)algo->baseline_initial <<
						HX_BASELINE_FRACTION_BITS;
		memset(algo->baseline_release_hold, 0,
		       sizeof(algo->baseline_release_hold));
		algo->baseline_initialized = true;
	}
	memset(algo->baseline_hist, 0, sizeof(algo->baseline_hist));

	/* Estimate panel-wide VCOM/temperature drift before classifying cells. */
	for (r = 0; r < HX_ROWS; r++) {
		for (c = 0; c < HX_COLS; c++) {
			int idx = r * HX_COLS + c;
			s32 sample = (s32)le16_to_cpup(raw + idx);
			s32 baseline = algo->baseline_q8[idx] >>
					HX_BASELINE_FRACTION_BITS;

			if (idx != 0) {
				s32 delta = sample - baseline;
				int bin = (delta + 65536) >> 6;

				algo->baseline_hist[clamp_t(int, bin, 0,
					HX_BASELINE_HIST_BINS - 1)]++;
			}
		}
	}
	for (common_bin = 0; common_bin < HX_BASELINE_HIST_BINS;
	     common_bin++) {
		cumulative += algo->baseline_hist[common_bin];
		if (cumulative >= HX_PIXELS / 2)
			break;
	}
	common_bin = min(common_bin, HX_BASELINE_HIST_BINS - 1);
	for (r = 0; r < HX_ROWS; r++) {
		for (c = 0; c < HX_COLS; c++) {
			int idx = r * HX_COLS + c;
			s32 sample, baseline, delta;
			int bin;

			if (!idx)
				continue;
			sample = (s32)le16_to_cpup(raw + idx);
			baseline = algo->baseline_q8[idx] >>
				   HX_BASELINE_FRACTION_BITS;
			delta = sample - baseline;
			bin = clamp_t(int, (delta + 65536) >> 6, 0,
				      HX_BASELINE_HIST_BINS - 1);
			if (bin == common_bin) {
				common_sum += delta;
				common_count++;
			}
		}
	}
	common_diff = common_count ? (s32)(common_sum / common_count) : 0;
	algo->diag_common_diff = common_diff;
	if (finger_state == HX_FINGER_PRESENT) {
		has_signal = true;
	} else if (finger_state == HX_FINGER_UNKNOWN) {
		/* Host tests and callers without a validated suffix retain the
		 * conservative signal/track fallback.
		 */
		for (r = 0; r < HX_ROWS && !has_signal; r++) {
			for (c = 0; c < HX_COLS; c++) {
				int idx = r * HX_COLS + c;
				s32 sample = (s32)le16_to_cpup(raw + idx);
				s32 baseline = algo->baseline_q8[idx] >>
						HX_BASELINE_FRACTION_BITS;

				if (sample - baseline - common_diff >=
				    algo->baseline_peak_threshold) {
					has_signal = true;
					break;
				}
			}
		}
		for (r = 0; r < HIMAX_MAX_TOUCH; r++)
			if (algo->tracks[r].active) {
				has_signal = true;
				break;
			}
	}

	recovery = has_signal && (!algo->baseline_prev_had_signal ||
				    !algo->baseline_had_freeze) &&
		   algo->baseline_recovery_frames <
				algo->baseline_recovery_max_frames;
	if (recovery)
		algo->baseline_recovery_frames++;
	else if (!has_signal || algo->baseline_had_freeze)
		algo->baseline_recovery_frames = 0;
	for (r = 0; r < HX_ROWS; r++) {
		for (c = 0; c < HX_COLS; c++) {
			int idx = r * HX_COLS + c;
			s32 sample = (s32)le16_to_cpup(raw + idx);
			s32 baseline = algo->baseline_q8[idx] >>
					HX_BASELINE_FRACTION_BITS;
			s32 delta = sample - baseline;
			s32 local = delta - common_diff;
			s32 step_q8 = 0;

			if (!algo->baseline_enabled) {
				delta = sample - algo->baseline_initial;
				goto store;
			}

			/* Freeze cells carrying a finger-sized positive signal.  A hold
			 * after release prevents the negative rebound from being absorbed
			 * into the baseline and creating a later false lift. */
			if (has_signal && local >= algo->baseline_peak_threshold) {
				found_freeze = true;
				algo->baseline_release_hold[idx] =
					algo->baseline_release_hold_frames;
				/* Frozen cells still follow global VCOM drift. */
				step_q8 = hx_baseline_step_q8(common_diff,
					algo->baseline_background_alpha_shift,
					algo->baseline_background_max_step);
				algo->baseline_q8[idx] = clamp_t(s32,
					algo->baseline_q8[idx] + step_q8, 0,
					0xffff << HX_BASELINE_FRACTION_BITS);
				delta = local;
				goto store;
			}
			if (algo->baseline_release_hold[idx]) {
				algo->baseline_release_hold[idx]--;
				if (local < -algo->baseline_negative_deadband) {
					delta = local;
					goto store;
				}
				delta = 0;
				goto store;
			}

			if (!has_signal) {
				if (abs(delta) <= algo->baseline_noise_deadband &&
				    algo->baseline_noise_tracking)
					step_q8 = hx_baseline_step_q8(delta,
						algo->baseline_noise_alpha_shift, 1);
				else
					step_q8 = hx_baseline_step_q8(delta,
						algo->baseline_no_finger_alpha_shift,
						algo->baseline_no_finger_max_step);
			} else if (recovery) {
				step_q8 = hx_baseline_step_q8(delta,
					algo->baseline_recovery_alpha_shift,
					algo->baseline_recovery_max_step);
			} else if (abs(delta) <= algo->baseline_noise_deadband &&
				   algo->baseline_noise_tracking)
				step_q8 = hx_baseline_step_q8(delta,
					algo->baseline_noise_alpha_shift, 1);
			else if (delta > algo->baseline_positive_deadband)
				step_q8 = hx_baseline_step_q8(delta,
					algo->baseline_positive_alpha_shift,
					algo->baseline_positive_max_step);
			else if (delta < -algo->baseline_negative_deadband)
				step_q8 = hx_baseline_step_q8(delta,
					algo->baseline_negative_alpha_shift,
					algo->baseline_negative_max_step);

			algo->baseline_q8[idx] = clamp_t(s32,
				algo->baseline_q8[idx] + step_q8, 0,
				0xffff << HX_BASELINE_FRACTION_BITS);

			/* Match v1.1.2 ProcessNoFinger/ProcessFinger background
			 * semantics: only frozen candidate cells reach the solver.
			 * Passing every local residual fills the fixed zone arena with
			 * background islands and can evict a real finger by scan order.
			 */
			delta = 0;
	store:
			algo->frame[r][c] = clamp_t(s32, delta, SHRT_MIN, SHRT_MAX);
		}
	}
	algo->baseline_prev_had_signal = has_signal;
	algo->diag_has_signal = has_signal;
	algo->baseline_had_freeze = found_freeze;

	/* pixel [0][0] is always invalid on this panel layout */
	algo->frame[0][0] = 0;
}

/* ======================================================================== */
/* Phase 1A½ — edge signal boost                                            */
/*                                                                           */
/* Compensate reduced capacitive sensitivity at sensor borders by scaling   */
/* border pixels upward.  Row 0/last and col 0/last get the full boost;    */
/* row 1/last-1 and col 1/last-1 get half.  Corner pixels (on two borders) */
/* are boosted once from each axis (multiplicative).                         */
/* ======================================================================== */

static void hx_edge_boost(struct hx_algo *algo)
{
	int r, c;
	s32 pct = algo->edge_boost_pct;
	s32 half_pct = pct / 2;

	if (!algo->edge_comp_enabled || pct <= 0)
		return;

	/* Boost border rows: row 0 and row HX_ROWS-1 (full), row 1 and HX_ROWS-2 (half) */
	for (c = 0; c < HX_COLS; c++) {
		s32 v;

		/* Top edge */
		v = algo->frame[0][c];
		if (v > 0)
			algo->frame[0][c] = clamp_t(s32, v + v * pct / 100, 0, SHRT_MAX);
		v = algo->frame[1][c];
		if (v > 0)
			algo->frame[1][c] = clamp_t(s32, v + v * half_pct / 100, 0, SHRT_MAX);

		/* Bottom edge */
		v = algo->frame[HX_ROWS - 1][c];
		if (v > 0)
			algo->frame[HX_ROWS - 1][c] = clamp_t(s32, v + v * pct / 100, 0, SHRT_MAX);
		v = algo->frame[HX_ROWS - 2][c];
		if (v > 0)
			algo->frame[HX_ROWS - 2][c] = clamp_t(s32, v + v * half_pct / 100, 0, SHRT_MAX);
	}

	/* Boost border columns: col 0 and col HX_COLS-1 (full), col 1 and HX_COLS-2 (half) */
	for (r = 0; r < HX_ROWS; r++) {
		s32 v;

		/* Left edge */
		v = algo->frame[r][0];
		if (v > 0)
			algo->frame[r][0] = clamp_t(s32, v + v * pct / 100, 0, SHRT_MAX);
		v = algo->frame[r][1];
		if (v > 0)
			algo->frame[r][1] = clamp_t(s32, v + v * half_pct / 100, 0, SHRT_MAX);

		/* Right edge */
		v = algo->frame[r][HX_COLS - 1];
		if (v > 0)
			algo->frame[r][HX_COLS - 1] = clamp_t(s32, v + v * pct / 100, 0, SHRT_MAX);
		v = algo->frame[r][HX_COLS - 2];
		if (v > 0)
			algo->frame[r][HX_COLS - 2] = clamp_t(s32, v + v * half_pct / 100, 0, SHRT_MAX);
	}
}

/* ======================================================================== */
/* Phase 1B — CMF (Common Mode Filter)                                      */
/*                                                                           */
/* Removes charger-induced common-mode noise by subtracting per-row and     */
/* per-column offsets computed from "quiet" pixels (|val| < exclusion).     */
/* DualDim mode: rows first, then columns.                                   */
/* ======================================================================== */

static void hx_apply_cmf(struct hx_algo *algo)
{
	int r, c;

	/* Row pass */
	for (r = 0; r < HX_ROWS; r++) {
		s32 sum = 0, count = 0, offset;

		for (c = 0; c < HX_COLS; c++) {
			s16 v = algo->frame[r][c];

			if (abs((int)v) < algo->cmf_exclusion) {
				sum += v;
				count++;
			}
		}
		if (!count)
			continue;

		offset = clamp_t(s32, sum / count,
				 -algo->cmf_max_correction,
				  algo->cmf_max_correction);
		for (c = 0; c < HX_COLS; c++) {
			s32 corrected = (s32)algo->frame[r][c] - offset;

			algo->frame[r][c] = clamp_t(s32, corrected, SHRT_MIN, SHRT_MAX);
		}
	}

	/* Column pass */
	for (c = 0; c < HX_COLS; c++) {
		s32 sum = 0, count = 0, offset;

		for (r = 0; r < HX_ROWS; r++) {
			s16 v = algo->frame[r][c];

			if (abs((int)v) < algo->cmf_exclusion) {
				sum += v;
				count++;
			}
		}
		if (!count)
			continue;

		offset = clamp_t(s32, sum / count,
				 -algo->cmf_max_correction,
				  algo->cmf_max_correction);
		for (r = 0; r < HX_ROWS; r++) {
			s32 corrected = (s32)algo->frame[r][c] - offset;

			algo->frame[r][c] = clamp_t(s32, corrected, SHRT_MIN, SHRT_MAX);
		}
	}
}

/* ======================================================================== */
/* Phase 1C — GridIIR temporal filter                                       */
/*                                                                           */
/* Per-pixel exponential decay for noise suppression.  Pixels above a       */
/* dynamic threshold (proportional to the frame maximum) bypass the filter  */
/* so real touch signals are never attenuated.                               */
/* ======================================================================== */

static void hx_apply_iir(struct hx_algo *algo)
{
	int r, c;
	s32 frame_max = 0;
	s32 dyn_threshold;
	u16 decay_weight, decay_step;

	if (!algo->iir_enabled) {
		memcpy(algo->iir_history, algo->frame, sizeof(algo->frame));
		algo->iir_initialized = true;
		return;
	}

	if (!algo->iir_initialized) {
		memcpy(algo->iir_history, algo->frame, sizeof(algo->frame));
		algo->iir_initialized = true;
		return;
	}

	for (r = 0; r < HX_ROWS; r++)
		for (c = 0; c < HX_COLS; c++)
			frame_max = max(frame_max, abs((int)algo->frame[r][c]));

	dyn_threshold = max((frame_max * algo->iir_gate_ratio_q8) >> 8,
			    (s32)algo->iir_gate_floor);
	decay_weight  = min_t(u16, algo->iir_decay_weight, 256);
	decay_step    = algo->iir_decay_step;

	for (r = 0; r < HX_ROWS; r++) {
		for (c = 0; c < HX_COLS; c++) {
			s32 cur = algo->frame[r][c];
			s32 output;

			if (cur >= dyn_threshold) {
				output = cur;
			} else {
				s32 hist  = algo->iir_history[r][c];
				s32 mixed = decay_weight * cur +
					    (256 - decay_weight) * hist;

				output = mixed >> 8;
				output = max(0, output - (s32)decay_step);
				if (output < algo->iir_noise_floor)
					output = 0;
			}

			algo->frame[r][c]       = clamp_t(s32, output, SHRT_MIN, SHRT_MAX);
			algo->iir_history[r][c] = algo->frame[r][c];
		}
	}
}

/* ======================================================================== */
/* Phase 1 entry point                                                       */
/* ======================================================================== */

void hx_preprocess_frame(struct hx_algo *algo, const u16 *raw)
{
	hx_preprocess_frame_state(algo, raw, HX_FINGER_UNKNOWN);
}

void hx_preprocess_frame_state(struct hx_algo *algo, const u16 *raw,
			       enum hx_finger_state finger_state)
{
	hx_prepare_frame_baseline(algo, raw, finger_state);

	if (algo->cmf_enabled)
		hx_apply_cmf(algo);

	hx_edge_boost(algo);

	hx_apply_iir(algo);
}

/*
 * Clamp-to-zero accessor: returns 0 for out-of-bounds or negative values so
 * neighbour lookups near the grid edge never need special-casing.
 */
static inline s16 hx_frame_at(const struct hx_algo *algo, int r, int c)
{
	s16 val;

	if (r < 0 || r >= HX_ROWS || c < 0 || c >= HX_COLS)
		return 0;
	val = algo->frame[r][c];
	return val > 0 ? val : 0;
}

/* ======================================================================== */
/* Phase 2A — macro-zone detection (8-connected BFS)                        */
/* ======================================================================== */

void hx_detect_macro_zones(struct hx_algo *algo)
{
	static const int dr[] = {-1, -1, -1, 0, 0, 1, 1, 1};
	static const int dc[] = {-1,  0,  1, -1, 1, -1, 0, 1};
	u16 head, tail;
	int r, c, d;

	memset(algo->visited, 0, sizeof(algo->visited));
	algo->zone_count = 0;
	algo->zone_arena_used = 0;

	for (r = 0; r < HX_ROWS; r++) {
		for (c = 0; c < HX_COLS; c++) {
			int idx = r * HX_COLS + c;
			struct hx_macro_zone candidate;
			struct hx_macro_zone *zone = &candidate;

			if (algo->visited[idx])
				continue;
			if (algo->frame[r][c] < algo->macro_threshold)
				continue;
			zone->arena_start = algo->zone_arena_used;
			zone->area       = 0;
			zone->signal_sum = 0;
			zone->min_r = r;  zone->max_r = r;
			zone->min_c = c;  zone->max_c = c;

			/* Ring-buffer BFS using the pre-allocated queue. */
			head = 0;
			tail = 0;
			algo->bfs_queue[tail++] = idx;
			algo->visited[idx] = 1;

			while (head != tail) {
				int ci = algo->bfs_queue[head++];
				int cr = ci / HX_COLS;
				int cc = ci % HX_COLS;
				s16 sig = algo->frame[cr][cc];

				if (algo->zone_arena_used < HX_PIXELS)
					algo->zone_arena[algo->zone_arena_used++] = ci;
				zone->area++;
				if (sig > 0)
					zone->signal_sum += sig;

				if (cr < zone->min_r) zone->min_r = cr;
				if (cr > zone->max_r) zone->max_r = cr;
				if (cc < zone->min_c) zone->min_c = cc;
				if (cc > zone->max_c) zone->max_c = cc;

				for (d = 0; d < 8; d++) {
					int nr = cr + dr[d];
					int nc = cc + dc[d];
					int ni;

					if (nr < 0 || nr >= HX_ROWS ||
					    nc < 0 || nc >= HX_COLS)
						continue;
					ni = nr * HX_COLS + nc;
					if (algo->visited[ni])
						continue;
					if (algo->frame[nr][nc] < algo->macro_threshold)
						continue;
					algo->visited[ni] = 1;
					algo->bfs_queue[tail++] = ni;
				}
			}

			if (algo->zone_count < HX_MAX_ZONES) {
				algo->zones[algo->zone_count++] = candidate;
			} else {
				u8 weakest = 0;
				u8 zi;

				for (zi = 1; zi < HX_MAX_ZONES; zi++)
					if (algo->zones[zi].signal_sum <
					    algo->zones[weakest].signal_sum)
						weakest = zi;
				if (candidate.signal_sum >
				    algo->zones[weakest].signal_sum)
					algo->zones[weakest] = candidate;
			}
		}
	}

	/* Windows exposes macro zones strongest-first.  Besides matching its
	 * behavior, deterministic ordering prevents later fixed-capacity stages
	 * from depending on row-major scan position.
	 */
	for (r = 0; r + 1 < algo->zone_count; r++) {
		int strongest = r;

		for (c = r + 1; c < algo->zone_count; c++)
			if (algo->zones[c].signal_sum >
			    algo->zones[strongest].signal_sum)
				strongest = c;
		if (strongest != r)
			swap(algo->zones[r], algo->zones[strongest]);
	}
}

/* ======================================================================== */
/* Phase 2B — palm rejection                                                 */
/*                                                                           */
/* Combine footprint, integrated signal, density and bounding-box evidence. */
/* A compact, sharp peak is preserved even when its total signal is high.   */
/* ======================================================================== */

static bool hx_box_has_domain(const struct hx_algo *algo,
			      const struct hx_palm_box *box)
{
	int r, c;

	for (r = box->min_r; r <= box->max_r; r++)
		for (c = box->min_c; c <= box->max_c; c++)
			if (hx_frame_at(algo, r, c) >= algo->macro_threshold)
				return true;
	return false;
}

static void hx_age_palm_boxes(struct hx_algo *algo)
{
	u8 i, dst = 0;

	for (i = 0; i < algo->palm_box_count; i++) {
		struct hx_palm_box box = algo->palm_boxes[i];

		if (hx_box_has_domain(algo, &box))
			box.missed = 0;
		else
			box.missed++;
		if (box.missed && (!algo->palm_box_max_hold ||
				   box.missed > algo->palm_box_max_hold))
			continue;
		algo->palm_boxes[dst++] = box;
	}
	algo->palm_box_count = dst;
}

static void hx_update_palm_box(struct hx_algo *algo,
			       const struct hx_macro_zone *z)
{
	struct hx_palm_box candidate;
	int center_r, center_c;
	u8 i;

	if (!algo->palm_box_enabled)
		return;
	candidate.active = true;
	candidate.min_r = max_t(int, 0, z->min_r - algo->palm_box_expand_rows);
	candidate.max_r = min_t(int, HX_ROWS - 1,
				z->max_r + algo->palm_box_expand_rows);
	candidate.min_c = max_t(int, 0, z->min_c - algo->palm_box_expand_cols);
	candidate.max_c = min_t(int, HX_COLS - 1,
				z->max_c + algo->palm_box_expand_cols);
	candidate.missed = 0;
	center_r = (candidate.min_r + candidate.max_r) / 2;
	center_c = (candidate.min_c + candidate.max_c) / 2;

	for (i = 0; i < algo->palm_box_count; i++) {
		struct hx_palm_box *old = &algo->palm_boxes[i];
		int old_r = (old->min_r + old->max_r) / 2;
		int old_c = (old->min_c + old->max_c) / 2;

		if (abs(center_r - old_r) <= algo->palm_box_match_distance &&
		    abs(center_c - old_c) <= algo->palm_box_match_distance) {
			*old = candidate;
			return;
		}
	}
	if (algo->palm_box_count < HX_MAX_PALM_BOXES)
		algo->palm_boxes[algo->palm_box_count++] = candidate;
}

void hx_reject_palms(struct hx_algo *algo)
{
	u8 dst = 0;
	u8 i;

	hx_age_palm_boxes(algo);
	if (!algo->palm_enabled)
		return;

	for (i = 0; i < algo->zone_count; i++) {
		struct hx_macro_zone *z = &algo->zones[i];
		u16 bbox_w, bbox_h, max_side, min_side;
		u16 pi;
		s32 max_signal = 0;
		s32 mean_signal;
		int palm_score = 0;
		bool strong_finger_shape;
		bool reject = false;

		/* The Windows solver treats these as evidence, not four independent
		 * kill switches.  The old Linux port dropped an entire normal finger
		 * as soon as its integrated signal crossed 80000. */
		if (z->area >= algo->palm_area_threshold)
			palm_score += 35;
		if (z->signal_sum >= algo->palm_signal_threshold)
			palm_score += 25;
		if (z->area >= 20 &&
		    z->signal_sum < (s32)algo->palm_density_low * z->area)
			palm_score += 15;

		bbox_w   = z->max_c - z->min_c + 1;
		bbox_h   = z->max_r - z->min_r + 1;
		max_side = max(bbox_w, bbox_h);
		min_side = min(bbox_w, bbox_h);
		if (z->area >= 10 && min_side > 0 &&
		    (u32)max_side * 256 >= 1024u * min_side)
			palm_score += 15;
		if (z->area >= 35 &&
		    (u32)z->area * 100 >= (u32)bbox_w * bbox_h * 40)
			palm_score += 15;

		for (pi = 0; pi < z->area; pi++) {
			int idx = algo->zone_arena[z->arena_start + pi];

			max_signal = max_t(s32, max_signal,
					   hx_frame_at(algo, idx / HX_COLS,
						       idx % HX_COLS));
		}
		mean_signal = z->area ? z->signal_sum / z->area : 0;
		strong_finger_shape = max_signal >= mean_signal + 100 &&
				      max_signal * 100 >= mean_signal * 335;
		if (strong_finger_shape)
			palm_score -= 20;

		/* Mirror PalmLikely: a large region needs several independent palm
		 * signals before it may suppress contacts. */
		reject = z->area >= 55 && palm_score >= 55;
		if (reject)
			hx_update_palm_box(algo, z);

		if (!reject) {
			if (dst != i)
				algo->zones[dst] = *z;
			dst++;
		}
	}

	algo->zone_count = dst;
}

/* ======================================================================== */
/* Phase 2C — peak detection within surviving zones                         */
/* ======================================================================== */

/*
 * Asymmetric local-maximum test.
 *
 * "Before" neighbours (up + left in scan order) must be strictly less;
 * "after" neighbours (down + right) may be equal.  This breaks ties on
 * flat ridges so exactly one peak is produced per finger plateau.
 */
static bool hx_is_asymmetric_peak(const struct hx_algo *algo, int r, int c)
{
	s16 v = algo->frame[r][c];
	int radius = max_t(int, 1, algo->peak_local_radius);
	int dr, dc;

	for (dr = -radius; dr <= radius; dr++) {
		for (dc = -radius; dc <= radius; dc++) {
			int nr, nc;
			s16 nv;
			bool after;

			if (dr == 0 && dc == 0)
				continue;
			nr = r + dr;
			nc = c + dc;
			if (nr < 0 || nr >= HX_ROWS || nc < 0 || nc >= HX_COLS)
				continue;
			nv    = algo->frame[nr][nc];
			after = (dr > 0) || (dr == 0 && dc > 0);
			if (after) {
				if (nv > v) return false;
			} else {
				if (nv >= v) return false;
			}
		}
	}
	return true;
}

/*
 * Pressure-drift detector.
 *
 * A flat palm press produces a nearly-uniform row of elevated pixels with
 * low cross-row gradient.  Returns true when the peak signal falls in the
	 * drift range [3/8, 3/4] of the independent signal limit and the row gradient is low
 * while the row signal sum is high relative to the peak.
 */
static bool hx_detect_pressure_drift(const struct hx_algo *algo, int r, int c)
{
	s16 peak_sig  = algo->frame[r][c];
	s16 limit3_4  = (algo->peak_signal_threshold_limit * 3) >> 2;
	s16 limit3_8  = (algo->peak_signal_threshold_limit * 3) >> 3;
	int grad_sum  = 0;
	int row_sum   = 0;
	int col;

	if (peak_sig > limit3_4 || peak_sig < limit3_8)
		return false;

	for (col = 1; col < HX_COLS - 1; col++) {
		int grad = abs((int)hx_frame_at(algo, r, col + 1) -
			       (int)hx_frame_at(algo, r, col - 1));

		if (grad > algo->peak_signal_threshold_limit / 3)
			return false;   /* sharp spike → not drift */
		grad_sum += grad;
		if (algo->frame[r][col] > 0)
			row_sum += algo->frame[r][col];
	}

	return (row_sum >= peak_sig * 9 / 2) &&
	       (peak_sig * 6 >= grad_sum);
}

/*
 * Insert a peak into the fixed-size peak array.  When the array is full,
 * replace the weakest existing entry if the new peak is stronger.
 */
static void hx_insert_peak(struct hx_algo *algo, const struct hx_peak *p)
{
	int k, weakest;

	if (algo->peak_count < HX_MAX_PEAKS) {
		algo->peaks[algo->peak_count++] = *p;
		return;
	}

	weakest = 0;
	for (k = 1; k < HX_MAX_PEAKS; k++) {
		if (algo->peaks[k].z < algo->peaks[weakest].z)
			weakest = k;
	}
	if (p->z > algo->peaks[weakest].z)
		algo->peaks[weakest] = *p;
}

void hx_detect_peaks(struct hx_algo *algo)
{
	u8 zi;

	algo->peak_count = 0;

	/* --- Asymmetric local-max scan within each surviving zone --- */
	for (zi = 0; zi < algo->zone_count; zi++) {
		struct hx_macro_zone *zone = &algo->zones[zi];
		u16 pi;

		for (pi = 0; pi < zone->area; pi++) {
			int idx = algo->zone_arena[zone->arena_start + pi];
			int r = idx / HX_COLS;
			int c = idx % HX_COLS;
			s16 v = algo->frame[r][c];
			struct hx_peak peak;
			bool on_edge = (r == 0 || r == HX_ROWS - 1 ||
					c == 0 || c == HX_COLS - 1);
			bool edge_threshold_cell = (c == 1 || c == HX_COLS - 2 ||
						    r == HX_ROWS - 1);
			int dr, dc;
			s32 nbr_sum = 0;

			if (v < (edge_threshold_cell ? algo->peak_edge_threshold :
						algo->peak_threshold))
				continue;
			if (!hx_is_asymmetric_peak(algo, r, c))
				continue;
			if (hx_detect_pressure_drift(algo, r, c))
				continue;

			for (dr = -1; dr <= 1; dr++)
				for (dc = -1; dc <= 1; dc++) {
					if (dr == 0 && dc == 0)
						continue;
					nbr_sum += hx_frame_at(algo, r + dr, c + dc);
				}

			peak = (struct hx_peak){
				.r         = r,
				.c         = c,
				.z         = v,
				.nbr_sum   = nbr_sum,
				.zone_area = zone->area,
				.zone_index = zi,
				.on_edge = on_edge,
			};
			hx_insert_peak(algo, &peak);
		}
	}

	/* Close maxima without a real saddle are one broad peak, not two
	 * fingers.  Integer midpoint sampling mirrors the Windows saddle gate.
	 */
	if (algo->peak_saddle_enabled) {
		u8 i, j;

		for (i = 0; i < algo->peak_count; i++) {
			for (j = i + 1; j < algo->peak_count; j++) {
				struct hx_peak *a = &algo->peaks[i];
				struct hx_peak *b = &algo->peaks[j];
				int dr = (int)b->r - a->r;
				int dc = (int)b->c - a->c;
				int steps = max(abs(dr), abs(dc));
				int saddle = 0;
				int weaker;
				int required_drop;
				int s;

				if (a->zone_index != b->zone_index)
					continue;
				if (steps == 0 || steps > algo->peak_saddle_radius)
					continue;
				for (s = 1; s < steps; s++) {
					int rr_num = dr * s;
					int cc_num = dc * s;
					int rr = a->r + (rr_num >= 0 ?
						(rr_num + steps / 2) / steps :
						(rr_num - steps / 2) / steps);
					int cc = a->c + (cc_num >= 0 ?
						(cc_num + steps / 2) / steps :
						(cc_num - steps / 2) / steps);

					saddle = max_t(int, saddle,
						       hx_frame_at(algo, rr, cc));
				}
				weaker = min(a->z, b->z);
				required_drop = max((int)algo->peak_saddle_drop,
						    weaker * 8 / 100);
				if (weaker - saddle < required_drop) {
					struct hx_peak *weak = a->z <= b->z ? a : b;

					weak->z = -1;
				}
			}
		}
		{
			u8 dst = 0;

			for (i = 0; i < algo->peak_count; i++)
				if (algo->peaks[i].z >= 0)
					algo->peaks[dst++] = algo->peaks[i];
			algo->peak_count = dst;
		}
	}

	/* --- Z8 isolation filter: (z >> 5) > nbr_sum → isolated spike --- */
	{
		u8 dst = 0, i;

		for (i = 0; i < algo->peak_count; i++) {
			if (!algo->peak_z8_enabled ||
			    (algo->peaks[i].z >> 5) <= algo->peaks[i].nbr_sum)
				algo->peaks[dst++] = algo->peaks[i];
		}
		algo->peak_count = dst;
	}

	/* --- Zone minimum-area filter: area < 2 → reject (except edge peaks) --- */
	{
		u8 dst = 0, i;

		for (i = 0; i < algo->peak_count; i++) {
			bool on_edge = (algo->peaks[i].r == 0 ||
					algo->peaks[i].r == HX_ROWS - 1 ||
					algo->peaks[i].c == 0 ||
					algo->peaks[i].c == HX_COLS - 1);

			if (algo->peaks[i].zone_area >= algo->peak_macro_min_area ||
			    on_edge)
				algo->peaks[dst++] = algo->peaks[i];
		}
		algo->peak_count = dst;
	}

	/* Persistent PalmBox suppression catches strong finger-like peaks that
	 * appear inside a palm domain on later frames.
	 */
	if (algo->palm_box_enabled && algo->palm_box_count) {
		u8 i, bi, dst = 0;

		for (i = 0; i < algo->peak_count; i++) {
			bool inside = false;

			for (bi = 0; bi < algo->palm_box_count; bi++) {
				struct hx_palm_box *b = &algo->palm_boxes[bi];

				if (algo->peaks[i].r >= b->min_r &&
				    algo->peaks[i].r <= b->max_r &&
				    algo->peaks[i].c >= b->min_c &&
				    algo->peaks[i].c <= b->max_c) {
					inside = true;
					break;
				}
			}
			if (!inside)
				algo->peaks[dst++] = algo->peaks[i];
		}
		algo->peak_count = dst;
	}

	/* --- Edge peak filter: weak edge peaks < max_sig * 5/8 --- */
	{
		int edge;

		for (edge = 0; edge < 2; edge++) {
			s16 max_sig = 0, cutoff;
			u8 dst = 0, i;

			for (i = 0; i < algo->peak_count; i++) {
				bool on_edge;

				on_edge = algo->peaks[i].c ==
					(edge == 0 ? 0 : HX_COLS - 1);
				if (on_edge && algo->peaks[i].z > max_sig)
					max_sig = algo->peaks[i].z;
			}
			if (max_sig == 0)
				continue;

			cutoff = (max_sig >> 3) * 5;
			for (i = 0; i < algo->peak_count; i++) {
				bool on_edge;

				on_edge = algo->peaks[i].c ==
					(edge == 0 ? 0 : HX_COLS - 1);
				if (!(on_edge && algo->peaks[i].z < cutoff))
					algo->peaks[dst++] = algo->peaks[i];
			}
			algo->peak_count = dst;
		}
	}

	/* --- Sort ascending by signal (selection sort, ≤20 elements) --- */
	{
		u8 i, j;

		for (i = 0; i + 1 < algo->peak_count; i++) {
			u8 min_idx = i;

			for (j = i + 1; j < algo->peak_count; j++) {
				if (algo->peaks[j].z < algo->peaks[min_idx].z)
					min_idx = j;
			}
			if (min_idx != i)
				swap(algo->peaks[i], algo->peaks[min_idx]);
		}
	}
}

/* ======================================================================== */
/* Phase 2D — zone expansion + weighted centroid                            */
/*                                                                           */
/* For each peak, BFS-expand outward while signal >= 50% of the peak       */
/* value.  Accumulate weighted centroid (Q8.8 fixed-point grid coords)     */
/* using s64 intermediate products.  When the BFS meets pixels already     */
/* owned by another peak, fall back to a 3x3 local centroid.               */
/*                                                                           */
/* Result: contacts[] filled, then converted to output coords [0, 65535].  */
/* ======================================================================== */

/*
 * Compute the zone expansion threshold: ~50% of min(peak_threshold, peak_z).
 * Uses integer multiply + shift: base * 0x40 >> 7 ≈ base * 0.5.
 */
static inline s16 hx_zone_thold(const struct hx_algo *algo, s16 peak_z)
{
	int base = min_t(int, algo->peak_threshold, peak_z);
	int shift = min_t(int, algo->zone_threshold_shift, 15);
	int result = (base * algo->zone_threshold_numer) >> shift;

	return (s16)max(result, 1);
}

static int hx_neighbor_support(const struct hx_algo *algo, int r, int c,
			       s16 threshold)
{
	int dr, dc;
	int support = 0;

	for (dr = -1; dr <= 1; dr++)
		for (dc = -1; dc <= 1; dc++)
			if ((dr || dc) &&
			    hx_frame_at(algo, r + dr, c + dc) >= threshold)
				support++;
	return support;
}

/*
 * Single-peak zone: BFS flood-fill weighted centroid.
 * Returns true if the expansion was clean (no overlap with other zones).
 */
static bool hx_expand_single_peak(struct hx_algo *algo, int pi,
				    struct hx_contact *ct)
{
	struct hx_peak *pk = &algo->peaks[pi];
	s16 thold = hx_zone_thold(algo, pk->z);
	u8 zone_id = (u8)(pi + 1);
	u16 head = 0, tail = 0;
	int seed = pk->r * HX_COLS + pk->c;
	bool clean = true;
	s64 w_col = 0, w_row = 0;
	s32 w_total = 0;
	u16 area = 0;
	s32 sig_sum = 0;

	static const int dr[] = {-1, -1, -1, 0, 0, 1, 1, 1};
	static const int dc[] = {-1,  0,  1, -1, 1, -1, 0, 1};

	algo->zone_map[seed] = zone_id;
	algo->bfs_queue[tail++] = seed;

	while (head != tail) {
		int idx = algo->bfs_queue[head++];
		int r = idx / HX_COLS;
		int c = idx % HX_COLS;
		s16 sig = hx_frame_at(algo, r, c);
		int d;

		area++;
		sig_sum += sig;
		w_col += (s64)c * 128 * sig;
		w_row += (s64)r * 128 * sig;
		w_total += sig;

		for (d = 0; d < 8; d++) {
			int nr = r + dr[d];
			int nc = c + dc[d];
			int ni;

			if (nr < 0 || nr >= HX_ROWS || nc < 0 || nc >= HX_COLS)
				continue;
			ni = nr * HX_COLS + nc;
			if (algo->zone_map[ni]) {
				if (algo->zone_map[ni] != zone_id)
					clean = false;
				continue;
			}
			if (hx_frame_at(algo, nr, nc) < thold)
				continue;
			if (algo->zone_max_radius &&
			    max(abs(nr - (int)pk->r), abs(nc - (int)pk->c)) >
				algo->zone_max_radius)
				continue;
			if (algo->zone_cleanup_enabled &&
			    hx_neighbor_support(algo, nr, nc, thold) < 2)
				continue;
			algo->zone_map[ni] = zone_id;
			algo->bfs_queue[tail++] = ni;
		}
	}

	if (w_total > 0) {
		ct->x = (s32)(w_col * 2 / w_total) + 0x80;
		ct->y = (s32)(w_row * 2 / w_total) + 0x80;
	} else {
		ct->x = pk->c * 256 + 128;
		ct->y = pk->r * 256 + 128;
	}
	ct->area = area;
	ct->signal_sum = sig_sum;
	ct->is_edge = (pk->r == 0 || pk->r == HX_ROWS - 1 ||
		       pk->c == 0 || pk->c == HX_COLS - 1);
	ct->peak_index = pi;

	return clean;
}

/*
 * Multi-peak fallback: 3x3 local weighted centroid around the peak.
 */
static void hx_local_centroid(struct hx_algo *algo, int pi,
			       struct hx_contact *ct)
{
	struct hx_peak *pk = &algo->peaks[pi];
	s64 w_col = 0, w_row = 0;
	s32 w_total = 0;
	u16 area = 0;
	s32 sig_sum = 0;
	int dr, dc;

	for (dr = -1; dr <= 1; dr++) {
		for (dc = -1; dc <= 1; dc++) {
			int nr = pk->r + dr;
			int nc = pk->c + dc;
			s16 sig;

			if (nr < 0 || nr >= HX_ROWS || nc < 0 || nc >= HX_COLS)
				continue;
			sig = hx_frame_at(algo, nr, nc);
			if (sig <= 0)
				continue;
			w_col += (s64)nc * 128 * sig;
			w_row += (s64)nr * 128 * sig;
			w_total += sig;
			area++;
			sig_sum += sig;
		}
	}

	if (w_total > 0) {
		ct->x = (s32)(w_col * 2 / w_total) + 0x80;
		ct->y = (s32)(w_row * 2 / w_total) + 0x80;
	} else {
		ct->x = pk->c * 256 + 128;
		ct->y = pk->r * 256 + 128;
	}
	ct->area = area;
	ct->signal_sum = sig_sum;
	ct->is_edge = (pk->r == 0 || pk->r == HX_ROWS - 1 ||
		       pk->c == 0 || pk->c == HX_COLS - 1);
	ct->peak_index = pi;
}

/*
 * Edge compensation: push centroid outward toward the physical sensor
 * boundary.  The sensor extends ~0.5 cells beyond the last grid node,
 * but the weighted centroid is biased inward because there's no data
 * outside the grid.  This function linearly pushes edge contacts
 * outward, with maximum push at the boundary itself, fading to zero
 * at edge_blend_q8 distance from the edge.
 */
static void hx_edge_compensate(struct hx_algo *algo, struct hx_contact *ct)
{
	s32 push_max = algo->edge_push_q8;
	s32 blend    = algo->edge_blend_q8;
	s32 dist, push;

	if (!algo->edge_comp_enabled || push_max <= 0 || blend <= 0)
		return;

	/* Left boundary: distance = ct->x (Q8.8, 0 = grid col 0 center) */
	dist = ct->x;
	if (dist < blend) {
		push = push_max * (blend - dist) / blend;
		ct->x = max_t(s32, ct->x - push, 0);
	}

	/* Right boundary: distance from last col center */
	dist = (HX_COLS - 1) * 256 + 128 - ct->x;
	if (dist < blend) {
		push = push_max * (blend - dist) / blend;
		ct->x = min_t(s32, ct->x + push, (HX_COLS - 1) * 256 + 256);
	}

	/* Top boundary */
	dist = ct->y;
	if (dist < blend) {
		push = push_max * (blend - dist) / blend;
		ct->y = max_t(s32, ct->y - push, 0);
	}

	/* Bottom boundary */
	dist = (HX_ROWS - 1) * 256 + 128 - ct->y;
	if (dist < blend) {
		push = push_max * (blend - dist) / blend;
		ct->y = min_t(s32, ct->y + push, (HX_ROWS - 1) * 256 + 256);
	}
}

void hx_expand_and_resolve(struct hx_algo *algo,
			    struct input_mt_pos *pos, int *cnt)
{
	int i, n;

	memset(algo->zone_map, 0, sizeof(algo->zone_map));
	algo->contact_count = 0;

	n = min_t(int, algo->peak_count, HX_MAX_PEAKS);

	for (i = 0; i < n; i++) {
		struct hx_contact *ct = &algo->contacts[algo->contact_count];
		bool clean;

		clean = hx_expand_single_peak(algo, i, ct);
		if (!clean)
			hx_local_centroid(algo, i, ct);

		/* Push edge centroids outward toward physical sensor boundary */
		if (ct->is_edge)
			hx_edge_compensate(algo, ct);

		algo->contact_count++;
	}

	/* If more peaks than slots, keep the strongest signal_sum contacts.
	 * Do this after resolving every stored peak: peaks are intentionally
	 * processed weakest-first for deterministic zone ownership. */
	if (algo->contact_count > HIMAX_MAX_TOUCH) {
		/* Selection-sort descending by signal_sum, keep first MAX */
		u8 ci, cj;

		for (ci = 0; ci + 1 < algo->contact_count; ci++) {
			u8 best = ci;

			for (cj = ci + 1; cj < algo->contact_count; cj++) {
				if (algo->contacts[cj].signal_sum >
				    algo->contacts[best].signal_sum)
					best = cj;
			}
			if (best != ci)
				swap(algo->contacts[ci], algo->contacts[best]);
		}
		algo->contact_count = HIMAX_MAX_TOUCH;
	}

	/* Convert Q8.8 grid coordinates to output space matching rxtx2xy:
	 *   x = ct->x / 6       (maps to [~21, ~2539])
	 *   y = 5 * ct->y / 32  (maps to [~20, ~1580])
	 * This matches the coordinate range the DT/touchscreen_properties
	 * are calibrated for.
	 */
	*cnt = 0;
	for (i = 0; i < algo->contact_count; i++) {
		struct hx_contact *ct = &algo->contacts[i];
		s32 x = clamp_val((s32)(ct->x / 6), 0, SZ_64K - 1);
		s32 y = clamp_val((s32)(5 * ct->y / 32), 0, SZ_64K - 1);
		bool near_edge = x < algo->edge_reject_margin ||
			x > (HX_COLS * 256 / 6) - algo->edge_reject_margin ||
			y < algo->edge_reject_margin ||
			y > (HX_ROWS * 40) - algo->edge_reject_margin;

		if (algo->edge_reject_enabled && near_edge &&
		    ct->signal_sum < algo->edge_reject_min_signal)
			continue;
		if (*cnt != i)
			algo->contacts[*cnt] = *ct;
		pos[*cnt].x = x;
		pos[*cnt].y = y;
		(*cnt)++;
	}
	algo->contact_count = *cnt;
}

/* ======================================================================== */
/* Phase 3A — Hungarian distance-matching tracker                           */
/* ======================================================================== */

/*
 * Squared distance from a detection to a track's *predicted* position.
 * Prediction: next_pos = current_pos + velocity.
 */
static inline s64 hx_dist2_predicted(const struct input_mt_pos *a,
					       const struct hx_track *b)
{
	s32 frames_ahead = max_t(s32, 1, (s32)b->missed + 1);
	s32 pred_x = b->x + b->vx * frames_ahead;
	s32 pred_y = b->y + b->vy * frames_ahead;
	s32 dx = a->x - pred_x;
	s32 dy = a->y - pred_y;

	return (s64)dx * dx + (s64)dy * dy;
}

static void hx_reset_track(struct hx_track *trk)
{
	memset(trk, 0, sizeof(*trk));
}

static void hx_filter_track(struct hx_algo *algo, struct hx_track *trk,
			    const struct input_mt_pos *det)
{
	s32 dx = det->x - trk->x;
	s32 dy = det->y - trk->y;
	s32 speed = max(abs(dx), abs(dy));
	s32 alpha;

	trk->vx = dx;
	trk->vy = dy;
	if (!algo->track_smoothing || !algo->euro_enabled) {
		trk->x = det->x;
		trk->y = det->y;
		trk->filtered_x_q8 = det->x << 8;
		trk->filtered_y_q8 = det->y << 8;
		return;
	}

	alpha = min(algo->euro_alpha_min_q8, algo->euro_alpha_max_q8);
	if (algo->euro_speed_threshold)
		alpha += (s32)(max(algo->euro_alpha_min_q8,
				 algo->euro_alpha_max_q8) - alpha) *
			 min_t(s32, speed, algo->euro_speed_threshold) /
			 algo->euro_speed_threshold;
	alpha = clamp_t(s32, alpha, 1, 255);
	trk->deriv_x_q8 += (alpha * ((dx << 8) - trk->deriv_x_q8)) >> 8;
	trk->deriv_y_q8 += (alpha * ((dy << 8) - trk->deriv_y_q8)) >> 8;
	trk->filtered_x_q8 +=
		(alpha * ((det->x << 8) - trk->filtered_x_q8)) >> 8;
	trk->filtered_y_q8 +=
		(alpha * ((det->y << 8) - trk->filtered_y_q8)) >> 8;
	trk->x = trk->filtered_x_q8 >> 8;
	trk->y = trk->filtered_y_q8 >> 8;
}

static void hx_hungarian_assign(struct hx_algo *algo, const u8 *active,
				int active_cnt, int det_cnt, s8 *match)
{
	const s64 inf = (s64)1 << 55;
	int cols = det_cnt + active_cnt;
	int i, j;

	memset(algo->assign_u, 0, sizeof(algo->assign_u));
	memset(algo->assign_v, 0, sizeof(algo->assign_v));
	memset(algo->assign_p, 0, sizeof(algo->assign_p));
	memset(algo->assign_way, 0, sizeof(algo->assign_way));
	for (i = 1; i <= active_cnt; i++) {
		s64 minv[HX_ASSIGN_COLS + 1];
		bool used[HX_ASSIGN_COLS + 1] = { false };
		int j0 = 0;

		for (j = 0; j <= cols; j++)
			minv[j] = inf;
		algo->assign_p[0] = i;
		do {
			int i0, j1 = 0;
			s64 delta = inf;

			used[j0] = true;
			i0 = algo->assign_p[j0];
			for (j = 1; j <= cols; j++) {
				s64 cur;

				if (used[j])
					continue;
				cur = algo->assign_cost[i0 - 1][j - 1] -
					algo->assign_u[i0] - algo->assign_v[j];
				if (cur < minv[j]) {
					minv[j] = cur;
					algo->assign_way[j] = j0;
				}
				if (minv[j] < delta) {
					delta = minv[j];
					j1 = j;
				}
			}
			for (j = 0; j <= cols; j++) {
				if (used[j]) {
					algo->assign_u[algo->assign_p[j]] += delta;
					algo->assign_v[j] -= delta;
				} else {
					minv[j] -= delta;
				}
			}
			j0 = j1;
		} while (algo->assign_p[j0]);
		do {
			int j1 = algo->assign_way[j0];

			algo->assign_p[j0] = algo->assign_p[j1];
			j0 = j1;
		} while (j0);
	}

	for (i = 0; i < HIMAX_MAX_TOUCH; i++)
		match[i] = -1;
	for (j = 1; j <= det_cnt; j++) {
		int row = algo->assign_p[j];

		if (row && algo->assign_cost[row - 1][j - 1] < inf / 2)
			match[active[row - 1]] = j - 1;
	}
}

void hx_track_contacts(struct hx_algo *algo,
		       struct input_mt_pos *det, int det_cnt)
{
	bool det_used[HIMAX_MAX_TOUCH] = { false };
	u8 active[HIMAX_MAX_TOUCH];
	s8 match[HIMAX_MAX_TOUCH];
	u16  jump_released = 0;  /* bitmask: slots freed by jump detection */
	int active_cnt = 0;
	int i, j;
	const s64 inf = (s64)1 << 55;

	for (i = 0; i < HIMAX_MAX_TOUCH; i++) {
		if (algo->tracks[i].active)
			active[active_cnt++] = i;
	}
	for (i = 0; i < active_cnt; i++) {
		struct hx_track *trk = &algo->tracks[active[i]];
		s64 unmatched = max_t(s64, algo->track_dist2_max, 1) * 16;
		for (j = 0; j < det_cnt; j++) {
			s64 d2 = hx_dist2_predicted(&det[j], trk);
			s64 gate2 = max_t(s64, algo->track_dist2_max, 1);
			if (trk->missed)
				gate2 *= min_t(s32, (s32)trk->missed + 1, 4);
			algo->assign_cost[i][j] = d2 <= gate2 ? d2 : inf;
		}
		for (j = det_cnt; j < det_cnt + active_cnt; j++)
			algo->assign_cost[i][j] = unmatched;
	}
	if (algo->hungarian_enabled) {
		hx_hungarian_assign(algo, active, active_cnt, det_cnt, match);
	} else {
		memset(match, -1, sizeof(match));
		for (i = 0; i < active_cnt; i++) {
			s64 best = inf;

			for (j = 0; j < det_cnt; j++)
				if (!det_used[j] && algo->assign_cost[i][j] < best) {
					best = algo->assign_cost[i][j];
					match[active[i]] = j;
				}
			if (match[active[i]] >= 0)
				det_used[match[active[i]]] = true;
		}
		memset(det_used, 0, sizeof(det_used));
	}

	for (i = 0; i < active_cnt; i++) {
		int ti = active[i];
		int di = match[ti];
		struct hx_track *trk = &algo->tracks[ti];

		if (di < 0)
			continue;

		/* Jump detection: if the actual (non-predicted) displacement
		 * exceeds the jump threshold, this is a finger swap, not a
		 * slide.  Release the old slot and let the detection spawn
		 * a new track at a *different* slot so that lift + press
		 * both appear in the same SYN_REPORT (zero added latency).
		 */
		if (algo->track_jump_dist2 > 0 && trk->age >= 2) {
			s32 dx = det[di].x - trk->x;
			s32 dy = det[di].y - trk->y;
			s64 actual_d2 = (s64)dx * dx + (s64)dy * dy;

			if (actual_d2 > algo->track_jump_dist2) {
				hx_reset_track(trk);
				jump_released |= (1u << ti);
				/* det stays unused → picked up by new-slot logic */
				continue;
			}
		}

		hx_filter_track(algo, trk, &det[di]);
		trk->missed = 0;
		if (di < algo->contact_count)
			trk->signal_sum = algo->contacts[di].signal_sum;
		if (trk->age < U8_MAX)
			trk->age++;
		if (trk->debounce > 0)
			trk->debounce--;
		det_used[di] = true;
	}

	/* Age or release unmatched tracks. */
	for (i = 0; i < HIMAX_MAX_TOUCH; i++) {
		struct hx_track *trk = &algo->tracks[i];

		if (!trk->active || match[i] >= 0)
			continue;

		/*
		 * Before the first stable touch is established, drop stray
		 * tracks immediately to prevent noise from being reported.
		 */
		if (algo->track_active_guard && !algo->touch_active) {
			hx_reset_track(trk);
			continue;
		}

		trk->missed++;
		if (trk->missed > algo->track_lost_frames)
			hx_reset_track(trk);
	}

	/* Create new slots for unmatched detections. */
	for (j = 0; j < det_cnt; j++) {
		struct hx_track *trk = NULL;

		if (det_used[j])
			continue;

		for (i = 0; i < HIMAX_MAX_TOUCH; i++) {
			if (!algo->tracks[i].active &&
			    !(jump_released & (1u << i))) {
				trk = &algo->tracks[i];
				break;
			}
		}
		if (!trk)
			continue;

		trk->active   = true;
		trk->age      = 1;
		trk->missed   = 0;
		trk->debounce = algo->debounce_base;
		if (j < algo->contact_count) {
			if (algo->contacts[j].signal_sum < algo->debounce_strong_signal)
				trk->debounce += algo->debounce_weak_extra;
			if (algo->contacts[j].is_edge)
				trk->debounce += algo->debounce_edge_extra;
		}
		trk->x        = det[j].x;
		trk->y        = det[j].y;
		trk->vx       = 0;
		trk->vy       = 0;
		trk->filtered_x_q8 = det[j].x << 8;
		trk->filtered_y_q8 = det[j].y << 8;
		if (j < algo->contact_count)
			trk->signal_sum = algo->contacts[j].signal_sum;
		trk->reported = trk->debounce == 0 && algo->touch_active;
	}
}

int hx_count_stable_tracks(struct hx_algo *algo)
{
	int i, cnt = 0;

	for (i = 0; i < HIMAX_MAX_TOUCH; i++) {
		if (algo->tracks[i].active && algo->tracks[i].reported)
			cnt++;
	}
	return cnt;
}

static void hx_suppress_rx_ghosts(struct hx_algo *algo,
				  struct input_mt_pos *det, int *det_cnt)
{
	bool drop[HIMAX_MAX_TOUCH] = { false };
	int i, j, dst;

	if (!algo->ghost_enabled)
		return;
	for (i = 0; i < *det_cnt; i++) {
		for (j = i + 1; j < *det_cnt; j++) {
			int weak, strong, ti;
			bool linked = false;

			if (abs(det[i].y - det[j].y) > algo->ghost_row_distance ||
			    abs(det[i].x - det[j].x) < algo->ghost_min_col_distance)
				continue;
			strong = algo->contacts[i].signal_sum >=
				 algo->contacts[j].signal_sum ? i : j;
			weak = strong == i ? j : i;
			if ((s64)algo->contacts[weak].signal_sum * 256 >
			    (s64)algo->contacts[strong].signal_sum *
				algo->ghost_weak_ratio_q8)
				continue;
			for (ti = 0; ti < HIMAX_MAX_TOUCH; ti++)
				if (algo->tracks[ti].active &&
				    hx_dist2_predicted(&det[weak], &algo->tracks[ti]) <=
					algo->track_dist2_max) {
					linked = true;
					break;
				}
			if (!linked)
				drop[weak] = true;
		}
	}
	for (i = 0, dst = 0; i < *det_cnt; i++) {
		if (drop[i])
			continue;
		if (dst != i) {
			det[dst] = det[i];
			algo->contacts[dst] = algo->contacts[i];
		}
		dst++;
	}
	*det_cnt = dst;
	algo->contact_count = dst;
}

static void hx_update_gesture(struct hx_algo *algo)
{
	struct hx_track *first = NULL;
	int reported = 0;
	int i;

	for (i = 0; i < HIMAX_MAX_TOUCH; i++) {
		if (algo->tracks[i].active && algo->tracks[i].reported) {
			if (!first)
				first = &algo->tracks[i];
			reported++;
		}
	}
	if (!reported) {
		if (algo->gesture_state == HX_GESTURE_RELEASE)
			algo->gesture_state = HX_GESTURE_IDLE;
		else if (algo->gesture_state != HX_GESTURE_IDLE)
			algo->gesture_state = HX_GESTURE_RELEASE;
		algo->gesture_frames = 0;
		return;
	}
	if (algo->gesture_state == HX_GESTURE_IDLE ||
	    algo->gesture_state == HX_GESTURE_RELEASE) {
		algo->gesture_state = HX_GESTURE_PRESS;
		algo->gesture_frames = 1;
		algo->gesture_start_x = first->x;
		algo->gesture_start_y = first->y;
		return;
	}
	if (algo->gesture_frames < U16_MAX)
		algo->gesture_frames++;
	if (abs(first->x - algo->gesture_start_x) >=
			algo->gesture_drag_distance ||
	    abs(first->y - algo->gesture_start_y) >=
			algo->gesture_drag_distance)
		algo->gesture_state = HX_GESTURE_DRAG;
	else if (algo->gesture_frames >= algo->gesture_long_press_frames)
		algo->gesture_state = HX_GESTURE_LONG_PRESS;
}

int hx_algo_process_frame(struct hx_algo *algo, const u16 *raw)
{
	return hx_algo_process_frame_state(algo, raw, HX_FINGER_UNKNOWN);
}

int hx_algo_process_frame_state(struct hx_algo *algo, const u16 *raw,
				enum hx_finger_state finger_state)
{
	struct input_mt_pos det[HIMAX_MAX_TOUCH];
	int det_cnt = 0;
	int stable = 0;
	int i;
	s16 frame_max = 0;

	algo->diag_frame_seq++;

	hx_preprocess_frame_state(algo, raw, finger_state);
	for (i = 0; i < HX_PIXELS; i++)
		frame_max = max(frame_max, ((s16 *)algo->frame)[i]);
	algo->diag_frame_max = frame_max;

	/* The controller's master-frame status is authoritative for lift-off.
	 * Do not let matrix rebound or the tracker's silent-gap window extend an
	 * already confirmed UP event.  The gap window remains useful for UNKNOWN
	 * status (host tests/legacy callers) and invalid frames, which never enter
	 * this function from the IRQ path. */
	if (finger_state == HX_FINGER_ABSENT) {
		algo->zone_count = 0;
		algo->peak_count = 0;
		algo->contact_count = 0;
		algo->diag_zones = 0;
		algo->diag_peaks = 0;
		algo->diag_contacts_pre_filter = 0;
		algo->diag_contacts_post_filter = 0;
		for (i = 0; i < HIMAX_MAX_TOUCH; i++)
			hx_reset_track(&algo->tracks[i]);
		goto update_state;
	}

	hx_detect_macro_zones(algo);
	algo->diag_zones = algo->zone_count;
	hx_reject_palms(algo);
	hx_detect_peaks(algo);
	algo->diag_peaks = algo->peak_count;
	hx_expand_and_resolve(algo, det, &det_cnt);
	algo->diag_contacts_pre_filter = det_cnt;
	hx_suppress_rx_ghosts(algo, det, &det_cnt);
	algo->diag_contacts_post_filter = det_cnt;
	hx_track_contacts(algo, det, det_cnt);

update_state:
	for (i = 0; i < HIMAX_MAX_TOUCH; i++)
		if (algo->tracks[i].active && algo->tracks[i].debounce == 0)
			stable++;
	if (stable) {
		if (!algo->touch_active &&
		    ++algo->touch_start_frames >= algo->track_start_debounce)
			algo->touch_active = true;
	} else {
		algo->touch_start_frames = 0;
		algo->touch_active = false;
	}
	algo->diag_active_tracks = 0;
	algo->diag_reported_tracks = 0;
	for (i = 0; i < HIMAX_MAX_TOUCH; i++) {
		if (algo->tracks[i].active)
			algo->diag_active_tracks++;
		if (algo->tracks[i].active && algo->tracks[i].debounce == 0 &&
		    algo->touch_active)
			algo->tracks[i].reported = true;
		if (algo->tracks[i].reported)
			algo->diag_reported_tracks++;
	}

	hx_update_gesture(algo);
	return hx_count_stable_tracks(algo);
}
