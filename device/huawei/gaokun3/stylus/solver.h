/* SPDX-License-Identifier: GPL-2.0 AND MIT */
/*
 * HPP3 coordinate, pressure and tilt steps ported from EGoTouchRev-rebuild
 * (https://github.com/awarson2233/EGoTouchRev-rebuild),
 * EGoTouchService/Solvers/StylusSolver: hpp3/CoordinateSolver.hpp,
 * hpp3/GridFeatureExtractor.hpp, hpp3/PressureSolver.hpp, hpp3/TiltProcess.hpp
 * and shared/CoorReviseProcess.hpp.  The tilt constants are re-measured on
 * this unit.  Shared by gk3pend and the host tools (test_*.c, tilt_cal.c).
 *
 * The ported code is under the MIT license of EGoTouchRev-rebuild:
 *
 * Copyright (c) 2025 Detach2233
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */
#define UNIT		1024	/* EGoTouchRev Asa::kCoorUnit: 1/1024 cell */
#define REGION_FLOOR	100	/* GridFeatureExtractor m_peakRegionFloor */

/* EGoTouchRev CoordinateSolver::TriangleAlgUsing3Point: offset in [0, UNIT) */
static int tri3(int l, int c, int r)
{
	int mn, den;

	if (r < l) {
		mn = c > r ? r : c - 1;
		den = c - mn;
		return UNIT / 2 - (((l - mn) * UNIT) / den) / 2;
	}
	mn = c > l ? l : c - 1;
	den = c - mn;
	return (((r - mn) * UNIT) / den) / 2 + UNIT / 2;
}

/* CoordinateSolver::ApplyPitchCompensation with the factory coefficients */
static int pitch_comp(int coor, const double *k)
{
	int rem = ((coor % UNIT) + UNIT) % UNIT;
	int x = rem < 0x201 ? 0x200 - rem : rem - 0x200;
	double dx = x;
	int comp = (int)(k[0] + k[1] * dx + k[2] * dx * dx + k[3] * dx * dx * dx);

	return rem >= 0x201 ? coor - comp : coor + comp;
}

static const double pitch_cols[4] = { 0.0, -1.7109151490662926, 0.005959771652221362, -5.113555667385272e-06 };
static const double pitch_rows[4] = { 0.0, -1.4495726495726495, 0.004745726495726496, -3.7393162393162394e-06 };

#define SENSOR_COLS	60
#define SENSOR_ROWS	40

/*
 * Column pitch.  The sensor columns are not evenly spaced.  Measured on a
 * BOE panel (boe,ppc357db1-4) by tapping targets at known positions: the 10
 * columns at each end are 33/32 of a nominal cell wide and the 40 in the
 * middle 63/64.  Taken as even, the pen read up to 1.3-1.5 mm off towards
 * the nearer end around columns 10 and 50, the same in every target row;
 * with this map a second session read 0.11 mm rms, about the tapping
 * precision.  The rows are even (end sections fit no better).  The factory
 * table (CoordinateSolver::ApplyPitchMap, from a CSOT-panel unit) has the
 * same corners with the opposite sign and half the size, 63/64 at the ends
 * and 129/128 in the middle: on the BOE panel it would triple the error.
 * So gk3pend applies this map on BOE only; other panels stay even until
 * measured.  col_pitch_map() takes a global column coordinate (UNIT per
 * nominal cell, sensor order) to the physical position in the same units,
 * linearly beyond the sensor.
 */
#define PITCH_END	66	/* width of the 10 end columns, 1/64 cell */
#define PITCH_MID	63	/* width of the 40 middle columns */

static int col_bound(int k)	/* left edge of column k, 1/64 cell */
{
	if (k <= 10)
		return k * PITCH_END;
	if (k <= 50)
		return 10 * PITCH_END + (k - 10) * PITCH_MID;
	return 10 * PITCH_END + 40 * PITCH_MID + (k - 50) * PITCH_END;
}

static int col_pitch_map(int col)
{
	int k = col < 0 ? 0 : col >= SENSOR_COLS * UNIT ? SENSOR_COLS - 1 : col / UNIT;
	int lo = col_bound(k);

	return (lo * UNIT + (col - k * UNIT) * (col_bound(k + 1) - lo)) / 64;
}

/*
 * Position inside an outermost sensor cell, from inner / peak: 0 = sensor
 * border, UNIT = inner boundary.  There is no outer neighbour, and the zero
 * outside the sensor capped the triangle position at about the cell centre,
 * leaving a ~2 mm strip along every edge the pen could not reach.  The
 * factory's EdgeCompensating/TriangleAlgEdge (virtual outer neighbour, ratio
 * 93, sum thresholds 3000-4000) flips between the border and mid-cell on the
 * weak corner signals here (projections of a few hundred), so instead:
 * interior frames give the triangle position against inner/peak, which is
 * tight for ratios >= 0.2 (0.55 + 0.55 * (r - 0.2), within +-0.03 cell) and
 * says little below that (the outer half of the cell); there it is pushed
 * linearly out to the border at r = 0.05, where the edge-hugging strokes sit.
 * Noisy, so gk3pend low-passes positions within a cell of an edge.
 */
static int edge_pos(int peak, int inner)
{
	int q = peak > 0 ? inner * 1000 / peak : 0;	/* ratio x1000 */

	if (q >= 1000)
		return UNIT - 1;
	if (q >= 200)
		return (550 + (q - 200) * 55 / 100) * UNIT / 1000;
	if (q <= 50)
		return 0;
	return 550 * (q - 50) / 150 * UNIT / 1000;
}

/* SolveByTriangle for one axis; lo/hi: window index of the first/last sensor cell, or -1 */
static int solve_axis(const int *p, int pk, int lo, int hi)
{
	if (pk == lo && pk + 1 <= 8)
		return pk * UNIT + edge_pos(p[pk], p[pk + 1]);
	if (pk == hi && pk - 1 >= 0)
		return (pk + 1) * UNIT - edge_pos(p[pk], p[pk - 1]);
	return pk * UNIT + tri3(pk ? p[pk - 1] : 0, p[pk], pk < 8 ? p[pk + 1] : 0);
}

static int edge_index(int idx)
{
	return idx >= 0 && idx <= 8 ? idx : -1;
}

/*
 * Local position of the TX1 peak inside the 9x9 window, in 1/UNIT cells
 * (window cell k spans k*UNIT .. (k+1)*UNIT).  g is the window, row-major;
 * ar/ac its anchor (the sensor cell under window cell 4).  Near an edge the
 * window reaches past the sensor (those cells read 0, or a little crosstalk
 * just past the last column): they are left out of the region and the
 * projections, as in GridFeatureExtractor/CoordinateSolver.
 */
#define GK3_PEAK_REGION_API 1

/* Returns 1 when the opt-in strong region is used, 0 for the legacy path. */
static int solve_local_region(const int *g, int ar, int ac, int *col, int *row,
			     int peak_relative)
{
	unsigned char in[81] = { 0 };
	int stack[81], sp = 0, r0 = 4, r1 = 4, c0 = 4, c1 = 4;
	int pc[9], pr[9], ic = 4, ir = 4, i, j;
	int seed = 40, count = 0;
	int rlo = 4 - ar < 0 ? 0 : 4 - ar, rhi = 4 + SENSOR_ROWS - 1 - ar > 8 ? 8 : 4 + SENSOR_ROWS - 1 - ar;
	int clo = 4 - ac < 0 ? 0 : 4 - ac, chi = 4 + SENSOR_COLS - 1 - ac > 8 ? 8 : 4 + SENSOR_COLS - 1 - ac;

	if (peak_relative) {
		if (ar < 0 || ar >= SENSOR_ROWS || ac < 0 || ac >= SENSOR_COLS)
			return solve_local_region(g, ar, ac, col, row, 0);
		for (i = rlo; i <= rhi; i++)
			for (j = clo; j <= chi; j++)
				if (g[i * 9 + j] > g[seed]) seed = i * 9 + j;
		if (g[seed] <= REGION_FLOOR)
			return solve_local_region(g, ar, ac, col, row, 0);
		r0 = r1 = seed / 9;
		c0 = c1 = seed % 9;
	}
	/* Legacy: centre, eight-connected, > floor. Opt-in: on-sensor maximum,
	 * four-connected, also > half peak (GridFeatureExtractor::InPeak).
	 * With a global maximum seed, its adjacent-value ratio checks are
	 * redundant for this strictly-above-half component. */
	in[seed] = 1;
	stack[sp++] = seed;
	while (sp) {
		int k = stack[--sp], r = k / 9, c = k % 9;

		/* The reference rejects/cleans regions of ten or more cells. This
		 * narrow experiment does not port multi-peak cleanup: retain the
		 * shipped result for ambiguous broad regions instead. */
		if (peak_relative && ++count >= 10)
			return solve_local_region(g, ar, ac, col, row, 0);
		if (r < r0) r0 = r;
		if (r > r1) r1 = r;
		if (c < c0) c0 = c;
		if (c > c1) c1 = c;
		for (int dr = -1; dr <= 1; dr++)
			for (int dc = -1; dc <= 1; dc++) {
				int nr = r + dr, nc = c + dc, n = nr * 9 + nc;

				if (nr < rlo || nr > rhi || nc < clo || nc > chi || in[n] || g[n] <= REGION_FLOOR)
					continue;
				if (peak_relative && ((dr == 0) == (dc == 0) || g[n] <= (g[seed] >> 1)))
					continue;
				in[n] = 1;
				stack[sp++] = n;
			}
	}

	/* project the region's bounding box onto the columns and rows on the sensor */
	for (i = 0; i < 9; i++) {
		pc[i] = pr[i] = 0;
		for (j = r0; j <= r1 && i >= clo && i <= chi; j++)
			pc[i] += g[j * 9 + i] > 0 ? g[j * 9 + i] : 0;
		for (j = c0; j <= c1 && i >= rlo && i <= rhi; j++)
			pr[i] += g[i * 9 + j] > 0 ? g[i * 9 + j] : 0;
	}
	for (i = 0; i < 9; i++) {
		if (pc[i] > pc[ic]) ic = i;
		if (pr[i] > pr[ir]) ir = i;
	}
	*col = pitch_comp(solve_axis(pc, ic, edge_index(4 - ac), edge_index(4 + SENSOR_COLS - 1 - ac)),
			  pitch_cols);
	*row = pitch_comp(solve_axis(pr, ir, edge_index(4 - ar), edge_index(4 + SENSOR_ROWS - 1 - ar)),
			  pitch_rows);
	return peak_relative != 0;
}

static void solve_local(const int *g, int ar, int ac, int *col, int *row)
{
	solve_local_region(g, ar, ac, col, row, 0);
}

/*
 * PressureSolver::MapPressure (hpp3/PressureSolver.hpp), factory curve:
 * <= 11 -> 0/1, 12..127 -> x^2/127, above that a quartic; boosts light strokes
 * (200 -> 408, 1000 -> 2625, 2000 -> 3786).
 */
static int map_pressure(int x)
{
	double d = x, m;

	if (x >= 0x0fff)
		return 0x0fff;
	if (x <= 11)
		return x > 1 ? 1 : x;
	if (x <= 127)
		m = 0.0078740157480315 * d * d;
	else
		m = -409.317785463 + d * (4.39982201266 + d * (-0.00161165641489 +
		    d * (2.623779267e-07 + d * -1.60182e-11)));
	if (m < 0)
		m = 0;
	if (m > 0x0fff)
		m = 0x0fff;
	return (int)m;
}

/* ---- tilt: hpp3/TiltProcess.hpp, GridFeatureExtractor TX2 refine, CoorReviseProcess ---- */

/*
 * Full-tilt TX1-TX2 offsets per sensor axis, 1/UNIT cells (GetTiltAxisLength).
 * EGoTouchRev's constants (250 * 5 * 1024 / 0x102C = 309 cols, 387 rows) are
 * from its author's unit; this pen's ring electrode sits much higher (~6 mm
 * above the tip).  Measured here (BOE panel, 2026-10-08, solve_tx2 below):
 * ~90 upright, 975-1245 at "45 degrees", 1190-1335 held as flat as still
 * draws.  Flat strokes read ~6% more on rows than on cols, the cell pitch
 * ratio (4.43 / 4.16 mm), so rows scale by it.  1330 puts the "45 degree"
 * cols strokes at ~48 degrees and the flat ones at ~66.
 */
#define TILT_LEN_COLS	1330
#define TILT_LEN_ROWS	1418
#define TILT_HIST	10

struct tilt_state {
	int ratio[TILT_HIST], ratio_n;
	int dc[TILT_HIST], dr[TILT_HIST], d_n;	/* averaged TX1-TX2 offsets */
	int tc[TILT_HIST], tr[TILT_HIST];	/* per-frame tilt, degrees */
	int last_dc, last_dr;
	int out_c, out_r, have_out;		/* last reported tilt, degrees */
	int prev_press;
	/* CoorReviseProcess */
	int rc[TILT_HIST], rr[TILT_HIST], r_n, sm_c, sm_r, rev_prev_press;
};

/* asin, Abramowitz & Stegun 4.4.45 (|error| < 5e-5 rad); no libm here */
static double asin_approx(double x)
{
	int neg = x < 0;
	double r;

	if (neg)
		x = -x;
	if (x > 1)
		x = 1;
	r = 1.5707963267948966 - __builtin_sqrt(1.0 - x) *
	    (1.5707288 + x * (-0.2121144 + x * (0.0742610 + x * -0.0187293)));
	return neg ? -r : r;
}

/* GetTiltByCoorDif: degrees, sign as in EGoTouchRev (diff = TX1 - TX2) */
static int tilt_deg(int diff, int len)
{
	if (diff <= -len)
		return -90;
	if (diff >= len)
		return 90;
	return (int)(asin_approx((double)diff / len) * 180.0 / 3.141592657);
}

/*
 * GetTX1TX2LenLimit: shrink the allowed offset when TX2 is weak vs TX1 (pen
 * near upright, where the ring electrode is far from the glass).  Factory
 * table { 20, 90, 155, 200 } -> { 0, 850, 950, 1000 }; on this pen the ratio
 * rises faster with tilt (normal drawing: 30-44% upright-ish, 60-79% at offsets
 * 400-1000, 80%+ beyond), which clamped 82% of contact frames, so the ramp
 * now opens fully at 80%.  Hard presses raise TX1 and still lower the ratio.
 */
static int tilt_len_limit(int ratio)
{
	static const int th[4] = { 20, 50, 65, 80 }, sc[4] = { 0, 500, 750, 1000 };
	int scale = 1000;

	if (ratio <= th[0])
		return 0;
	if (ratio > th[3])
		return TILT_LEN_COLS;
	for (int i = 0; i < 3; i++)
		if (th[i] < ratio && ratio <= th[i + 1]) {
			scale = sc[i] + (ratio - th[i]) * (sc[i + 1] - sc[i]) / (th[i + 1] - th[i]);
			break;
		}
	return TILT_LEN_COLS * scale / 1000;
}

static int avg_n(const int *b, int have, int n)
{
	int s = 0;

	if (n > have)
		n = have;
	if (n <= 0)
		return 0;
	for (int i = 0; i < n; i++)
		s += b[i];
	return s / n;
}

static void push(int *b, int v)
{
	for (int i = TILT_HIST - 1; i > 0; i--)
		b[i] = b[i - 1];
	b[0] = v;
}

static int isqrt_u32(unsigned int x)
{
	int r = 0, bit = 0x8000;

	while (bit && x < (unsigned int)bit)
		bit >>= 1;
	for (; bit; bit >>= 1) {
		r += bit;
		if (x < (unsigned int)(r * r))
			r -= bit;
	}
	return r;
}

/* 3x3 sum around a cell (EGoTouchRev Calc3x3Sum; the TX1 "signalX") */
static int sum3x3(const int *g, int pr, int pc)
{
	int s = 0;

	for (int r = pr - 1; r <= pr + 1; r++)
		for (int c = pc - 1; c <= pc + 1; c++)
			if (r >= 0 && r < 9 && c >= 0 && c < 9 && g[r * 9 + c] > 0)
				s += g[r * 9 + c];
	return s;
}

/*
 * TX2 (ring electrode) position, global 1/UNIT cells, and its signal.
 * EGoTouchRev subtracts TX1/5 from TX2 as crosstalk first.  This pen shows no
 * such crosstalk: TX2 at the TX1 peak cell reads lower than its neighbours
 * (near upright the ring projects as an annulus around the tip), so the
 * subtraction only carved a hole there and pushed the centroid to one side
 * (upright offset 161 -> 88 without it, p90 473 -> 141).  The rest is the
 * original: seed > 99, mean-subtracted centroid over +-2 cells of the peak.
 */
static int solve_tx2(const int *g2, int a2r, int a2c, int *col, int *row, int *sig)
{
	int v[81], pk = -1, pv = 99, r0, r1, c0, c1, n = 0;
	long tot = 0, w = 0, wc = 0, wr = 0, base;

	for (int k = 0; k < 81; k++) {
		v[k] = g2[k] > 0 ? g2[k] : 0;
		if (v[k] > pv) {
			pv = v[k];
			pk = k;
		}
	}
	if (pk < 0)
		return 0;
	r0 = pk / 9 - 2 < 0 ? 0 : pk / 9 - 2;
	r1 = pk / 9 + 2 > 8 ? 8 : pk / 9 + 2;
	c0 = pk % 9 - 2 < 0 ? 0 : pk % 9 - 2;
	c1 = pk % 9 + 2 > 8 ? 8 : pk % 9 + 2;
	for (int r = r0; r <= r1; r++)
		for (int c = c0; c <= c1; c++, n++)
			tot += v[r * 9 + c];
	base = tot / n;
	for (int r = r0; r <= r1; r++)
		for (int c = c0; c <= c1; c++) {
			long x = v[r * 9 + c] - base;

			if (x > 0) {
				w += x;
				wc += c * x;
				wr += r * x;
			}
		}
	*col = (w ? (int)(wc * UNIT / w) : (pk % 9) * UNIT) + UNIT / 2 + (a2c - 4) * UNIT;
	*row = (w ? (int)(wr * UNIT / w) : (pk / 9) * UNIT) + UNIT / 2 + (a2r - 4) * UNIT;
	*sig = sum3x3(v, pk / 9, pk % 9);
	return 1;
}

static int jitter1(int prev, int cur)	/* JitterFilter1Degree */
{
	return prev < cur ? cur - 1 : cur < prev ? cur + 1 : cur;
}

/*
 * TiltProcess::Process for one frame with a valid TX2.  t1/t2 are global
 * positions (cols, rows) in 1/UNIT cells; press: this frame has pressure.
 * Averaging only runs while the tip is down; hover is single-frame.
 * Result in s->out_c / s->out_r (degrees, EGoTouchRev sign).
 */
static void tilt_update(struct tilt_state *s, int t1c, int t1r, int t2c, int t2r,
			int sig1, int sig2, int press)
{
	int ratio, lim, dc, dr, tc, tr, mag;

	if (!s->have_out || !s->prev_press) {
		s->ratio_n = s->d_n = 0;
		s->last_dc = s->last_dr = 0;
		for (int i = 0; i < TILT_HIST; i++)
			s->ratio[i] = s->dc[i] = s->dr[i] = s->tc[i] = s->tr[i] = 0;
	}
	ratio = sig1 ? sig2 * 100 / sig1 : 0;
	if (ratio > 500)
		ratio = 500;
	push(s->ratio, ratio);
	if (s->ratio_n < TILT_HIST)
		s->ratio_n++;
	lim = tilt_len_limit(avg_n(s->ratio, s->ratio_n, 3));

	dc = t1c - t2c;
	dr = t1r - t2r;
	if (s->d_n == 0) {
		dc = dc > lim ? lim : dc < -lim ? -lim : dc;
		dr = dr > lim ? lim : dr < -lim ? -lim : dr;
	} else if (dc > lim || dc < -lim || dr > lim || dr < -lim) {
		dc = (dc + s->last_dc * 7) / 8;
		dr = (dr + s->last_dr * 7) / 8;
	}
	push(s->dc, dc);
	push(s->dr, dr);
	if (s->d_n < TILT_HIST)
		s->d_n++;
	dc = avg_n(s->dc, s->d_n, 5);
	dr = avg_n(s->dr, s->d_n, 5);
	s->last_dc = dc;
	s->last_dr = dr;
	if (dc > lim || dc < -lim || dr > lim || dr < -lim) {
		dc = s->dc[0];
		dr = s->dr[0];
	}
	mag = isqrt_u32((unsigned int)(dc * dc + dr * dr));
	if (!mag)
		mag = 1;
	if (lim < mag) {
		dc = lim * dc / mag;
		dr = lim * dr / mag;
	}
	tc = tilt_deg(dc, TILT_LEN_COLS);
	tr = tilt_deg(dr, TILT_LEN_ROWS);
	push(s->tc, tc);
	push(s->tr, tr);
	if (s->prev_press && s->out_c) {
		tc = avg_n(s->tc, s->d_n, 5);
		tr = avg_n(s->tr, s->d_n, 5);
	}
	s->out_c = jitter1(s->out_c, tc);
	s->out_r = jitter1(s->out_r, tr);
	s->have_out = 1;
	s->prev_press = press;
}

/*
 * CoorReviseProcess: move the tip position a per-axis factor (units of 1/UNIT
 * cell per degree of tilt); while pressing, at most 15 per frame and a 5-frame
 * average.  The factors are flash parameters; EGoTouchRev uses 5 for both.
 * Measured here by tapping one target at different tilts (2026-10-08, two
 * rounds of 10): along cols the tip moves only 1.2 +- 0.8 units/degree, and 5
 * spread the taps further in both rounds (e.g. 0.48 -> 0.81 mm rms), so cols
 * use 1.  Rows came out 4.3-5.3 +- 1.4 (rows taps are hard to aim: the pen
 * body hides the tip), consistent with the original 5.
 */
#define REVISE_COLS	1
#define REVISE_ROWS	5

static void tilt_revise(struct tilt_state *s, int press, int *col, int *row)
{
	int oc = REVISE_COLS * s->out_c, orr = REVISE_ROWS * s->out_r;

	if (s->rev_prev_press && !press)
		s->r_n = 0;
	s->rev_prev_press = press;
	if (press) {
		if (s->r_n) {
			oc = oc > s->sm_c + 15 ? s->sm_c + 15 : oc < s->sm_c - 15 ? s->sm_c - 15 : oc;
			orr = orr > s->sm_r + 15 ? s->sm_r + 15 : orr < s->sm_r - 15 ? s->sm_r - 15 : orr;
		}
		push(s->rc, oc);
		push(s->rr, orr);
		if (s->r_n < TILT_HIST)
			s->r_n++;
		s->sm_c = avg_n(s->rc, s->r_n, 5);
		s->sm_r = avg_n(s->rr, s->r_n, 5);
	} else {
		s->sm_c = oc;
		s->sm_r = orr;
	}
	*col += s->sm_c;
	*row += s->sm_r;
}
