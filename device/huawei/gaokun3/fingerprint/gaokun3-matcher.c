/* SPDX-License-Identifier: LGPL-2.1-or-later
 * Adapted from libfprint-fte7001's fte7001-matcher.c for Gaokun3's
 * sensors; the frame carries its geometry. This is a host-side matcher, not
 * the Windows matcher.
 */

#include "gaokun3-matcher.h"

#include <assert.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define GAOKUN3_M_PATCH 16
#define GAOKUN3_M_SIGMA_COUNT 4
#define GAOKUN3_M_DESC_DIM 128
#define GAOKUN3_M_LOWE_RATIO 0.9
#define GAOKUN3_M_RANSAC_ITERS 200
#define GAOKUN3_M_RANSAC_TOL 2.5
#define GAOKUN3_M_MIN_FEATS 6
#define GAOKUN3_Q_MIN_COVERAGE 0.70
#define GAOKUN3_Q_MIN_MEAN_TC 9.0

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* ===== MT19937 + CPython random 兼容层 ===== */

typedef struct {
  uint32_t mt[624];
  int idx;
} Mt19937;

static void
mt_init (Mt19937 *r, uint32_t seed)
{
  /* CPython int seed → init_by_array([seed])（_randommodule.c
   * random_seed：int 走 init_by_array，单元素 key）。
   * 逐行对照 CPython Modules/_randommodule.c init_by_array。 */
  uint32_t key[1] = { seed };
  int key_length = 1;
  int i, j, k;
  uint32_t *mt = r->mt;

  mt[0] = 19650218u;
  for (i = 1; i < 624; i++)
    mt[i] = 1812433253u * (mt[i - 1] ^ (mt[i - 1] >> 30)) + (uint32_t) i;

  i = 1;
  j = 0;
  k = (624 > key_length ? 624 : key_length);
  for (; k; k--)
    {
      mt[i] = (mt[i] ^ ((mt[i - 1] ^ (mt[i - 1] >> 30)) * 1664525u)) + key[j];
      mt[i] &= 0xffffffffu;
      i++;
      j++;
      if (i >= 624)
        {
          mt[0] = mt[623];
          i = 1;
        }
      if (j >= key_length)
        j = 0;
    }
  for (k = 623; k; k--)
    {
      mt[i] = (mt[i] ^ ((mt[i - 1] ^ (mt[i - 1] >> 30)) * 1566083941u)) - (uint32_t) i;
      mt[i] &= 0xffffffffu;
      i++;
      if (i >= 624)
        {
          mt[0] = mt[623];
          i = 1;
        }
    }
  mt[0] = 0x80000000u;
  r->idx = 624;
}

static uint32_t
mt_genrand (Mt19937 *r)
{
  uint32_t y;
  int i;

  if (r->idx >= 624)
    {
      for (i = 0; i < 624; i++)
        {
          y = (r->mt[i] & 0x80000000u) | (r->mt[(i + 1) % 624] & 0x7fffffffu);
          r->mt[i] = r->mt[(i + 397) % 624] ^ (y >> 1) ^ ((y & 1) ? 0x9908b0dfu : 0);
        }
      r->idx = 0;
    }
  y = r->mt[r->idx++];
  y ^= (y >> 11);
  y ^= (y << 7) & 0x9d2c5680u;
  y ^= (y << 15) & 0xefc60000u;
  y ^= (y >> 18);
  return y;
}

/* random.Random.randrange(n) → _randbelow_with_getrandbits(n)：
 *   k = n.bit_length(); r = getrandbits(k); while r >= n: 重取。
 * CPython 3.14 实测源码（inspect.getsource）逐行核对。 */
static uint32_t
mt_randbelow (Mt19937 *r, uint32_t n)
{
  int k = 0;
  uint32_t v;

  assert (n > 0);
  /* n.bit_length()：最高有效位位置（n=1 → 1，n=4 → 3，n=5 → 3） */
  while (n >> k)
    k++;
  do
    {
      /* getrandbits(k) for 1<=k<=32 = genrand() >> (32-k)，k=32 时全宽 */
      v = (k >= 32) ? mt_genrand (r) : (mt_genrand (r) >> (32 - k));
    }
  while (v >= n);
  return v;
}

/* random.Random(42).sample(population, 2) — CPython 3.14 池法
 * （n <= setsize=21 时走池法；n>21 且 k=2 走 set 法）：
 *   池法: pool = list(pop); j = randbelow(n-i); result[i]=pool[j];
 *         pool[j] = pool[n-i-1]        ← swap-with-last，不是 pop！
 *   集合法: j = randbelow(n); while j in selected: j = randbelow(n)
 * CPython 3.14 实测源码逐行核对。返回 [0,n) 内两个不同索引。 */
static void
mt_sample2 (Mt19937 *r, uint32_t n, uint32_t out[2])
{
  uint32_t setsize = 21;  /* k=2 ≤ 5 → 不加 4**ceil 项 */

  assert (n >= 2);

  if (n <= setsize)
    {
      /* 池法（swap-with-last） */
      uint32_t pool[64];
      uint32_t i;
      assert (n <= 64);
      for (i = 0; i < n; i++)
        pool[i] = i;
      for (i = 0; i < 2; i++)
        {
          uint32_t j = mt_randbelow (r, n - i);
          out[i] = pool[j];
          pool[j] = pool[n - i - 1];
        }
    }
  else
    {
      /* 集合法：拒绝采样直到不重复 */
      uint32_t j0 = mt_randbelow (r, n);
      uint32_t j1;
      do
        {
          j1 = mt_randbelow (r, n);
        }
      while (j1 == j0);
      out[0] = j0;
      out[1] = j1;
    }
}

/* ===== 高斯模糊（原型 gauss_blur）===== */

static const double SIGMAS[GAOKUN3_M_SIGMA_COUNT] = { 2.0, 2.8, 4.0, 5.7 };

/* px and out are row major: stride w, height h. */
static void
gaokun3_gauss_blur (const float *px, int w, int h, double sigma,
                    float *out /* w*h */)
{
  int radius = (int) (2.5 * sigma + 0.5);
  int ksize = 2 * radius + 1;
  double g[64];
  double s = 0.0;
  float *tmp;
  int x, y, k;

  if (radius < 1)
    radius = 1;
  g[0] = 1.0;  /* 占位，下面重算 */

  for (k = -radius; k <= radius; k++)
    {
      g[k + radius] = exp (-(double) (k * k) / (2.0 * sigma * sigma));
      s += g[k + radius];
    }
  for (k = 0; k < ksize; k++)
    g[k] /= s;

  tmp = malloc (sizeof (float) * (size_t) w * (size_t) h);
  assert (tmp);

  /* horizontal */
  for (y = 0; y < h; y++)
    {
      int row = y * w;
      for (x = 0; x < w; x++)
        {
          double acc = 0.0;
          for (k = 0; k < ksize; k++)
            {
              int xx = x + k - radius;
              if (xx >= 0 && xx < w)
                acc += g[k] * (double) px[row + xx];
            }
          tmp[row + x] = (float) acc;
        }
    }

  /* vertical */
  for (y = 0; y < h; y++)
    for (x = 0; x < w; x++)
      {
        double acc = 0.0;
        for (k = 0; k < ksize; k++)
          {
            int yy = y + k - radius;
            if (yy >= 0 && yy < h)
              acc += g[k] * (double) tmp[yy * w + x];
          }
        out[y * w + x] = (float) acc;
      }

  free (tmp);
}

/* ===== Frame geometry ===== */

bool
gaokun3_frame_geometry_valid (const Gaokun3Frame *frame)
{
  return frame && frame->width >= GAOKUN3_MIN_SIDE &&
         frame->height >= GAOKUN3_MIN_SIDE &&
         frame->width <= GAOKUN3_MAX_WIDTH &&
         frame->height <= GAOKUN3_MAX_HEIGHT;
}

/* ===== 质量闸门（原型 quality_gate）===== */

bool
gaokun3_quality_check (const Gaokun3Frame *frame,
                       float *coverage, float *mean_tc)
{
  /* Evaluate local texture across the full sensor frame. */
  const uint8_t *px = frame->pixels;
  int w = frame->width, h = frame->height;
  int y, x, i;
  double sum_tc = 0.0;
  long n_tc = 0, n_cov = 0;

  if (coverage)
    *coverage = 0;
  if (mean_tc)
    *mean_tc = 0;
  if (!gaokun3_frame_geometry_valid (frame))
    return false;
  for (y = 1; y < h - 1; y++)
    for (x = 1; x < w - 1; x++)
      {
        double vals[9], m = 0.0, ss = 0.0, tc;
        for (i = 0; i < 9; i++)
          {
            int dy = (i / 3) - 1, dx = (i % 3) - 1;
            vals[i] = (double) px[(y + dy) * w + x + dx];
            m += vals[i];
          }
        m /= 9.0;
        for (i = 0; i < 9; i++)
          {
            double d = vals[i] - m;
            ss += d * d;
          }
        tc = sqrt (ss / 9.0);
        if (tc > 4.0)
          n_cov++;
        sum_tc += tc;
        n_tc++;
      }

  {
    float cov = (float) ((double) n_cov / (double) n_tc);
    float mtc = (float) (sum_tc / (double) n_tc);

    if (coverage)
      *coverage = cov;
    if (mean_tc)
      *mean_tc = mtc;
    return cov >= GAOKUN3_Q_MIN_COVERAGE && mtc >= GAOKUN3_Q_MIN_MEAN_TC;
  }
}

/* ===== 关键点检测（原型 detect_keypoints）===== */

/* 8 x 8 spatial buckets, at most two keypoints per bucket: 25 x 5 buckets
 * for the largest (40 x 196) frame. */
#define GAOKUN3_M_GRID 8
#define GAOKUN3_M_GRID_ROWS \
  ((GAOKUN3_MAX_HEIGHT + GAOKUN3_M_GRID - 1) / GAOKUN3_M_GRID)
#define GAOKUN3_M_GRID_COLS \
  ((GAOKUN3_MAX_WIDTH + GAOKUN3_M_GRID - 1) / GAOKUN3_M_GRID)
#define GAOKUN3_M_MAX_KPS (GAOKUN3_M_GRID_ROWS * GAOKUN3_M_GRID_COLS * 2)

static int
gaokun3_detect_keypoints (const float *px, int w, int h, Gaokun3Keypoint *out)
{
  static const float THRESH = 3.0f;
  static const int GRID = GAOKUN3_M_GRID;
  float layers[GAOKUN3_M_SIGMA_COUNT][GAOKUN3_MAX_PIXELS];
  float dogs[3][GAOKUN3_MAX_PIXELS];
  Gaokun3Keypoint pts[1024];
  int n_pts = 0;
  int li, x, y;

  for (li = 0; li < GAOKUN3_M_SIGMA_COUNT; li++)
    gaokun3_gauss_blur (px, w, h, SIGMAS[li], layers[li]);
  for (li = 0; li < 3; li++)
    for (y = 0; y < w * h; y++)
      dogs[li][y] = layers[li + 1][y] - layers[li][y];

  for (li = 0; li < 3; li++)
    {
      const float *dog = dogs[li];
      const float *dprev = dogs[li > 0 ? li - 1 : 0];
      const float *dnext = dogs[li < 2 ? li + 1 : 2];
      int same = (li == 0) || (li == 2);

      for (y = 2; y < h - 2; y++)
        for (x = 2; x < w - 2; x++)
          {
            float v = dog[y * w + x];
            int is_max = 1, is_min = 1;
            int dy, dx, stop = 0;

            if (fabsf (v) < THRESH)
              continue;
            for (dy = -1; dy <= 1 && (is_max || is_min); dy++)
              for (dx = -1; dx <= 1; dx++)
                {
                  float nv;
                  if (dy == 0 && dx == 0)
                    continue;
                  nv = dog[(y + dy) * w + x + dx];
                  if (nv >= v)
                    is_max = 0;
                  if (nv <= v)
                    is_min = 0;
                  if (!is_max && !is_min)
                    break;
                }
            if (!(is_max || is_min))
              continue;
            if (!same)
              {
                int l;
                for (l = 0; l < 2; l++)
                  {
                    float nv = l == 0 ? dprev[y * w + x]
                                       : dnext[y * w + x];
                    if (is_max && nv >= v)
                      {
                        is_max = 0;
                        stop = 1;
                      }
                    if (is_min && nv <= v)
                      {
                        is_min = 0;
                        stop = 1;
                      }
                    if (stop)
                      break;
                  }
              }
            if (!(is_max || is_min))
              continue;
            if (n_pts < 1024)
              {
                pts[n_pts].x = x;
                pts[n_pts].y = y;
                pts[n_pts].v = v;
                pts[n_pts].layer = li;
                n_pts++;
              }
          }
    }

  /* 桶化：每 8×8 桶取 |v| 最强的 2 个（原型 L101-110）。桶表用定长数组；
   * 每桶按插入序维护 top-2（|v| 降序），等价于原型的 sort+take-2。 */
  {
    Gaokun3Keypoint buckets[GAOKUN3_M_GRID_ROWS][GAOKUN3_M_GRID_COLS][2];
    int bucket_n[GAOKUN3_M_GRID_ROWS][GAOKUN3_M_GRID_COLS];
    int rows = (h + GRID - 1) / GRID, cols = (w + GRID - 1) / GRID;
    int kept = 0;
    int bx, by, idx;

    memset (bucket_n, 0, sizeof (bucket_n));

    for (idx = 0; idx < n_pts; idx++)
      {
        Gaokun3Keypoint *b;
        int *bn;
        float av = fabsf (pts[idx].v);

        bx = pts[idx].x / GRID;
        by = pts[idx].y / GRID;
        b = buckets[by][bx];
        bn = &bucket_n[by][bx];

        if (*bn < 2)
          {
            b[(*bn)++] = pts[idx];
          }
        else
          {
            /* 找到 |v| 最小的已存点，若新点更强则替换（保序插入：
             * 原型是 sort 后取前 2，并列时保留先出现的 —— 与此处
             * “弱者被替换”在 |v| 互异时等价）。 */
            int weak = fabsf (b[0].v) <= fabsf (b[1].v) ? 0 : 1;
            if (av > fabsf (b[weak].v))
              b[weak] = pts[idx];
          }
      }

    for (by = 0; by < rows; by++)
      for (bx = 0; bx < cols; bx++)
        for (idx = 0; idx < bucket_n[by][bx]; idx++)
          {
            Gaokun3Keypoint k = buckets[by][bx][idx];
            int pos;
            /* (y,x) 升序插入（原型 kept.sort(key=(p[1], p[0]))） */
            for (pos = kept; pos > 0; pos--)
              {
                Gaokun3Keypoint q = out[pos - 1];
                if (q.y > k.y || (q.y == k.y && q.x > k.x))
                  out[pos] = q;
                else
                  break;
              }
            out[pos] = k;
            kept++;
          }

    return kept;
  }
}

/* ===== Orientation-aligned spatial/gradient descriptors ===== */

/* Caller verifies a one-pixel gradient margin plus the bilinear footprint. */
static double
gaokun3_sample (const float *px, int w, double x, double y)
{
  int ix = (int) floor (x), iy = (int) floor (y);
  double fx = x - ix, fy = y - iy;
  const float *p = px + iy * w + ix;

  return (1 - fy) * ((1 - fx) * p[0] + fx * p[1]) +
         fy * ((1 - fx) * p[w] + fx * p[w + 1]);
}

static double
gaokun3_patch_orient (const float *px, int w, const Gaokun3Keypoint *kp)
{
  double hist[36] = { 0 }, smooth[36];
  int dx, dy, b, pass, best = 0;

  /* Interpolate and smooth a weighted orientation histogram. Quantizing the
   * dominant direction to one of eight bins made small rotations unstable. */
  for (dy = -7; dy <= 7; dy++)
    for (dx = -7; dx <= 7; dx++)
      {
        int x = kp->x + dx, y = kp->y + dy;
        double gx = px[y * w + x + 1] - px[y * w + x - 1];
        double gy = px[(y + 1) * w + x] - px[(y - 1) * w + x];
        double angle = (atan2 (gy, gx) + M_PI) * 36 / (2 * M_PI);
        int bin = (int) floor (angle);
        double fraction = angle - bin;
        double weight = hypot (gx, gy) * exp (-(dx * dx + dy * dy) / 50.0);

        hist[bin % 36] += weight * (1 - fraction);
        hist[(bin + 1) % 36] += weight * fraction;
      }
  for (pass = 0; pass < 4; pass++)
    {
      for (b = 0; b < 36; b++)
        smooth[b] = (hist[(b + 35) % 36] + 2 * hist[b] + hist[(b + 1) % 36]) / 4;
      memcpy (hist, smooth, sizeof (hist));
    }
  for (b = 1; b < 36; b++)
    if (hist[b] > hist[best])
      best = b;
  {
    double left = hist[(best + 35) % 36], center = hist[best];
    double right = hist[(best + 1) % 36];
    double denominator = left - 2 * center + right;
    double offset = fabs (denominator) > 1e-12 ?
                    0.5 * (left - right) / denominator : 0;
    return (best + offset) * 2 * M_PI / 36 - M_PI;
  }
}

static int
gaokun3_describe (const float *px, int w, int h, const Gaokun3Keypoint *kp,
                  float desc[GAOKUN3_M_DESC_DIM])
{
  int py, pxx, i;
  double theta, c, s, norm = 0;

  if (kp->x < 9 || kp->y < 9 || kp->x >= w - 9 || kp->y >= h - 9)
    return 0;
  theta = gaokun3_patch_orient (px, w, kp);
  c = cos (theta);
  s = sin (theta);
  memset (desc, 0, sizeof (float) * GAOKUN3_M_DESC_DIM);

  for (py = 0; py < GAOKUN3_M_PATCH; py++)
    for (pxx = 0; pxx < GAOKUN3_M_PATCH; pxx++)
      {
        double dx = pxx - 7.5, dy = py - 7.5;
        double x = kp->x + c * dx - s * dy;
        double y = kp->y + s * dx + c * dy;
        double gx, gy, angle, fraction, cx, cy, fx, fy, weight;
        int bin, ix, iy, xx, yy;

        /* Rotate the spatial sampling grid along with the gradient direction.
         * Reject unsupported border patches rather than inventing padding. */
        if (x < 1 || x >= w - 2 || y < 1 || y >= h - 2)
          return 0;
        gx = gaokun3_sample (px, w, x + 1, y) - gaokun3_sample (px, w, x - 1, y);
        gy = gaokun3_sample (px, w, x, y + 1) - gaokun3_sample (px, w, x, y - 1);
        angle = fmod ((atan2 (gy, gx) - theta) * 8 / (2 * M_PI) + 16, 8);
        bin = (int) floor (angle);
        fraction = angle - bin;
        cx = (pxx + 0.5) / 4 - 0.5;
        cy = (py + 0.5) / 4 - 0.5;
        ix = (int) floor (cx);
        iy = (int) floor (cy);
        fx = cx - ix;
        fy = cy - iy;
        weight = hypot (gx, gy) * exp (-(dx * dx + dy * dy) / 128);
        for (yy = 0; yy < 2; yy++)
          for (xx = 0; xx < 2; xx++)
            {
              int bx = ix + xx, by = iy + yy;
              double w;
              if (bx < 0 || bx >= 4 || by < 0 || by >= 4)
                continue;
              w = weight * (xx ? fx : 1 - fx) * (yy ? fy : 1 - fy);
              desc[(by * 4 + bx) * 8 + bin] += (float) (w * (1 - fraction));
              desc[(by * 4 + bx) * 8 + (bin + 1) % 8] += (float) (w * fraction);
            }
      }
  for (i = 0; i < GAOKUN3_M_DESC_DIM; i++)
    norm += (double) desc[i] * desc[i];
  norm = sqrt (norm);
  if (norm < 1e-6)
    return 0;
  for (i = 0; i < GAOKUN3_M_DESC_DIM; i++)
    desc[i] = (float) fmin (desc[i] / norm, 0.2);
  norm = 0;
  for (i = 0; i < GAOKUN3_M_DESC_DIM; i++)
    norm += (double) desc[i] * desc[i];
  norm = sqrt (norm);
  if (norm < 1e-6)
    return 0;
  for (i = 0; i < GAOKUN3_M_DESC_DIM; i++)
    desc[i] = (float) (desc[i] / norm);
  return 1;
}

/* ===== 特征提取入口 ===== */

Gaokun3FeatureSet
gaokun3_feature_extract (const Gaokun3Frame *frame)
{
  Gaokun3FeatureSet fs = { NULL, NULL, 0 };
  float px[GAOKUN3_MAX_PIXELS];
  Gaokun3Keypoint kps[GAOKUN3_M_MAX_KPS];
  int n_kps, i, w, h;

  if (!gaokun3_frame_geometry_valid (frame))
    return fs;
  w = frame->width;
  h = frame->height;
  /* Use the complete decoded sensor frame. */
  for (i = 0; i < w * h; i++)
    px[i] = (float) frame->pixels[i];

  n_kps = gaokun3_detect_keypoints (px, w, h, kps);
  if (n_kps == 0)
    return fs;

  fs.kps = malloc (sizeof (Gaokun3Keypoint) * n_kps);
  fs.descs = malloc (sizeof (float) * GAOKUN3_M_DESC_DIM * n_kps);
  if (!fs.kps || !fs.descs)
    {
      free (fs.kps);
      free (fs.descs);
      fs.kps = NULL;
      fs.descs = NULL;
      return fs;
    }

  for (i = 0; i < n_kps; i++)
    {
      float d[GAOKUN3_M_DESC_DIM];
      if (gaokun3_describe (px, w, h, &kps[i], d))
        {
          fs.kps[fs.n_feats] = kps[i];
          memcpy (fs.descs[fs.n_feats], d, sizeof (d));
          fs.n_feats++;
        }
    }

  if (fs.n_feats == 0)
    {
      gaokun3_feature_set_free (&fs);
    }
  return fs;
}

void
gaokun3_feature_set_free (Gaokun3FeatureSet *fs)
{
  if (!fs)
    return;
  free (fs->kps);
  free (fs->descs);
  fs->kps = NULL;
  fs->descs = NULL;
  fs->n_feats = 0;
}

/* ===== 匹配（原型 match_feats，Lowe ratio 0.9）===== */

/* 上限：n_feats ≤ 128（桶化 8×8×2），matches ≤ min(nA,nB) ≤ 128 */
#define GAOKUN3_M_MAX_MATCHES 128

typedef struct {
  int i, j;
  float d1;
} Gaokun3Match;

static double
gaokun3_desc_dist (const float a[GAOKUN3_M_DESC_DIM],
                   const float b[GAOKUN3_M_DESC_DIM])
{
  double s = 0.0;
  int i;
  for (i = 0; i < GAOKUN3_M_DESC_DIM; i++)
    {
      double d = (double) a[i] - (double) b[i];
      s += d * d;
    }
  return sqrt (s);
}

static int
gaokun3_match_feats (const Gaokun3FeatureSet *a, const Gaokun3FeatureSet *b,
                     Gaokun3Match *out, int max_out)
{
  int n = 0, i;

  for (i = 0; i < a->n_feats; i++)
    {
      /* 每对 i 找 j 的最近+严格次近（原型 dists.sort() 后取前 2 等价：
       * Python sort 对 (dist, j) 元组排序，并列时 j 小者先 —— 双最值
       * 扫描在 dist 互异时等价，且 j 从小到大扫描天然满足并列取小 j）。 */
      double best = 1e30, second = 1e30;
      int bestj = -1, jj;

      for (jj = 0; jj < b->n_feats; jj++)
        {
          double d = gaokun3_desc_dist (a->descs[i], b->descs[jj]);
          if (d < best)
            {
              second = best;
              best = d;
              bestj = jj;
            }
          else if (d < second)
            second = d;
        }

      if (b->n_feats >= 2 && second > 0.0 && best / second < GAOKUN3_M_LOWE_RATIO)
        {
          if (n < max_out)
            {
              out[n].i = i;
              out[n].j = bestj;
              out[n].d1 = (float) best;
              n++;
            }
        }
    }
  return n;
}

/* ===== 相似变换 RANSAC（原型 ransac_similarity）===== */

static int
gaokun3_ransac_similarity (const Gaokun3FeatureSet *fa,
                           const Gaokun3FeatureSet *fb,
                           const Gaokun3Match *matches, int n_matches)
{
  Mt19937 rng;
  int best = 0;
  int it;

  if (n_matches < 4)
    return 0;

  mt_init (&rng, 42);

  for (it = 0; it < GAOKUN3_M_RANSAC_ITERS; it++)
    {
      uint32_t pick[2];
      int i1, j1, i2, j2;
      double ax1, ay1, ax2, ay2, bx1, by1, bx2, by2;
      double dax, day, dbx, dby, na, nb, ang, c, s;
      int inl = 0, m;

      /* 原型：rng.sample(matches, 2)（不放回；mt_sample2 复现 CPython 3.14） */
      mt_sample2 (&rng, (uint32_t) n_matches, pick);

      i1 = matches[pick[0]].i;
      j1 = matches[pick[0]].j;
      i2 = matches[pick[1]].i;
      j2 = matches[pick[1]].j;

      ax1 = fa->kps[i1].x; ay1 = fa->kps[i1].y;
      ax2 = fa->kps[i2].x; ay2 = fa->kps[i2].y;
      bx1 = fb->kps[j1].x; by1 = fb->kps[j1].y;
      bx2 = fb->kps[j2].x; by2 = fb->kps[j2].y;

      dax = ax2 - ax1; day = ay2 - ay1;
      dbx = bx2 - bx1; dby = by2 - by1;
      na = hypot (dax, day);
      nb = hypot (dbx, dby);
      if (na < 1e-6 || nb < 1e-6)
        continue;
      if (nb / na > 1.3 || nb / na < 0.77)
        continue;
      ang = atan2 (dby, dbx) - atan2 (day, dax);
      /* Fit the similarity transform defined by both sampled pairs. Merely
       * checking nb/na while applying a unit-scale rotation rejects otherwise
       * consistent points as their distance from the anchor grows. */
      c = (nb / na) * cos (ang);
      s = (nb / na) * sin (ang);

      for (m = 0; m < n_matches; m++)
        {
          double x = fa->kps[matches[m].i].x - ax1;
          double y = fa->kps[matches[m].i].y - ay1;
          double rx = c * x - s * y + bx1;
          double ry = s * x + c * y + by1;
          if (fabs (rx - fb->kps[matches[m].j].x) <= GAOKUN3_M_RANSAC_TOL &&
              fabs (ry - fb->kps[matches[m].j].y) <= GAOKUN3_M_RANSAC_TOL)
            inl++;
        }
      if (inl > best)
        best = inl;
    }
  return best;
}

/* ===== 归一化打分（原型 norm_score）===== */

float
gaokun3_match_score (const Gaokun3FeatureSet *a, const Gaokun3FeatureSet *b)
{
  Gaokun3Match matches[256];
  int n_matches;

  if (a->n_feats < GAOKUN3_M_MIN_FEATS || b->n_feats < GAOKUN3_M_MIN_FEATS)
    return 0.0f;

  n_matches = gaokun3_match_feats (a, b, matches, 256);
  if (n_matches < 4)
    return 0.0f;

  {
    int inl = gaokun3_ransac_similarity (a, b, matches, n_matches);
    return (float) (100.0 * (double) inl /
                    (double) (a->n_feats < b->n_feats ?
                              a->n_feats : b->n_feats));
  }
}
