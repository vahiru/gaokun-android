/* SPDX-License-Identifier: LGPL-2.1-or-later */
#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Adapted from the FTE7001/FT9338 matcher. The geometry travels with each
 * frame: one image serves FocalTech FTE7001 (40 x 196) and Goodix GDIX5125
 * (36 x 160) units, and the sensor is only known at run time. */
#define GAOKUN3_MAX_WIDTH 40
#define GAOKUN3_MAX_HEIGHT 196
#define GAOKUN3_MAX_PIXELS (GAOKUN3_MAX_WIDTH * GAOKUN3_MAX_HEIGHT)
#define GAOKUN3_MIN_SIDE 20

typedef struct {
  uint16_t width, height;              /* row major, width * height used */
  uint8_t pixels[GAOKUN3_MAX_PIXELS];  /* unused tail stays zero */
} Gaokun3Frame;

typedef struct {
  int x, y;
  float v;
  int layer;
} Gaokun3Keypoint;

typedef struct {
  Gaokun3Keypoint *kps;
  float (*descs)[128];
  int n_feats;
} Gaokun3FeatureSet;

bool gaokun3_frame_geometry_valid (const Gaokun3Frame *frame);
/* Frames with an invalid geometry fail the quality check and yield no
 * features. */
bool gaokun3_quality_check (const Gaokun3Frame *frame,
                            float *coverage, float *mean_tc);
Gaokun3FeatureSet gaokun3_feature_extract (const Gaokun3Frame *frame);
void gaokun3_feature_set_free (Gaokun3FeatureSet *fs);
float gaokun3_match_score (const Gaokun3FeatureSet *a,
                           const Gaokun3FeatureSet *b);
