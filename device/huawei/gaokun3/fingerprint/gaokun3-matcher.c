/* SPDX-License-Identifier: LGPL-2.1-or-later
 * Host-side matcher for Gaokun3's narrow fingerprint sensors.
 *
 * A frame shows a strip of the finger, often only partly in contact, so two
 * frames of one finger share a small area at an unknown offset and rotation.
 * Keypoints with ridge-oriented gradient descriptors propose rigid
 * alignments; each proposal is refined by maximising the correlation of the
 * two band-passed ridge images over their common contact area. The evidence
 * for a pose is the Fisher z of that correlation, scaled by the square root
 * of the area it was measured on, plus the mutually most similar keypoint
 * pairs the pose explains: parallel ridges of two fingers can correlate well
 * over a small area, but their distinctive keypoints then disagree.
 */

#include "gaokun3-matcher.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define DESC_DIM 128
/* Keypoint scales around the ridge period, about 6.5 samples. */
#define DOG_LEVELS 4
static const double kDogSigmas[DOG_LEVELS] = { 1.6, 2.2, 3.1, 4.4 };
/* Ridge image band: drop sample noise below, pressure shading above. */
#define BAND_LOW_SIGMA 0.8
#define BAND_HIGH_SIGMA 3.0
/* Contact area: local ridge contrast at least this share of the frame's
 * 90th percentile, measured over a (2 * CONTRAST_RADIUS + 1)^2 window. */
#define CONTACT_SHARE 0.4
#define CONTRAST_RADIUS 4
/* One keypoint per CELL x CELL cell; descriptors cover PATCH x PATCH. */
#define CELL 6
#define PATCH 16
/* A finger meets the key at quite different angles depending on how the
 * tablet is held. With templates of enrolment frames only, five fingers'
 * presses were recognised 104 of 212 times within 40 degrees and 124 within
 * 60; other fingers' finger scores stayed under 22 in 21,000 to 28,000
 * comparisons per finger either way. Beyond 60 the gain was small (126 at 90,
 * 125 at 120) while chance scores of 18 or more grew; with no limit at all,
 * one finger's template took other fingers' frames at 37 nine times. */
#define MAX_ROTATION (60.0 * M_PI / 180.0)
/* Proposals agree within this distance and rotation. */
#define AGREE_PX 3.0
#define AGREE_RAD (20.0 * M_PI / 180.0)
/* Distinct poses correlated, and the best of them refined. */
#define PROPOSALS 8
#define REFINED 3
/* Correlations over fewer common samples are not scored. */
#define MIN_OVERLAP 400
/* Neighbouring ridge samples are correlated: one independent sample per
 * CORR_AREA samples. */
#define CORR_AREA 16.0
/* Evidence of one mutually most similar keypoint pair the pose explains,
 * in units of correlation z. */
#define KEYPOINT_WEIGHT 2.0

/* The side key is often only partly covered; MIN_OVERLAP, not this gate,
 * decides whether a partial frame carries enough ridges to match. */
#define Q_MIN_COVERAGE 0.28f
#define Q_MIN_MEAN_TC 9.0f

static int
clamp_int (int v, int lo, int hi)
{
  return v < lo ? lo : (v > hi ? hi : v);
}

bool
gaokun3_frame_geometry_valid (const Gaokun3Frame *frame)
{
  return frame && frame->width >= GAOKUN3_MIN_SIDE &&
         frame->height >= GAOKUN3_MIN_SIDE &&
         frame->width <= GAOKUN3_MAX_WIDTH &&
         frame->height <= GAOKUN3_MAX_HEIGHT;
}

bool
gaokun3_quality_check (const Gaokun3Frame *frame,
                       float *coverage, float *mean_tc)
{
  const uint8_t *px = frame ? frame->pixels : NULL;
  int w, h, y, x, i;
  double sum_tc = 0.0;
  long n_tc = 0, n_cov = 0;
  float cov, mtc;

  if (coverage)
    *coverage = 0;
  if (mean_tc)
    *mean_tc = 0;
  if (!gaokun3_frame_geometry_valid (frame))
    return false;

  w = frame->width;
  h = frame->height;
  for (y = 1; y < h - 1; y++)
    for (x = 1; x < w - 1; x++)
      {
        double m = 0.0, ss = 0.0, tc;
        for (i = 0; i < 9; i++)
          m += px[(y + i / 3 - 1) * w + x + i % 3 - 1];
        m /= 9.0;
        for (i = 0; i < 9; i++)
          {
            double d = px[(y + i / 3 - 1) * w + x + i % 3 - 1] - m;
            ss += d * d;
          }
        tc = sqrt (ss / 9.0);
        if (tc > 12.0)
          n_cov++;
        sum_tc += tc;
        n_tc++;
      }
  cov = (float) ((double) n_cov / (double) n_tc);
  mtc = (float) (sum_tc / (double) n_tc);
  if (coverage)
    *coverage = cov;
  if (mean_tc)
    *mean_tc = mtc;
  return cov >= Q_MIN_COVERAGE && mtc >= Q_MIN_MEAN_TC;
}

/* Separable Gaussian with replicated borders: a zero border would put false
 * gradients along the long edges of a 36-sample strip. */
static void
blur (const float *in, int w, int h, double sigma, float *tmp, float *out)
{
  double g[32], sum = 0.0;
  int r = (int) (2.5 * sigma + 0.5), x, y, k;

  if (r < 1)
    r = 1;
  if (r > 15)
    r = 15;
  for (k = -r; k <= r; k++)
    sum += g[k + r] = exp (-(double) (k * k) / (2.0 * sigma * sigma));
  for (k = 0; k <= 2 * r; k++)
    g[k] /= sum;
  for (y = 0; y < h; y++)
    for (x = 0; x < w; x++)
      {
        double acc = 0.0;
        for (k = -r; k <= r; k++)
          acc += g[k + r] * in[y * w + clamp_int (x + k, 0, w - 1)];
        tmp[y * w + x] = (float) acc;
      }
  for (y = 0; y < h; y++)
    for (x = 0; x < w; x++)
      {
        double acc = 0.0;
        for (k = -r; k <= r; k++)
          acc += g[k + r] * tmp[clamp_int (y + k, 0, h - 1) * w + x];
        out[y * w + x] = (float) acc;
      }
}

/* Mean of in over the clipped (2r + 1)^2 window around every sample. */
static void
box_mean (const float *in, int w, int h, int r, float *out)
{
  double *sat = calloc ((size_t) (w + 1) * (h + 1), sizeof (*sat));
  int x, y;

  if (!sat)
    {
      memset (out, 0, sizeof (float) * w * h);
      return;
    }
  for (y = 0; y < h; y++)
    for (x = 0; x < w; x++)
      sat[(y + 1) * (w + 1) + x + 1] = in[y * w + x] + sat[y * (w + 1) + x + 1] +
                                       sat[(y + 1) * (w + 1) + x] -
                                       sat[y * (w + 1) + x];
  for (y = 0; y < h; y++)
    for (x = 0; x < w; x++)
      {
        int x0 = clamp_int (x - r, 0, w), x1 = clamp_int (x + r + 1, 0, w);
        int y0 = clamp_int (y - r, 0, h), y1 = clamp_int (y + r + 1, 0, h);
        double s = sat[y1 * (w + 1) + x1] - sat[y0 * (w + 1) + x1] -
                   sat[y1 * (w + 1) + x0] + sat[y0 * (w + 1) + x0];
        /* Rounding in the table differences can leave a hair below zero
         * over a flat area; the mean of non-negative inputs is not. */
        out[y * w + x] = (float) (s > 0 ? s / ((x1 - x0) * (y1 - y0)) : 0);
      }
  free (sat);
}

static int
cmp_float (const void *a, const void *b)
{
  float x = *(const float *) a, y = *(const float *) b;
  return (x > y) - (x < y);
}

static double
sample (const float *img, int w, int h, double x, double y)
{
  int ix, iy;
  double fx, fy;
  const float *p;

  x = x < 0 ? 0 : (x > w - 1.001 ? w - 1.001 : x);
  y = y < 0 ? 0 : (y > h - 1.001 ? h - 1.001 : y);
  ix = (int) x;
  iy = (int) y;
  fx = x - ix;
  fy = y - iy;
  p = img + iy * w + ix;
  return (1 - fy) * ((1 - fx) * p[0] + fx * p[1]) +
         fy * ((1 - fx) * p[w] + fx * p[w + 1]);
}

/* Axial ridge orientation (doubled-angle structure tensor), [-pi/2, pi/2). */
static double
orientation (const float *band, int w, int h, int kx, int ky,
             const double weight[13 * 13])
{
  double sc = 0.0, ss = 0.0;
  int dx, dy;

  for (dy = -6; dy <= 6; dy++)
    for (dx = -6; dx <= 6; dx++)
      {
        int x = clamp_int (kx + dx, 1, w - 2), y = clamp_int (ky + dy, 1, h - 2);
        double gx = band[y * w + x + 1] - band[y * w + x - 1];
        double gy = band[(y + 1) * w + x] - band[(y - 1) * w + x];
        double wt = weight[(dy + 6) * 13 + dx + 6];
        sc += wt * (gx * gx - gy * gy);
        ss += wt * 2.0 * gx * gy;
      }
  return 0.5 * atan2 (ss, sc);
}

/* 4 x 4 cells x 8 gradient directions over a patch turned to the ridge
 * orientation; samples outside the contact area are left out. */
static bool
describe (const float *grad_x, const float *grad_y, const uint8_t *contact,
          int w, int h, const Gaokun3Keypoint *kp,
          const double weight[PATCH * PATCH], float desc[DESC_DIM])
{
  const double half = PATCH / 2.0 - 0.5, cell = PATCH / 4.0;
  const double c = cos (kp->angle), s = sin (kp->angle);
  double norm = 0.0;
  int px, py, i, pass;

  memset (desc, 0, sizeof (float) * DESC_DIM);
  for (py = 0; py < PATCH; py++)
    for (px = 0; px < PATCH; px++)
      {
        double rx = px - half, ry = py - half;
        double x = kp->x + c * rx - s * ry, y = kp->y + s * rx + c * ry;
        double gx, gy, a, frac, cx, cy, fx, fy, wt;
        int bin, ix, iy, xx, yy;

        if (x < 1 || y < 1 || x > w - 2 || y > h - 2 ||
            !contact[(int) lround (y) * w + (int) lround (x)])
          continue;
        gx = sample (grad_x, w, h, x, y);
        gy = sample (grad_y, w, h, x, y);
        a = fmod ((atan2 (gy, gx) - kp->angle) * 8.0 / (2.0 * M_PI) + 16.0, 8.0);
        bin = (int) a & 7;
        frac = a - floor (a);
        cx = (px + 0.5) / cell - 0.5;
        cy = (py + 0.5) / cell - 0.5;
        ix = (int) floor (cx);
        iy = (int) floor (cy);
        fx = cx - ix;
        fy = cy - iy;
        wt = hypot (gx, gy) * weight[py * PATCH + px];
        for (yy = 0; yy < 2; yy++)
          for (xx = 0; xx < 2; xx++)
            {
              int bx = ix + xx, by = iy + yy;
              double ws;
              if (bx < 0 || bx > 3 || by < 0 || by > 3)
                continue;
              ws = wt * (xx ? fx : 1 - fx) * (yy ? fy : 1 - fy);
              desc[(by * 4 + bx) * 8 + bin] += (float) (ws * (1 - frac));
              desc[(by * 4 + bx) * 8 + ((bin + 1) & 7)] += (float) (ws * frac);
            }
      }
  /* SIFT normalisation: unit length, clip strong gradients, unit again. */
  for (pass = 0; pass < 2; pass++)
    {
      norm = 0.0;
      for (i = 0; i < DESC_DIM; i++)
        norm += (double) desc[i] * desc[i];
      norm = sqrt (norm);
      if (norm < 1e-6)
        return false;
      for (i = 0; i < DESC_DIM; i++)
        desc[i] = (float) (pass ? desc[i] / norm : fmin (desc[i] / norm, 0.2));
    }
  return true;
}

bool
gaokun3_frame_resize (const Gaokun3Frame *in, double scale, Gaokun3Frame *out)
{
  int w, h, nw, nh, x, y;
  float *src, *tmp;

  if (!gaokun3_frame_geometry_valid (in) || !out || out == in || !(scale > 0))
    return false;
  w = in->width;
  h = in->height;
  nw = (int) lround (w * scale);
  nh = (int) lround (h * scale);
  memset (out, 0, sizeof (*out));
  out->width = (uint16_t) clamp_int (nw, 0, 0xffff);
  out->height = (uint16_t) clamp_int (nh, 0, 0xffff);
  if (!gaokun3_frame_geometry_valid (out))
    return false;
  src = malloc (sizeof (float) * w * h * 2);
  if (!src)
    return false;
  tmp = src + w * h;
  for (x = 0; x < w * h; x++)
    src[x] = in->pixels[x];
  /* Shrinking: keep what the coarser grid cannot hold from aliasing. */
  if (scale < 1.0)
    blur (src, w, h, 0.5 * sqrt (1.0 / (scale * scale) - 1.0), tmp, src);
  for (y = 0; y < nh; y++)
    for (x = 0; x < nw; x++)
      {
        double v = sample (src, w, h, (x + 0.5) / scale - 0.5, (y + 0.5) / scale - 0.5);
        out->pixels[y * nw + x] = (uint8_t) clamp_int ((int) lround (v), 0, 255);
      }
  free (src);
  return true;
}

Gaokun3FeatureSet
gaokun3_feature_extract (const Gaokun3Frame *frame)
{
  Gaokun3FeatureSet fs;
  int w, h, n, i, x, y, l, cols, rows;
  float *buf, *px, *tmp, *band, *contrast, *cover, *levels[DOG_LEVELS];
  float *sorted, ref;
  uint8_t *contact;
  Gaokun3Keypoint *best;
  double orient_weight[13 * 13], patch_weight[PATCH * PATCH];

  memset (&fs, 0, sizeof (fs));
  if (!gaokun3_frame_geometry_valid (frame))
    return fs;
  w = frame->width;
  h = frame->height;
  n = w * h;
  cols = (w + CELL - 1) / CELL;
  rows = (h + CELL - 1) / CELL;

  buf = malloc (sizeof (float) * n * (7 + DOG_LEVELS));
  contact = malloc ((size_t) n);
  best = calloc ((size_t) (cols * rows), sizeof (*best));
  fs.ridges = malloc ((size_t) n);
  fs.kps = malloc (sizeof (*fs.kps) * cols * rows);
  fs.descs = malloc (sizeof (*fs.descs) * cols * rows);
  if (!buf || !contact || !best || !fs.ridges || !fs.kps || !fs.descs)
    goto fail;
  px = buf;
  tmp = px + n;
  band = tmp + n;
  contrast = band + n;
  cover = contrast + n;
  sorted = cover + n;
  for (l = 0; l < DOG_LEVELS; l++)
    levels[l] = sorted + n + l * n;

  for (i = 0; i < n; i++)
    px[i] = frame->pixels[i];

  /* Ridge image and the contact area. */
  blur (px, w, h, BAND_LOW_SIGMA, tmp, band);
  blur (px, w, h, BAND_HIGH_SIGMA, tmp, contrast);
  for (i = 0; i < n; i++)
    {
      band[i] -= contrast[i];
      tmp[i] = band[i] * band[i];
    }
  box_mean (tmp, w, h, CONTRAST_RADIUS, contrast);
  for (i = 0; i < n; i++)
    sorted[i] = contrast[i] = sqrtf (contrast[i]);
  qsort (sorted, (size_t) n, sizeof (float), cmp_float);
  ref = sorted[n * 9 / 10];
  if (!(ref > 0))
    goto fail;
  for (i = 0; i < n; i++)
    {
      contact[i] = contrast[i] >= CONTACT_SHARE * ref;
      tmp[i] = contact[i];
      fs.ridges[i] = contact[i]
        ? (int8_t) clamp_int ((int) lroundf (32.0f * band[i] / contrast[i]), -127, 127)
        : GAOKUN3_NO_RIDGE;
    }
  box_mean (tmp, w, h, CONTRAST_RADIUS, cover);

  /* Difference-of-Gaussian extrema inside the contact area, the strongest
   * per cell. */
  for (l = 0; l < DOG_LEVELS; l++)
    blur (px, w, h, kDogSigmas[l], tmp, levels[l]);
  for (l = 0; l + 1 < DOG_LEVELS; l++)
    for (i = 0; i < n; i++)
      levels[l][i] = levels[l + 1][i] - levels[l][i];
  for (l = 0; l + 1 < DOG_LEVELS; l++)
    for (y = 2; y < h - 2; y++)
      for (x = 2; x < w - 2; x++)
        {
          float v = levels[l][y * w + x];
          bool is_max = true, is_min = true;
          int dl, dx, dy;
          Gaokun3Keypoint *cellbest;

          if (cover[y * w + x] < 0.9f)
            continue;
          for (dl = -1; dl <= 1; dl++)
            {
              if (l + dl < 0 || l + dl + 1 >= DOG_LEVELS)
                continue;
              for (dy = -1; dy <= 1; dy++)
                for (dx = -1; dx <= 1; dx++)
                  {
                    float u = levels[l + dl][(y + dy) * w + x + dx];
                    if (!dl && !dx && !dy)
                      continue;
                    is_max = is_max && u < v;
                    is_min = is_min && u > v;
                  }
            }
          if (!is_max && !is_min)
            continue;
          cellbest = &best[(y / CELL) * cols + x / CELL];
          if (fabsf (v) > cellbest->angle)
            {
              /* angle holds the strength until the cell is final */
              cellbest->x = (float) x;
              cellbest->y = (float) y;
              cellbest->angle = fabsf (v);
              cellbest->polarity = is_max ? 1 : -1;
            }
        }
  for (y = 0; y < 13; y++)
    for (x = 0; x < 13; x++)
      orient_weight[y * 13 + x] = exp (-(double) ((x - 6) * (x - 6) + (y - 6) * (y - 6)) / 18.0);
  for (y = 0; y < PATCH; y++)
    for (x = 0; x < PATCH; x++)
      {
        const double rx = x - (PATCH / 2.0 - 0.5), ry = y - (PATCH / 2.0 - 0.5);
        patch_weight[y * PATCH + x] = exp (-(rx * rx + ry * ry) /
                                           (2.0 * (PATCH / 2.0 - 0.5) * (PATCH / 2.0 - 0.5)));
      }
  /* Central differences of the ridge image, reusing two finished levels. */
  for (y = 0; y < h; y++)
    for (x = 0; x < w; x++)
      {
        const int xl = x > 0 ? x - 1 : x, xr = x < w - 1 ? x + 1 : x;
        const int yu = y > 0 ? y - 1 : y, yd = y < h - 1 ? y + 1 : y;
        levels[0][y * w + x] = band[y * w + xr] - band[y * w + xl];
        levels[1][y * w + x] = band[yd * w + x] - band[yu * w + x];
      }
  for (i = 0; i < cols * rows; i++)
    {
      Gaokun3Keypoint kp = best[i];
      if (!kp.polarity)
        continue;
      kp.angle = (float) orientation (band, w, h, (int) kp.x, (int) kp.y, orient_weight);
      if (describe (levels[0], levels[1], contact, w, h, &kp, patch_weight,
                    fs.descs[fs.n_feats]))
        fs.kps[fs.n_feats++] = kp;
    }
  fs.width = (uint16_t) w;
  fs.height = (uint16_t) h;
  free (buf);
  free (contact);
  free (best);
  return fs;

fail:
  free (buf);
  free (contact);
  free (best);
  gaokun3_feature_set_free (&fs);
  return fs;
}

void
gaokun3_feature_set_free (Gaokun3FeatureSet *fs)
{
  if (!fs)
    return;
  free (fs->kps);
  free (fs->descs);
  free (fs->ridges);
  memset (fs, 0, sizeof (*fs));
}

/* ---- Matching ---------------------------------------------------------- */

/* b's frame position of a's sample p: q = R(rot) (p - centre) + centre + t */
typedef struct {
  double rot, tx, ty;
} Pose;

typedef struct {
  int a, b;
  double rot;          /* implied by the two orientations */
  double ax, ay, bx, by;
  int support;
} Proposal;

static double
wrap (double a)
{
  while (a > M_PI)
    a -= 2 * M_PI;
  while (a <= -M_PI)
    a += 2 * M_PI;
  return a;
}

static Pose
pose_from (const Proposal *p, double cx, double cy)
{
  Pose t;
  double c = cos (p->rot), s = sin (p->rot);

  t.rot = p->rot;
  t.tx = p->bx - cx - (c * (p->ax - cx) - s * (p->ay - cy));
  t.ty = p->by - cy - (s * (p->ax - cx) + c * (p->ay - cy));
  return t;
}

/* Turning a patch by half a turn moves cell (x, y) to (3 - x, 3 - y) and
 * every gradient direction by four bins. */
static void
turn_descriptor (const float *in, float *out)
{
  int cell, bin;

  for (cell = 0; cell < 16; cell++)
    for (bin = 0; bin < 8; bin++)
      out[(15 - cell) * 8 + ((bin + 4) & 7)] = in[cell * 8 + bin];
}

static float
dot (const float *a, const float *b)
{
  float sum = 0.0f;
  int i;

  for (i = 0; i < DESC_DIM; i++)
    sum += a[i] * b[i];
  return sum;
}

/* Correlation of a's ridges with b's at pose t, over every step-th sample. */
static double
correlate (const Gaokun3FeatureSet *a, const Gaokun3FeatureSet *b,
           const Pose *t, int step, int *count)
{
  const int w = a->width, h = a->height, bw = b->width, bh = b->height;
  const double cx = (w - 1) / 2.0, cy = (h - 1) / 2.0;
  const double c = cos (t->rot), s = sin (t->rot);
  double sa = 0, sb = 0, saa = 0, sbb = 0, sab = 0, den;
  int x, y, n = 0;

  for (y = 0; y < h; y += step)
    for (x = 0; x < w; x += step)
      {
        const int va = a->ridges[y * w + x];
        double u, v, fx, fy, vb;
        const int8_t *q;
        int ix, iy;

        if (va == GAOKUN3_NO_RIDGE)
          continue;
        u = c * (x - cx) - s * (y - cy) + cx + t->tx;
        v = s * (x - cx) + c * (y - cy) + cy + t->ty;
        if (u < 0 || v < 0 || u >= bw - 1 || v >= bh - 1)
          continue;
        ix = (int) u;
        iy = (int) v;
        q = b->ridges + iy * bw + ix;
        if (q[0] == GAOKUN3_NO_RIDGE || q[1] == GAOKUN3_NO_RIDGE ||
            q[bw] == GAOKUN3_NO_RIDGE || q[bw + 1] == GAOKUN3_NO_RIDGE)
          continue;
        fx = u - ix;
        fy = v - iy;
        vb = (1 - fy) * ((1 - fx) * q[0] + fx * q[1]) +
             fy * ((1 - fx) * q[bw] + fx * q[bw + 1]);
        sa += va;
        sb += vb;
        saa += (double) va * va;
        sbb += vb * vb;
        sab += va * vb;
        n++;
      }
  *count = n;
  if (n < 16)
    return 0.0;
  den = (n * saa - sa * sa) * (n * sbb - sb * sb);
  return den > 0 ? (n * sab - sa * sb) / sqrt (den) : 0.0;
}

/* Hill-climb the correlation over rotation and offset from t on every other
 * sample, coarse steps first; the result is measured on all samples. */
static double
refine (const Gaokun3FeatureSet *a, const Gaokun3FeatureSet *b, Pose *t,
        int *count)
{
  static const struct { double rot, px; } steps[] = {
    { 2.0 * M_PI / 180.0, 1.0 },
    { 1.0 * M_PI / 180.0, 0.5 },
  };
  size_t l;
  int n;
  double best = correlate (a, b, t, 2, &n);

  for (l = 0; l < sizeof (steps) / sizeof (steps[0]); l++)
    {
      int iter, k;
      for (iter = 0; iter < 6; iter++)
        {
          Pose next = *t;
          double next_best = best;
          for (k = 0; k < 6; k++)
            {
              const double sign = (k & 1) ? -1.0 : 1.0;
              Pose trial = *t;
              double r;
              if (k < 2)
                trial.rot += sign * steps[l].rot;
              else if (k < 4)
                trial.tx += sign * steps[l].px;
              else
                trial.ty += sign * steps[l].px;
              if (fabs (trial.rot) > MAX_ROTATION)
                continue;
              r = correlate (a, b, &trial, 2, &n);
              if (r > next_best)
                {
                  next_best = r;
                  next = trial;
                }
            }
          if (next_best <= best)
            break;
          best = next_best;
          *t = next;
        }
    }
  return correlate (a, b, t, 1, count);
}

static int
cmp_support (const void *x, const void *y)
{
  const Proposal *a = x, *b = y;
  return (b->support > a->support) - (b->support < a->support);
}

/* Keypoint pairs that are each other's most similar partner, both ways. */
static int
mutual_pairs (const float *sim, int n_a, int n_b, int *pair_a, int *pair_b)
{
  int i, j, n = 0;

  for (i = 0; i < n_a; i++)
    {
      int bj = 0, bi = 0;
      for (j = 1; j < n_b; j++)
        if (sim[i * n_b + j] > sim[i * n_b + bj])
          bj = j;
      if (sim[i * n_b + bj] <= -2.0f)
        continue;
      for (j = 1; j < n_a; j++)
        if (sim[j * n_b + bj] > sim[bi * n_b + bj])
          bi = j;
      if (bi != i)
        continue;
      pair_a[n] = i;
      pair_b[n] = bj;
      n++;
    }
  return n;
}

float
gaokun3_match_score (const Gaokun3FeatureSet *a, const Gaokun3FeatureSet *b)
{
  const double cx = a ? (a->width - 1) / 2.0 : 0, cy = a ? (a->height - 1) / 2.0 : 0;
  Proposal *props = NULL;
  float *sim = NULL, (*b_turned)[DESC_DIM] = NULL;
  uint8_t *turned = NULL, *taken = NULL;
  int *pair_a = NULL, *pair_b = NULL;
  Pose chosen[PROPOSALS];
  double first[PROPOSALS];
  int n_props = 0, n_chosen = 0, n_pairs, i, j, k;
  double best = 0.0;

  if (!a || !b || a->n_feats < 1 || b->n_feats < 1 || !a->ridges ||
      !b->ridges || a->width != b->width || a->height != b->height)
    return 0.0f;

  sim = malloc (sizeof (float) * a->n_feats * b->n_feats);
  turned = malloc ((size_t) a->n_feats * b->n_feats);
  taken = calloc ((size_t) a->n_feats * b->n_feats, 1);
  props = malloc (sizeof (Proposal) * 2 * (a->n_feats + b->n_feats));
  pair_a = malloc (sizeof (int) * a->n_feats);
  pair_b = malloc (sizeof (int) * a->n_feats);
  b_turned = malloc (sizeof (*b_turned) * b->n_feats);
  if (!sim || !turned || !taken || !props || !pair_a || !pair_b || !b_turned)
    goto out;
  for (j = 0; j < b->n_feats; j++)
    turn_descriptor (b->descs[j], b_turned[j]);

  for (i = 0; i < a->n_feats; i++)
    for (j = 0; j < b->n_feats; j++)
      {
        float s0 = -2.0f, s1 = -2.0f;
        if (a->kps[i].polarity == b->kps[j].polarity)
          {
            s0 = dot (a->descs[i], b->descs[j]);
            s1 = dot (a->descs[i], b_turned[j]);
          }
        sim[i * b->n_feats + j] = s0 >= s1 ? s0 : s1;
        turned[i * b->n_feats + j] = s1 > s0;
      }
  n_pairs = mutual_pairs (sim, a->n_feats, b->n_feats, pair_a, pair_b);

  /* Proposals: the two most similar partners of every keypoint, either way
   * round, within the rotation a press can have. */
  for (k = 0; k < 2; k++)
    {
      const int n_from = k ? b->n_feats : a->n_feats;
      const int n_to = k ? a->n_feats : b->n_feats;
      for (i = 0; i < n_from; i++)
        {
          int top[2] = { -1, -1 }, r;
          for (j = 0; j < n_to; j++)
            {
              const int ia = k ? j : i, ib = k ? i : j;
              const float s = sim[ia * b->n_feats + ib];
              if (s <= -2.0f)
                continue;
              if (top[0] < 0 || s > sim[(k ? top[0] : i) * b->n_feats + (k ? i : top[0])])
                {
                  top[1] = top[0];
                  top[0] = j;
                }
              else if (top[1] < 0 || s > sim[(k ? top[1] : i) * b->n_feats + (k ? i : top[1])])
                top[1] = j;
            }
          for (r = 0; r < 2; r++)
            {
              const int ia = k ? top[r] : i, ib = k ? i : top[r];
              const Gaokun3Keypoint *ka, *kb;
              Proposal *p;
              if (top[r] < 0 || taken[ia * b->n_feats + ib])
                continue;
              taken[ia * b->n_feats + ib] = 1;
              ka = &a->kps[ia];
              kb = &b->kps[ib];
              p = &props[n_props];
              p->rot = wrap ((double) kb->angle - ka->angle +
                             (turned[ia * b->n_feats + ib] ? M_PI : 0.0));
              if (fabs (p->rot) > MAX_ROTATION)
                continue;
              p->a = ia;
              p->b = ib;
              p->ax = ka->x;
              p->ay = ka->y;
              p->bx = kb->x;
              p->by = kb->y;
              p->support = 0;
              n_props++;
            }
        }
    }

  /* Support: the other proposals that land within AGREE_PX of their partner
   * under this proposal's pose. Proposals are tried best supported first. */
  for (i = 0; i < n_props; i++)
    {
      const Pose t = pose_from (&props[i], cx, cy);
      const double c = cos (t.rot), s = sin (t.rot);
      for (j = 0; j < n_props; j++)
        {
          double u, v;
          if (j == i || fabs (wrap (props[j].rot - t.rot)) > AGREE_RAD)
            continue;
          u = c * (props[j].ax - cx) - s * (props[j].ay - cy) + cx + t.tx;
          v = s * (props[j].ax - cx) + c * (props[j].ay - cy) + cy + t.ty;
          u -= props[j].bx;
          v -= props[j].by;
          if (u * u + v * v <= AGREE_PX * AGREE_PX)
            props[i].support++;
        }
    }
  qsort (props, (size_t) n_props, sizeof (Proposal), cmp_support);

  /* Correlate each distinct pose once, refine the most promising ones and
   * keep the strongest evidence (see the top of this file). */
  for (i = 0; i < n_props && n_chosen < PROPOSALS; i++)
    {
      const Pose t = pose_from (&props[i], cx, cy);
      int m, n;
      for (m = 0; m < n_chosen; m++)
        if (fabs (wrap (chosen[m].rot - t.rot)) < AGREE_RAD / 2 &&
            hypot (chosen[m].tx - t.tx, chosen[m].ty - t.ty) < 2 * AGREE_PX)
          break;
      if (m < n_chosen)
        continue;
      chosen[n_chosen] = t;
      first[n_chosen++] = correlate (a, b, &t, 2, &n);
    }
  for (k = 0; k < REFINED && k < n_chosen; k++)
    {
      Pose t;
      double r, z = 0.0, c, s, evidence;
      int n, m, top = 0, agree = 0;
      for (m = 1; m < n_chosen; m++)
        if (first[m] > first[top])
          top = m;
      t = chosen[top];
      first[top] = -2.0;
      r = refine (a, b, &t, &n);
      if (n >= MIN_OVERLAP && r > 0)
        z = atanh (r > 0.999 ? 0.999 : r) * sqrt (n / CORR_AREA);
      c = cos (t.rot);
      s = sin (t.rot);
      for (m = 0; m < n_pairs; m++)
        {
          const Gaokun3Keypoint *ka = &a->kps[pair_a[m]], *kb = &b->kps[pair_b[m]];
          const double u = c * (ka->x - cx) - s * (ka->y - cy) + cx + t.tx;
          const double v = s * (ka->x - cx) + c * (ka->y - cy) + cy + t.ty;
          agree += hypot (u - kb->x, v - kb->y) <= AGREE_PX;
        }
      evidence = z + KEYPOINT_WEIGHT * agree;
      if (evidence > best)
        best = evidence;
    }

out:
  free (sim);
  free (turned);
  free (taken);
  free (props);
  free (pair_a);
  free (pair_b);
  free (b_turned);
  return (float) (best > 100.0 ? 100.0 : best);
}
