/* SPDX-License-Identifier: LGPL-2.1-or-later */
#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Adapted from the FTE7001/FT9338 matcher for Gaokun3's 40 x 196 images. */
#define GAOKUN3_WIDTH 40
#define GAOKUN3_HEIGHT 196
#define GAOKUN3_PIXELS (GAOKUN3_WIDTH * GAOKUN3_HEIGHT)

typedef struct {
  uint8_t pixels[GAOKUN3_PIXELS];
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

bool gaokun3_quality_check (const Gaokun3Frame *frame,
                            float *coverage, float *mean_tc);
Gaokun3FeatureSet gaokun3_feature_extract (const Gaokun3Frame *frame);
void gaokun3_feature_set_free (Gaokun3FeatureSet *fs);
float gaokun3_match_score (const Gaokun3FeatureSet *a,
                           const Gaokun3FeatureSet *b);
