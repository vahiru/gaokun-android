#!/usr/bin/env python3
"""B1 诊断补丁：定性"到底是哪个 image 没绑内存却被当渲染目标用"。

背景（见 docs/dev/gpu-turnip.md）：
  patch 0004 v2 让 Android 用 turnip 启动进桌面了，但仍有少量 GPU SMMU
  fault（`FAR=0x100`，即 iova≈0 + 小偏移），7 分钟后拖垮框架。
  `struct tu_image` 的注释写明 `iova` 是 "Set when bound"，未绑定就是 0
  —— 所以"未绑内存的 image 被当 attachment"会让 GPU 写到 iova 0。

  另外读源码发现：v2 的两处修改里，真正止住 SIGSEGV 的很可能只是
  tu_cmd_buffer.cc 的那句 `iview->image->mem &&` 防御（它把问题变成静默的
  iova=0），而不是 anb_memory 绑定 —— 因为 ANB image 在 vkCreateImage 里
  就被 vk_android_import_anb() 绑好了（tu_image.cc:893）。
  所以必须先查清"谁在用没绑定的 image"，再谈修法。

三个探针（各自限流 32 次，避免刷屏）：
  P1 tu_image_bind 收到 memory=NULL —— 打 image 属性 + pNext 链 + 调用栈
  P2 tu_image_view_init 给 iova==0 的非 sparse image 建 view —— ★真凶哨兵
  P3 tu_allocate_transient_attachments 跳过无内存 attachment —— 每帧命中计数

看日志：adb logcat | grep gaokun
幂等，可重复运行。
"""
import sys

V = '/home/vahiru/aosp/external/mesa3d/src/freedreno/vulkan/'
rc = 0


def patch(path, edits):
    """edits = [(名字, old, new)]，全部幂等。"""
    global rc
    s = open(path).read()
    for name, old, new in edits:
        if new in s:
            print(f'{path.split("/")[-1]}: {name} 已在，跳过')
            continue
        if old not in s:
            print(f'!! {path.split("/")[-1]}: {name} 未匹配锚点', file=sys.stderr)
            rc = 1
            continue
        s = s.replace(old, new, 1)
        print(f'{path.split("/")[-1]}: {name} 已应用')
    open(path, 'w').write(s)


# ─────────────────────── tu_image.cc ───────────────────────
HELPERS = r'''
/* ─── gaokun B1 诊断（临时，定位残余 SMMU fault 用）─── */
#include <dlfcn.h>
#include <inttypes.h>
#include "util/log.h"

/* ⚠️ 必须放在文件靠前处：tu_image_view_init（约 190 行）就要用它。
 * ⚠️ image 不能是 const：vk_image_is_android_native_buffer() 要非 const。 */
void
gaokun_dump_image(const char *tag, struct tu_image *image)
{
   mesa_logw("gaokun %s image=%p fmt=%u %ux%ux%u mips=%u layers=%u samples=%u "
             "tiling=%u usage=0x%x flags=0x%x size=%" PRIu64 " iova=0x%" PRIx64
             " mem=%p ubwc=%d anb=%d ahb=%d anb_mem=%p",
             tag, (const void *) image, (unsigned) image->vk.format,
             image->vk.extent.width, image->vk.extent.height,
             image->vk.extent.depth, image->vk.mip_levels,
             image->vk.array_layers, (unsigned) image->vk.samples,
             (unsigned) image->vk.tiling, (unsigned) image->vk.usage,
             (unsigned) image->vk.create_flags, image->total_size, image->iova,
             (image->vk.create_flags & VK_IMAGE_CREATE_SPARSE_BINDING_BIT)
                ? (void *) 0x1 : (void *) image->mem,
             image->ubwc_enabled,
             vk_image_is_android_native_buffer(&image->vk),
             vk_image_is_android_hardware_buffer(&image->vk),
             (void *) (uintptr_t) image->vk.anb_memory);
}

static void
gaokun_dump_callers(const char *tag)
{
   void *ra[4] = { __builtin_return_address(0), __builtin_return_address(1),
                   __builtin_return_address(2), __builtin_return_address(3) };
   for (int i = 0; i < 4; i++) {
      if (!ra[i])
         break;
      Dl_info di;
      if (dladdr(ra[i], &di) && di.dli_fname) {
         mesa_logw("gaokun %s  caller[%d] %p  %s!%s", tag, i, ra[i],
                   di.dli_fname, di.dli_sname ? di.dli_sname : "?");
      } else {
         mesa_logw("gaokun %s  caller[%d] %p  ?", tag, i, ra[i]);
      }
   }
}
/* ─── gaokun B1 诊断结束 ─── */

'''

ANCHOR_HELPERS = '''#include "tu_wsi.h"
'''

P1_OLD = '''      if (image->vk.anb_memory != VK_NULL_HANDLE) {
         mem = tu_device_memory_from_handle(image->vk.anb_memory);
         offset = 0;
      } else {
         /* 既没传 memory 又没有已导入的 ANB 内存：无可绑定，跳过而不是 UB */
         return VK_SUCCESS;
      }'''

P1_NEW = '''      /* gaokun B1 探针 P1：谁在用 memory=NULL 绑定？绑的是什么 image？ */
      {
         static unsigned n_p1 = 0;
         if (n_p1++ < 32) {
            char pn[192] = "";
            size_t off = 0;
            for (const VkBaseInStructure *s =
                    (const VkBaseInStructure *) bind_info->pNext;
                 s && off < sizeof(pn) - 16; s = s->pNext)
               off += snprintf(pn + off, sizeof(pn) - off, "%u,",
                               (unsigned) s->sType);
            mesa_logw("gaokun P1 NULL-bind #%u pNext=[%s] anb_mem=%p", n_p1, pn,
                      (void *) (uintptr_t) image->vk.anb_memory);
            gaokun_dump_image("P1", image);
            gaokun_dump_callers("P1");
         }
      }

      if (image->vk.anb_memory != VK_NULL_HANDLE) {
         mem = tu_device_memory_from_handle(image->vk.anb_memory);
         offset = 0;
      } else {
         /* 既没传 memory 又没有已导入的 ANB 内存：无可绑定，跳过而不是 UB */
         return VK_SUCCESS;
      }'''

P2_OLD = '''   vk_image_view_init(&device->vk, &iview->vk, pCreateInfo);
   assert(iview->vk.format != VK_FORMAT_UNDEFINED);

   iview->image = image;'''

P2_NEW = '''   vk_image_view_init(&device->vk, &iview->vk, pCreateInfo);
   assert(iview->vk.format != VK_FORMAT_UNDEFINED);

   iview->image = image;

   /* gaokun B1 探针 P2 ★：给"未绑定内存"的 image 建 view —— 这个 view 一旦
    * 当 attachment/纹理用，GPU 就会访问 iova 0 → SMMU translation fault。 */
   if (image->iova == 0 &&
       !(image->vk.create_flags & VK_IMAGE_CREATE_SPARSE_BINDING_BIT)) {
      static unsigned n_p2 = 0;
      if (n_p2++ < 32) {
         mesa_logw("gaokun P2 view-on-unbound-image #%u", n_p2);
         gaokun_dump_image("P2", image);
         gaokun_dump_callers("P2");
      }
   }'''

patch(V + 'tu_image.cc', [
    ('诊断辅助函数', ANCHOR_HELPERS, ANCHOR_HELPERS + HELPERS),
    ('P1 NULL-bind 探针', P1_OLD, P1_NEW),
    ('P2 unbound-view 哨兵', P2_OLD, P2_NEW),
])

# ─────────────────────── tu_cmd_buffer.cc ───────────────────────
P3_OLD = '''      if (iview && iview->image->mem &&'''

P3_NEW = '''      /* gaokun B1 探针 P3：统计"无内存 attachment"实际命中频次 */
      if (iview &&
          !(iview->image->vk.create_flags & VK_IMAGE_CREATE_SPARSE_BINDING_BIT) &&
          !iview->image->mem) {
         static unsigned n_p3 = 0;
         if (n_p3++ < 32) {
            mesa_logw("gaokun P3 unbound-attachment #%u idx=%u sysmem=%d", n_p3,
                      i, sysmem);
            gaokun_dump_image("P3", iview->image);
         }
      }

      if (iview && iview->image->mem &&'''

P3_DECL_OLD = '''static VkResult
tu_allocate_transient_attachments(struct tu_cmd_buffer *cmd, bool sysmem)'''

P3_DECL_NEW = '''/* gaokun B1 诊断：定义在 tu_image.cc */
extern void gaokun_dump_image(const char *tag, struct tu_image *image);

static VkResult
tu_allocate_transient_attachments(struct tu_cmd_buffer *cmd, bool sysmem)'''

patch(V + 'tu_cmd_buffer.cc', [
    ('P3 声明', P3_DECL_OLD, P3_DECL_NEW),
    ('P3 unbound-attachment 探针', P3_OLD, P3_NEW),
])

sys.exit(rc)
