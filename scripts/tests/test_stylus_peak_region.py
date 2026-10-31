#!/usr/bin/env python3
"""Exercise the real solver: weak connected tails must not move the strong peak.

Before the opt-in API exists, the test-only adapter calls shipped solve_local;
this gives a behavioral red assertion rather than a missing-symbol failure.
"""
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
DRIVER = r'''
#define main daemon_main
#include "gk3pend.c"
#undef main
#ifndef GK3_PEAK_REGION_API
static int solve_local_region(const int *g, int ar, int ac, int *c, int *r, int relative)
{
    (void)relative;
    solve_local(g, ar, ac, c, r);
    return 0;
}
#endif
#define CHECK(c, message) do { if (!(c)) { fprintf(stderr, "FAIL: %s\n", message); return 1; } } while (0)
static void base_grid(int *g)
{
    memset(g, 0, 81 * sizeof(*g));
    g[40] = 4000;
    g[39] = 1200; g[41] = 1200;
    g[31] = 3000; g[49] = 1000;
}
static unsigned int rng_state = 1917;
static int sample(void)
{
    rng_state = rng_state * 1664525u + 1013904223u;
    return (int)(rng_state >> 16) - 32768;
}
int main(int argc, char **argv)
{
    int g[81], c0, r0, c1, r1;
    if (argc != 2) return 2;
    if (!strcmp(argv[1], "profile")) {
        CHECK(region_on == 1, "hard peak not enabled by default");
        CHECK(pf_mincutoff == 1000 && pf_beta == 125 && pf_dcutoff == 5000, "scanned filter defaults differ");
        CHECK(SUBPX == 16, "PR55 subpixel precision lost");
        CHECK(screen_x_scale == 1.0, "unit-specific calibration leaked into global defaults");
        for (int i = 0; i <= 2559; i++)
            CHECK(screen_x_calibrate(i) == i, "identity calibration moved a coordinate");
        screen_x_scale = 1.0040851371;
        CHECK(screen_x_calibrate(1279.875) == 1279.875, "calibration moved center");
        CHECK(screen_x_calibrate(100) < 100 && screen_x_calibrate(2400) > 2400,
              "calibration does not expand both sides away from center");
    } else if (!strcmp(argv[1], "tail")) {
        base_grid(g);
        solve_local_region(g, 10, 20, &c0, &r0, 1);
        /* A weak branch expands the legacy box without joining the half-peak. */
        g[22] = 200; g[21] = 600;
        solve_local_region(g, 10, 20, &c1, &r1, 1);
        CHECK(c0 == c1 && r0 == r1, "weak connected tail changed strong-peak coordinates");
    } else if (!strcmp(argv[1], "diagonal")) {
        memset(g, 0, sizeof(g)); g[40] = 4000; g[41] = g[49] = 600;
        solve_local_region(g, 10, 20, &c0, &r0, 1);
        g[50] = 3000;
        solve_local_region(g, 10, 20, &c1, &r1, 1);
        CHECK(c0 == c1 && r0 == r1, "diagonal-only secondary peak joined the primary region");
    } else if (!strcmp(argv[1], "peak")) {
        base_grid(g);
        solve_local_region(g, 10, 20, &c0, &r0, 1);
        /* The anchor can be off the maximum by one cell; use physical peak. */
        int shifted[81] = {0};
        for (int r = 0; r < 9; r++) for (int c = 0; c < 8; c++) shifted[r*9+c+1] = g[r*9+c];
        solve_local_region(shifted, 10, 20, &c1, &r1, 1);
        CHECK(c1 == c0 + UNIT && r1 == r0, "off-centre strong peak did not translate by one cell");
    } else if (!strcmp(argv[1], "broad")) {
        memset(g, 0, sizeof(g));
        for (int r = 3; r < 6; r++) for (int c = 3; c < 7; c++) g[r*9+c] = 3000;
        g[40] = 4000;
        solve_local(g, 10, 20, &c0, &r0);
        int selected = solve_local_region(g, 10, 20, &c1, &r1, 1);
        CHECK(!selected && c0 == c1 && r0 == r1, "ambiguous broad region did not fall back to shipped behavior");
    } else if (!strcmp(argv[1], "nine-ten")) {
        memset(g, 0, sizeof(g));
        for (int r = 3; r < 6; r++) for (int c = 3; c < 6; c++) g[r*9+c] = 3000;
        g[40] = 5000;
        CHECK(solve_local_region(g, 10, 20, &c0, &r0, 1) == 1, "nine-cell region was rejected");
        g[42] = 3000;
        solve_local(g, 10, 20, &c0, &r0);
        CHECK(solve_local_region(g, 10, 20, &c1, &r1, 1) == 0, "ten-cell region was accepted");
        CHECK(c0 == c1 && r0 == r1, "ten-cell fallback differs from legacy");
    } else if (!strcmp(argv[1], "strict-threshold")) {
        for (int peak = 200; peak <= 4000; peak += 3800) {
            int below, equal, above, unused;
            memset(g, 0, sizeof(g));
            g[40] = peak; g[39] = g[41] = peak/4; g[30] = peak/5;
            g[31] = peak/2-1;
            solve_local_region(g, 10, 20, &below, &unused, 1);
            g[31] = peak/2;
            solve_local_region(g, 10, 20, &equal, &unused, 1);
            g[31] = peak/2+1;
            solve_local_region(g, 10, 20, &above, &unused, 1);
            CHECK(below == equal, "threshold-equal cell joined the peak region");
            CHECK(above != equal, "strictly-above-threshold cell failed to join region");
        }
    } else if (!strcmp(argv[1], "bounds")) {
        for (int ar = 0; ar < SENSOR_ROWS; ar++) for (int ac = 0; ac < SENSOR_COLS; ac++) {
            base_grid(g);
            for (int r = 0; r < 9; r++) for (int c = 0; c < 9; c++)
                if (ar+r-4 < 0 || ar+r-4 >= SENSOR_ROWS || ac+c-4 < 0 || ac+c-4 >= SENSOR_COLS) g[r*9+c] = 32767;
            solve_local_region(g, ar, ac, &c0, &r0, 1);
            for (int r = 0; r < 9; r++) for (int c = 0; c < 9; c++)
                if (ar+r-4 < 0 || ar+r-4 >= SENSOR_ROWS || ac+c-4 < 0 || ac+c-4 >= SENSOR_COLS) g[r*9+c] = -32768;
            solve_local_region(g, ar, ac, &c1, &r1, 1);
            CHECK(c0 == c1 && r0 == r1, "off-sensor crosstalk changed coordinates");
            CHECK(c0 >= 0 && c0 <= 9*UNIT && r0 >= 0 && r0 <= 9*UNIT, "coordinate outside local window");
        }
    } else if (!strcmp(argv[1], "legacy")) {
        for (int n = 0; n < 10000; n++) {
            for (int i = 0; i < 81; i++) g[i] = sample();
            g[40] = 200 + (sample() & 0x3fff);
            int ar = n % SENSOR_ROWS, ac = (n / SENSOR_ROWS) % SENSOR_COLS;
            solve_local(g, ar, ac, &c0, &r0);
            solve_local_region(g, ar, ac, &c1, &r1, 0);
            CHECK(c0 == c1 && r0 == r1, "disabled policy differs from shipped solver");
            solve_local_region(g, ar, ac, &c1, &r1, 1);
            CHECK(c1 >= 0 && c1 <= 9*UNIT && r1 >= 0 && r1 <= 9*UNIT, "random grid outside local window");
        }
    } else if (!strcmp(argv[1], "empty")) {
        memset(g, 0, sizeof(g));
        solve_local(g, 10, 20, &c0, &r0);
        int selected = solve_local_region(g, 10, 20, &c1, &r1, 1);
        CHECK(!selected && c0 == c1 && r0 == r1, "empty peak did not fall back");
    } else return 2;
    printf("PASS: %s\n", argv[1]);
    return 0;
}
'''

with tempfile.TemporaryDirectory(prefix='gk3-region-') as tmp:
    tmp = Path(tmp)
    (tmp/'test.c').write_text(DRIVER)
    cmd = ['gcc', '-std=gnu11', '-O2', '-fno-math-errno', '-Wall', '-Wextra', '-Werror',
           '-I', str(ROOT/'device/huawei/gaokun3/stylus'), str(tmp/'test.c'), '-o', str(tmp/'test'), '-lm']
    if '--ubsan' in sys.argv:
        cmd += ['-fsanitize=undefined', '-fno-sanitize-recover=all']
    subprocess.run(cmd, check=True)
    cases = [sys.argv[sys.argv.index('--case')+1]] if '--case' in sys.argv else [
        'profile', 'tail', 'diagonal', 'peak', 'broad', 'nine-ten', 'strict-threshold', 'bounds', 'legacy', 'empty']
    for case in cases:
        subprocess.run([str(tmp/'test'), case], check=True, timeout=30)
