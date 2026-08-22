/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Himax HX83121A touch algorithm — data structures and interface.
 *
 * All algorithm state lives in struct hx_algo, which is allocated once in
 * probe via devm_kzalloc and referenced from struct himax_ts_data.
 */
#ifndef HX_ALGO_H
#define HX_ALGO_H

#ifdef HX_ALGO_HOST_TEST
#include <stdbool.h>
#include <stdint.h>
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef int8_t s8;
typedef int16_t s16;
typedef int32_t s32;
typedef int64_t s64;
struct input_mt_pos { int x; int y; };
#else
#include <linux/input/mt.h>
#include <linux/types.h>
#endif

/* Grid dimensions — must match the firmware raw frame layout. */
#define HX_ROWS        40
#define HX_COLS        60
#define HX_PIXELS      (HX_ROWS * HX_COLS)  /* 2400 */

/* Detection limits */
#define HX_MAX_ZONES   20
#define HX_MAX_PEAKS   20
#define HX_MAX_PALM_BOXES 8
#define HX_ASSIGN_COLS (HIMAX_MAX_TOUCH * 2)
#define HX_BASELINE_HIST_BINS 2048

/* Maximum simultaneous reported contacts. */
#define HIMAX_MAX_TOUCH 10

/**
 * struct hx_peak - single detected local-maximum candidate.
 * @r:          row in the grid (0-based)
 * @c:          column in the grid
 * @z:          signal value at the peak
 * @nbr_sum:    sum of all 8-neighbour signals (used for Z8 filter)
 * @zone_area:  area of the macro-zone this peak belongs to
 * @zone_index: index of the owning macro-zone
 * @on_edge:    true when the peak lies on the outermost grid row or column
 */
struct hx_peak {
	u8  r;
	u8  c;
	s16 z;
	s32 nbr_sum;
	u16 zone_area;
	u8  zone_index;
	bool on_edge;
};

/**
 * struct hx_contact - sub-pixel coordinate after centroid expansion.
 * @x, @y:      Q8.8 fixed-point grid coordinates
 * @area:       number of pixels contributing to this contact
 * @signal_sum: integrated signal over the contact area
 * @is_edge:    true when the originating peak lies on the grid boundary
 * @peak_index: index of the peak that produced this contact
 */
struct hx_contact {
	s32  x;
	s32  y;
	u16  area;
	s32  signal_sum;
	bool is_edge;
	u8   peak_index;
};

/**
 * struct hx_track - persistent touch slot state.
 * @active:     slot is in use
 * @x, @y:      current output coordinates [0, 65535]
 * @vx, @vy:    velocity in output units per frame (for prediction)
 * @signal_sum: integrated signal (forwarded to pressure reporting)
 * @age:        frames the slot has been active
 * @missed:     consecutive frames the slot had no matching detection
 * @debounce:   remaining debounce frames before the slot is reported
 */
struct hx_track {
	bool active;
	bool reported;
	s32  x;
	s32  y;
	s32  vx;
	s32  vy;
	s32  signal_sum;
	u8   age;
	u8   missed;
	u8   debounce;
	s32  filtered_x_q8;
	s32  filtered_y_q8;
	s32  deriv_x_q8;
	s32  deriv_y_q8;
};

/**
 * struct hx_macro_zone - contiguous above-threshold region.
 * @arena_start: first pixel in hx_algo.zone_arena
 * @area:       total pixel count
 * @signal_sum: sum of positive pixel values within the zone
 * @min_r … max_c: bounding box
 */
struct hx_macro_zone {
	u16 arena_start;
	u16 area;
	s32 signal_sum;
	u8  min_r;
	u8  max_r;
	u8  min_c;
	u8  max_c;
};

struct hx_palm_box {
	bool active;
	u8 min_r, max_r, min_c, max_c;
	u16 missed;
};

enum hx_gesture_state {
	HX_GESTURE_IDLE = 0,
	HX_GESTURE_PRESS,
	HX_GESTURE_DRAG,
	HX_GESTURE_LONG_PRESS,
	HX_GESTURE_RELEASE,
};

/**
 * struct hx_algo - all algorithm state, allocated once in probe.
 *
 * Memory budget: ~40 KB.  Never allocate on the stack.
 */
struct hx_algo {
	/* ---- Frame buffers ---- */
	s16 frame[HX_ROWS][HX_COLS];        /* baseline-subtracted signal  */
	s16 iir_history[HX_ROWS][HX_COLS];  /* IIR temporal-filter history */
	bool iir_initialized;

	/* ---- Scratch buffers (shared between pipeline stages) ---- */
	u8  visited[HX_PIXELS];              /* BFS visited flags           */
	u8  zone_map[HX_PIXELS];             /* per-pixel zone-ID map       */
	u16 bfs_queue[HX_PIXELS];            /* ring-buffer BFS queue       */
	u16 zone_arena[HX_PIXELS];           /* all zone pixels, no truncation */
	u16 zone_arena_used;

	/* ---- Detection results ---- */
	struct hx_macro_zone zones[HX_MAX_ZONES];
	u8   zone_count;

	struct hx_peak peaks[HX_MAX_PEAKS];
	u8   peak_count;

	/* Keep every peak candidate until the final strength-based capacity cut.
	 * The old implementation truncated the ascending peak list to ten first,
	 * which selected the ten weakest candidates on noisy frames. */
	struct hx_contact contacts[HX_MAX_PEAKS];
	u8   contact_count;

	/* ---- Per-cell adaptive baseline (Q8 fixed point) ---- */
	s32 baseline_q8[HX_PIXELS];
	u8  baseline_release_hold[HX_PIXELS];
	u16 baseline_hist[HX_BASELINE_HIST_BINS];
	bool baseline_initialized;
	bool baseline_prev_had_signal;
	bool baseline_had_freeze;
	u8 baseline_recovery_frames;

	/* Read-only pipeline snapshot for diagnosing hardware-only dropouts. */
	u32 diag_frame_seq;
	s32 diag_common_diff;
	s16 diag_frame_max;
	u8 diag_has_signal;
	u8 diag_zones;
	u8 diag_peaks;
	u8 diag_contacts_pre_filter;
	u8 diag_contacts_post_filter;
	u8 diag_active_tracks;
	u8 diag_reported_tracks;

	struct hx_palm_box palm_boxes[HX_MAX_PALM_BOXES];
	u8 palm_box_count;

	/* ---- Tracking state ---- */
	struct hx_track tracks[HIMAX_MAX_TOUCH];
	bool touch_active;
	u8   touch_start_frames;
	enum hx_gesture_state gesture_state;
	u16 gesture_frames;
	s32 gesture_start_x;
	s32 gesture_start_y;

	/* Hungarian scratch is kept off the kernel stack. */
	s64 assign_cost[HIMAX_MAX_TOUCH][HX_ASSIGN_COLS];
	s64 assign_u[HIMAX_MAX_TOUCH + 1];
	s64 assign_v[HX_ASSIGN_COLS + 1];
	u8 assign_p[HX_ASSIGN_COLS + 1];
	u8 assign_way[HX_ASSIGN_COLS + 1];

	/* ---- Tunable parameters (sysfs-writable, atomically updated) ---- */
	/* Preprocessing */
	bool baseline_enabled;
	u16  baseline_initial;
	s16  baseline_noise_deadband;
	s16  baseline_positive_deadband;
	s16  baseline_negative_deadband;
	s16  baseline_peak_threshold;
	u8   baseline_release_hold_frames;
	u8   baseline_positive_alpha_shift;
	u8   baseline_negative_alpha_shift;
	u8   baseline_noise_alpha_shift;
	s16  baseline_positive_max_step;
	s16  baseline_negative_max_step;
	u8   baseline_background_alpha_shift;
	u8   baseline_no_finger_alpha_shift;
	u8   baseline_recovery_alpha_shift;
	s16  baseline_background_max_step;
	s16  baseline_no_finger_max_step;
	s16  baseline_recovery_max_step;
	u8   baseline_recovery_max_frames;
	bool baseline_noise_tracking;
	bool cmf_enabled;          /* CMF on/off (default: true)           */
	s16  cmf_exclusion;        /* exclude pixels > this from CMF mean  */
	s16  cmf_max_correction;   /* clamp per-row/col offset             */
	bool iir_enabled;          /* IIR temporal filter on/off           */
	u16  iir_decay_weight;     /* blend weight 0-256 (256 = no blend)  */
	u16  iir_decay_step;       /* per-frame decay in signal units      */
	s16  iir_noise_floor;      /* clamp-to-zero below this             */
	s16  iir_gate_floor;       /* min dynamic threshold                */
	u8   iir_gate_ratio_q8;    /* dyn threshold = max * ratio/256      */
	/* Detection */
	s16  macro_threshold;      /* minimum pixel value to seed BFS      */
	s16  peak_threshold;       /* minimum peak signal                  */
	u8   peak_local_radius;
	bool peak_z8_enabled;
	bool peak_saddle_enabled;
	u8   peak_saddle_radius;
	s16  peak_saddle_drop;
	s16  peak_signal_threshold_limit;
	s16  peak_edge_threshold;
	u8   peak_macro_min_area;
	bool palm_enabled;         /* palm-rejection on/off                */
	u8   palm_area_threshold;  /* area >= this → palm                  */
	s32  palm_signal_threshold;/* signal_sum >= this → palm            */
	s16  palm_density_low;     /* signal/area < this → palm            */
	bool palm_box_enabled;
	u8 palm_box_expand_rows;
	u8 palm_box_expand_cols;
	u8 palm_box_match_distance;
	u16 palm_box_max_hold;
	bool zone_cleanup_enabled;
	u8 zone_max_radius;
	u8 zone_threshold_numer;
	u8 zone_threshold_shift;
	/* Pressure / touch-major reporting */
	bool pressure_enabled;     /* report PRESSURE + TOUCH_MAJOR        */
	/* Edge compensation */
	bool edge_comp_enabled;    /* edge compensation on/off             */
	s16  edge_boost_pct;       /* signal boost for border pixels (%)   */
	s16  edge_push_q8;         /* max outward push in Q8.8 (128=0.5)  */
	s16  edge_blend_q8;        /* blend range in Q8.8 (512=2 cells)   */
	bool edge_reject_enabled;
	u16 edge_reject_margin;
	s32 edge_reject_min_signal;
	/* Tracking */
	s32  track_dist2_max;      /* max squared distance for match       */
	u8   track_lost_frames;    /* missed frames before slot release    */
	u8   debounce_base;        /* new-slot debounce count              */
	bool track_smoothing;      /* position smoothing on/off            */
	bool track_active_guard;   /* kill stray tracks before 1st stable  */
	u8   track_start_debounce; /* frames to confirm touch_active       */
	s32  track_jump_dist2;     /* position jump → force lift+repress   */
	bool hungarian_enabled;
	u8 debounce_weak_extra;
	u8 debounce_edge_extra;
	s32 debounce_strong_signal;
	bool ghost_enabled;
	u16 ghost_row_distance;
	u8 ghost_weak_ratio_q8;
	u16 ghost_min_col_distance;
	bool euro_enabled;
	u8 euro_alpha_min_q8;
	u8 euro_alpha_max_q8;
	u16 euro_speed_threshold;
	u16 gesture_drag_distance;
	u16 gesture_long_press_frames;
};

enum hx_finger_state {
	HX_FINGER_UNKNOWN,
	HX_FINGER_ABSENT,
	HX_FINGER_PRESENT,
};

/* ---- Public API ---- */

void hx_algo_init_defaults(struct hx_algo *algo);
void hx_algo_reset_runtime(struct hx_algo *algo);
int hx_algo_process_frame(struct hx_algo *algo, const u16 *raw);
int hx_algo_process_frame_state(struct hx_algo *algo, const u16 *raw,
				enum hx_finger_state finger_state);

/* Phase 1: preprocessing (baseline subtraction, CMF, IIR) */
void hx_preprocess_frame(struct hx_algo *algo, const u16 *raw);
void hx_preprocess_frame_state(struct hx_algo *algo, const u16 *raw,
			       enum hx_finger_state finger_state);

/* Phase 2A: macro-zone detection */
void hx_detect_macro_zones(struct hx_algo *algo);

/* Phase 2B: palm rejection */
void hx_reject_palms(struct hx_algo *algo);

/* Phase 2C: peak detection within surviving zones */
void hx_detect_peaks(struct hx_algo *algo);

/* Phase 2D: zone expansion + weighted centroid → contacts → output positions */
void hx_expand_and_resolve(struct hx_algo *algo,
			    struct input_mt_pos *pos, int *cnt);

/* Phase 3A: Hungarian or greedy tracker update with velocity prediction */
void hx_track_contacts(struct hx_algo *algo,
		       struct input_mt_pos *det, int det_cnt);

/* Count slots that have passed debounce */
int hx_count_stable_tracks(struct hx_algo *algo);

#endif /* HX_ALGO_H */
