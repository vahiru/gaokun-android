#!/usr/bin/env python3
"""patch 0004 v3：实现 turnip 的 `TODO handle VkNativeBufferANDROID`。

实测定性（2026-08-19，三探针一轮编译，见 docs/dev/gpu-turnip.md）：
  Android 的 libvulkan 走 **延迟绑定** —— vkCreateImage 时**不带** ANB
  （所以 vk_image_init 没把 android_buffer_type 设成 NATIVE、
  vk_android_import_anb() 从未运行、vk.anb_memory 为空），
  到 vkBindImageMemory2 才把 gralloc buffer 用 VkNativeBufferANDROID 递进来：

    P1 NULL-bind pNext=[1000010000, 1000060009]
                       ↑NATIVE_BUFFER_ANDROID  ↑BIND_IMAGE_MEMORY_SWAPCHAIN_INFO_KHR
    caller: libGLESv2_angle.so → libvulkan.so → vulkan.freedreno.so
    image:  1600x2560 fmt=37 usage=0x97  anb=0 ahb=0 anb_mem=0 mem=0 iova=0

  turnip 从来没实现这条路（`UNREACHABLE("VkBindImageMemoryInfo with no memory")`
  下面就是那句 TODO），于是：
    v1 → UNREACHABLE 在 release 下是 UB
    v2 → 找 create 时的 anb_memory，这里当然是空 → "安全跳过" → image 永远
         没有内存、iova 恒 0 → **GPU 往地址 0 写** → GPU SMMU translation
         fault（实测 66 次里 65 次是写，FAR 全是 1600 宽 RGBA 图的行偏移）
         → 本平台 context-fault 中断打不到 CPU → stall 死锁（D5/D6 那条链）

v3 的修法（复用 runtime 现成 helper，可直接提上游）：
  ① 用 bind info 里的 ANB 造一份等价 VkImageCreateInfo
  ② vk_android_get_anb_layout() 取 gralloc buffer 的真实 modifier/plane layout
     → tu_image_update_layout() 重算布局
     （**必须做**：image 默认 UBWC，而我们的 minigbm 是 nocompression=LINEAR，
       不重算就是错位 + 越界写）
  ③ vk_android_import_anb() 导入 dma-buf 并绑定（它自己会再调一次
     BindImageMemory2，这次 memory 非空，走正常路径落 mem/iova）
     重复绑定先 FreeMemory 掉上一块，否则每次轮转漏一个 dma-buf

幂等。运行前提：文件处于 v2 状态（可选叠加 apply-diag-b1.py 的探针）。
"""
import sys

V = '/home/vahiru/aosp/external/mesa3d/src/freedreno/vulkan/'
P = V + 'tu_image.cc'

OLD = """      if (image->vk.anb_memory != VK_NULL_HANDLE) {
         mem = tu_device_memory_from_handle(image->vk.anb_memory);
         offset = 0;
      } else {
         /* 既没传 memory 又没有已导入的 ANB 内存：无可绑定，跳过而不是 UB */
         return VK_SUCCESS;
      }"""

NEW = """      /* ★ Android loader 的延迟绑定：gralloc buffer 是在 **bind 这一刻**
       * 才通过 pNext 的 VkNativeBufferANDROID 递进来的（vkCreateImage 时没有，
       * 故 vk.anb_memory 为空）。这正是 mesa 那句
       * "TODO handle VkNativeBufferANDROID" 指的路径。不实现它 =
       * image 永远没内存、iova 恒 0 → GPU 写地址 0 → SMMU translation fault。
       * (gaokun patch 0004 v3；实测调用方 ANGLE → libvulkan) */
      const VkNativeBufferANDROID *anb =
         vk_find_struct_const(bind_info->pNext, NATIVE_BUFFER_ANDROID);
      if (anb) {
         /* 造一份等价 create info，pNext 指向刚拿到的 ANB，直接复用 runtime
          * 的两个 helper（它们都以 VkImageCreateInfo 为入口）。 */
         VkImageCreateInfo anb_ci = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
            .pNext = (const void *) anb,
            .flags = image->vk.create_flags,
            .imageType = image->vk.image_type,
            .format = image->vk.format,
            .extent = image->vk.extent,
            .mipLevels = image->vk.mip_levels,
            .arrayLayers = image->vk.array_layers,
            .samples = (VkSampleCountFlagBits) image->vk.samples,
            .tiling = image->vk.tiling,
            .usage = image->vk.usage,
            .sharingMode = image->vk.sharing_mode,
            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
         };

         /* ① 按 gralloc buffer 的真实布局重算：image 默认走 UBWC，而 gralloc
          *    可能是 LINEAR（我们现在 vendor.minigbm.debug=nocompression），
          *    不重算就是错位渲染 + 越界写。 */
         VkImageDrmFormatModifierExplicitCreateInfoEXT anb_eci;
         VkSubresourceLayout anb_plane_layouts[TU_MAX_PLANE_COUNT];
         result = vk_android_get_anb_layout(&anb_ci, &anb_eci,
                                           anb_plane_layouts,
                                           TU_MAX_PLANE_COUNT);
         if (result != VK_SUCCESS)
            return result;

         result = TU_CALLX(device, tu_image_update_layout)(
            device, image, anb_eci.drmFormatModifier, anb_plane_layouts);
         if (result != VK_SUCCESS)
            return result;

         /* ② 真正导入并绑定。vk_android_import_anb() 会 dup dma-buf fd、
          *    以 dedicated 方式 AllocateMemory 存进 vk.anb_memory，然后自己
          *    再调一次 BindImageMemory2 —— 那次 memory 非空，会走下面的正常
          *    路径把 mem/iova 落实，所以这里导入完直接返回。
          *    ⚠️ 重复绑定（BufferQueue 轮转）要先放掉上一块，否则漏 dma-buf。 */
         if (image->vk.anb_memory != VK_NULL_HANDLE) {
            device->vk.dispatch_table.FreeMemory(tu_device_to_handle(device),
                                                 image->vk.anb_memory, NULL);
            image->vk.anb_memory = VK_NULL_HANDLE;
         }

         return vk_android_import_anb(&device->vk, &anb_ci, NULL, &image->vk);
      }

      if (image->vk.anb_memory != VK_NULL_HANDLE) {
         /* create 时就导入过 ANB 的老路（真 ANB image 的重复绑定）。 */
         mem = tu_device_memory_from_handle(image->vk.anb_memory);
         offset = 0;
      } else {
         /* 既没 ANB、又没已导入内存：无可绑定。跳过而不是 UB，但要吼一声
          * —— 这条路走通了就意味着又有 image 会拿着 iova 0 去渲染。 */
         static unsigned n_unbindable = 0;
         if (n_unbindable++ < 8)
            mesa_logw("gaokun 无法绑定的 NULL-bind #%u：既无 VkNativeBufferANDROID "
                      "也无 anb_memory，image %p 将保持 iova=0", n_unbindable,
                      (void *) image);
         return VK_SUCCESS;
      }"""

s = open(P).read()
if NEW in s:
    print('tu_image.cc: 已是 v3，跳过')
elif OLD in s:
    open(P, 'w').write(s.replace(OLD, NEW, 1))
    print('tu_image.cc: patch 0004 v3 已应用（实现 VkNativeBufferANDROID 延迟绑定）')
else:
    print('!! tu_image.cc 未匹配 v2 锚点（当前状态不是 v2？）', file=sys.stderr)
    sys.exit(1)
