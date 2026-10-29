"""Compile the patched production plan-building function with a fake DRM backend.

The fake backend preserves the real backend's one-plan-entry-per-input-layer
contract, without requiring Android headers or a DRM device on the host.
"""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


class ComposerIndexTest(unittest.TestCase):
    def test_sparse_z_order_uses_dense_plan_index(self):
        repo = Path(__file__).resolve().parents[2]
        source = repo / 'refs/a17/external/drm_hwcomposer'
        compiler = shutil.which('clang++') or shutil.which('g++')
        if not compiler or not (source / 'hwc/HwcDisplay.cpp').exists():
            self.skipTest('requires host C++ compiler and reference composer source')
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            for name in ('hwc/HwcDisplay.cpp', 'compositor/LayerToPlaneJoiningPlan.h'):
                dest = root / name
                dest.parent.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(source / name, dest)
            subprocess.run(['git', 'apply', str(repo / 'tools/hos/patches/drm-hwcomposer-client-index.patch')],
                           cwd=root, check=True, capture_output=True)
            text = (root / 'hwc/HwcDisplay.cpp').read_text()
            begin = text.index('std::unique_ptr<LayerToPlaneJoiningPlan>\nHwcDisplay::CreateLayerToPlaneJoiningPlan(')
            end = text.index('\nbool HwcDisplay::CommitStagedComposition', begin)
            unit = PRELUDE + text[begin:end] + MAIN
            (root / 'test.cpp').write_text(unit)
            result = subprocess.run([compiler, '-std=c++17', '-Wall', '-Wextra', '-Werror',
                                     str(root / 'test.cpp'), '-o', str(root / 'test')],
                                    capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            subprocess.run([str(root / 'test')], check=True, capture_output=True)


PRELUDE = r'''
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <vector>
#define ALOGW(...) ((void)0)
#define ALOGW_IF(...) ((void)0)
#define ALOGE(...) ((void)0)
enum class CompositionType { kDevice, kCursor, kClient, kInvalid, kDeviceOccluded, kSolidColor };
struct LayerData { int id; };
struct HwcLayer {
  uint32_t z;
  int id;
  bool usable = true;
  uint32_t GetZOrder() const { return z; }
  LayerData GetLayerData() const { return {id}; }
  bool IsLayerUsableAsDevice() const { return usable; }
};
struct CompositionPlanner {
  using CompositionTypeMap = std::map<const HwcLayer *, CompositionType>;
};
struct LayerToPlaneJoiningPlan {
  std::vector<LayerData> plan;
  std::optional<int> client_z_order;
  static std::unique_ptr<LayerToPlaneJoiningPlan> CreateLayerToPlaneJoiningPlan(
      int, std::vector<LayerData> layers, std::optional<LayerData> cursor) {
    auto p = std::make_unique<LayerToPlaneJoiningPlan>();
    p->plan = std::move(layers);
    if (cursor) p->plan.push_back(*cursor);
    return p;
  }
};
struct HwcDisplay {
  std::map<int, HwcLayer> layers_;
  HwcLayer client_layer_{0, 9999};
  int GetPipe() const { return 0; }
  std::unique_ptr<LayerToPlaneJoiningPlan> CreateLayerToPlaneJoiningPlan(
      const CompositionPlanner::CompositionTypeMap &) const;
};
'''

MAIN = r'''
int main() {
  for (uint32_t z : {1U, 100U, 0x10000001U, 0xfffffffeU}) {
    for (bool device_below : {false, true}) {
      for (bool cursor : {false, true}) {
        HwcDisplay display;
        CompositionPlanner::CompositionTypeMap types;
        display.layers_.emplace(1, HwcLayer{z, 1});
        types[&display.layers_.at(1)] = CompositionType::kClient;
        if (device_below) {
          display.layers_.emplace(2, HwcLayer{z - 1, 2});
          types[&display.layers_.at(2)] = CompositionType::kDevice;
        }
        if (cursor) {
          display.layers_.emplace(3, HwcLayer{z + 1, 3});
          types[&display.layers_.at(3)] = CompositionType::kCursor;
        }
        auto plan = display.CreateLayerToPlaneJoiningPlan(types);
        assert(plan && plan->client_z_order);
        assert(*plan->client_z_order == (device_below ? 1 : 0));
        assert(plan->plan.at(*plan->client_z_order).id == 9999);
        if (cursor) assert(plan->plan.back().id == 3);
        display.client_layer_.usable = false;
        assert(!display.CreateLayerToPlaneJoiningPlan(types));
      }
    }
  }
  HwcDisplay display;
  display.layers_.emplace(1, HwcLayer{0x10000001U, 1});
  CompositionPlanner::CompositionTypeMap types;
  types[&display.layers_.at(1)] = CompositionType::kDevice;
  auto plan = display.CreateLayerToPlaneJoiningPlan(types);
  assert(plan && !plan->client_z_order && plan->plan.at(0).id == 1);
}
'''


if __name__ == '__main__':
    unittest.main()
