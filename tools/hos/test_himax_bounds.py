"""Replay shipped driver patches, then compile and exercise its reporting call.

Requires the existing local refs and rc2d manifest; never touches their files.
Only Himax source files are materialized, but every manifest patch/hash is
examined in release order, including all patches that touch those files.
"""
import hashlib
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
FILES = ('drivers/input/touchscreen/himax-spi-core.c',
         'drivers/input/touchscreen/hx-algo.c',
         'drivers/input/touchscreen/hx-algo.h')


class HimaxBoundsTests(unittest.TestCase):
    def test_release_replay_and_compiled_coordinate_boundaries(self):
        manifest = ROOT / 'out/rc2-1792421010/kernel-source.txt'
        base = ROOT / 'refs/gaokun-buildbot/patches/others/0003-Input-touchscreen-add-Himax-HX83121A-SPI-driver.patch'
        helper = ROOT / 'refs/linux-v7.2-rc2/drivers/input/touchscreen.c'
        if not all(p.exists() for p in (manifest, base, helper)):
            self.skipTest('local release manifest / reference sources unavailable')
        cc = shutil.which('cc')
        if not cc:
            self.skipTest('local C compiler unavailable')
        patches = re.findall(r'^\s*\d+\s+([0-9a-f]{64})\s+(patches/\S+)', manifest.read_text(), re.M)
        self.assertEqual(len(patches), 78)
        with tempfile.TemporaryDirectory(prefix='hos-himax-bounds-') as name:
            dest = Path(name)
            def apply(path):
                subprocess.run(['git', 'apply', '--unsafe-paths',
                                *['--include=' + f for f in FILES], str(path)],
                               cwd=dest, check=True, capture_output=True)
            apply(base)
            replayed = []
            for digest, path in patches:
                data = (ROOT / path).read_bytes()
                self.assertEqual(hashlib.sha256(data).hexdigest(), digest, path)
                targets = re.findall(rb'^\+\+\+ b/(\S+)', data, re.M)
                if set(t.decode() for t in targets).intersection(FILES):
                    apply(ROOT / path)
                    replayed.append(path)
            self.assertEqual(len(replayed), 12)
            source = dest / FILES[0]
            original = source.read_text()
            apply(ROOT / 'tools/hos/patches/himax-clamp-before-coordinate-transform.patch')
            patched = source.read_text()
            call_pattern = r'touchscreen_report_pos\(ts->input_dev, &ts->props,.*?true\);'
            original_call = re.findall(call_pattern, original, re.S)
            patched_call = re.findall(call_pattern, patched, re.S)
            self.assertEqual(len(original_call), 1)
            self.assertEqual(len(patched_call), 1)
            self.assertIn('clamp_t(s32,', patched_call[0])
            comment = ('\t\t/* Edge compensation may reach size, one past the inclusive max.\n'
                       '\t\t * Clamp while signed, before inversion and axis swapping.\n'
                       '\t\t */\n')
            self.assertEqual(patched.replace(comment, '').replace(patched_call[0], original_call[0]), original)
            transform = re.search(r'touchscreen_apply_prop_to_x_y\(.*?\n\}', helper.read_text(), re.S).group()
            program = r'''
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
typedef int32_t s32;
#define swap(a,b) do { unsigned int tmp=(a); (a)=(b); (b)=tmp; } while(0)
#define clamp_t(type,v,lo,hi) ({ type cv=(v), cl=(lo), ch=(hi); cv<cl?cl:cv>ch?ch:cv; })
struct touchscreen_properties { unsigned int max_x,max_y; bool invert_x,invert_y,swap_x_y; };
static void TRANSFORM
struct result { int32_t x,y; };
static void touchscreen_report_pos(struct result *out, const struct touchscreen_properties *p,
                                  unsigned int x,unsigned int y,bool mt) {
    (void)mt; touchscreen_apply_prop_to_x_y(p,&x,&y); out->x=(int32_t)x; out->y=(int32_t)y;
}
struct track { s32 x,y; };
struct algo { struct track tracks[1]; };
struct state { struct result *input_dev; struct touchscreen_properties props; struct algo *algo; };
static struct result report(s32 x,s32 y,unsigned int mx,unsigned int my,bool ix,bool iy,bool sw) {
    struct result out; struct algo a={{{x,y}}};
    struct state st={&out,{mx,my,ix,iy,sw},&a}, *ts=&st; int i=0;
    CALL
    return out;
}
#define CHECK(c) do { if(!(c)) { fprintf(stderr,"check failed line %d\n",__LINE__); return 1; } } while(0)
int main(void) {
    struct result r=report(2560,1600,2559,1599,true,false,true);
    CHECK(r.x==1599 && r.y==0);
    r=report(-1,-100,2559,1599,true,false,true);
    CHECK(r.x==0 && r.y==2559);
    r=report(INT32_MIN,INT32_MAX,2559,1599,true,false,true);
    CHECK(r.x==1599 && r.y==2559);
    /* Exhaustive native coordinates: the whole valid panel is unchanged. */
    for(int y=0;y<=1599;y++) for(int x=0;x<=2559;x++) {
        r=report(x,y,2559,1599,true,false,true);
        CHECK(r.x==y && r.y==2559-x);
    }
    /* Non-native bounds and every orientation ensure no hard-coded size. */
    for(int ix=0;ix<2;ix++) for(int iy=0;iy<2;iy++) for(int sw=0;sw<2;sw++) {
        for(int y=-2;y<=14;y++) for(int x=-2;x<=19;x++) {
            int cx=x<0?0:x>17?17:x, cy=y<0?0:y>12?12:y;
            int ex=ix?17-cx:cx, ey=iy?12-cy:cy;
            r=report(x,y,17,12,ix,iy,sw);
            CHECK(r.x==(sw?ey:ex) && r.y==(sw?ex:ey));
        }
    }
    return 0;
}
'''.replace('TRANSFORM', transform).replace('CALL', patched_call[0])
            cfile = dest / 'bounds.c'
            cfile.write_text(program)
            subprocess.run([cc, '-std=gnu11', '-O2', '-Wall', '-Wextra', '-Werror',
                            '-fsanitize=undefined', str(cfile), '-o', str(dest/'bounds')], check=True)
            subprocess.run([str(dest/'bounds')], check=True)


if __name__ == '__main__':
    unittest.main()
