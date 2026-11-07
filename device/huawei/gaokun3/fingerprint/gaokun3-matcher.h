/* SPDX-License-Identifier: LGPL-2.1-or-later */
#pragma once

#include <stdbool.h>
#include <stdint.h>

/* The geometry travels with each frame: one image serves FocalTech FTE7001
 * (40 x 196) and Goodix GDIX5125 (36 x 160) units, and the sensor is only
 * known at run time. */
#define GAOKUN3_MAX_WIDTH 40
#define GAOKUN3_MAX_HEIGHT 196
#define GAOKUN3_MAX_PIXELS (GAOKUN3_MAX_WIDTH * GAOKUN3_MAX_HEIGHT)
#define GAOKUN3_MIN_SIDE 20
/* Marks a ridge sample outside the finger's contact area. */
#define GAOKUN3_NO_RIDGE INT8_MIN

typedef struct {
  uint16_t width, height;              /* row major, width * height used */
  uint8_t pixels[GAOKUN3_MAX_PIXELS];  /* unused tail stays zero */
} Gaokun3Frame;

typedef struct {
  float x, y;
  float angle;   /* ridge orientation, radians in [-pi/2, pi/2) */
  int polarity;  /* +1 for a band-pass maximum, -1 for a minimum */
} Gaokun3Keypoint;

typedef struct {
  Gaokun3Keypoint *kps;
  float (*descs)[128];
  int n_feats;
  uint16_t width, height;
  /* Band-passed ridge image at local unit contrast, 32 per unit, or
   * GAOKUN3_NO_RIDGE; width * height samples. */
  int8_t *ridges;
} Gaokun3FeatureSet;

bool gaokun3_frame_geometry_valid (const Gaokun3Frame *frame);
/* Frames with an invalid geometry fail the quality check and yield no
 * features. */
bool gaokun3_quality_check (const Gaokun3Frame *frame,
                            float *coverage, float *mean_tc);
/* Resamples a frame to round(width * scale) x round(height * scale), with a
 * Gaussian prefilter when shrinking; false when the result would have no
 * valid geometry. in and out may not overlap. */
bool gaokun3_frame_resize (const Gaokun3Frame *in, double scale,
                           Gaokun3Frame *out);
Gaokun3FeatureSet gaokun3_feature_extract (const Gaokun3Frame *frame);
void gaokun3_feature_set_free (Gaokun3FeatureSet *fs);
/* 0 when nothing aligns; grows with the correlation of the aligned ridges
 * and with the area it was measured on. */
float gaokun3_match_score (const Gaokun3FeatureSet *a,
                           const Gaokun3FeatureSet *b);
