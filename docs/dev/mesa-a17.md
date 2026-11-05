# Android 17上的Turnip：基于meson2hermetic重新生成Mesa 26.1构建规则

状态：**现行**（1.0.0-rc.3起随Android 17发布构建使用）。

Android 17的`external/mesa3d`升级到了26.1.0-devel，移除了旧版`meson_to_hermetic`脚本。本仓采用上游新版生成器`meson2hermetic`（`meson convert`），在上游Android配置基础上加入`gaokun_turnip`目标，一次性重新生成整棵Mesa的`Android.bp`，直接产出`cc_library_shared "vulkan.freedreno"`。

```bash
# 在副本目录上测试生成（约20秒，不执行m）：
rm -rf ~/a17-mesa-work/mesa3d && mkdir -p ~/a17-mesa-work &&
  cp -a ~/crdroid/external/mesa3d ~/a17-mesa-work/mesa3d && rm ~/a17-mesa-work/mesa3d/.git
bash scripts/mesa-a17/regen.sh ~/a17-mesa-work/mesa3d --tree ~/crdroid --export ~/a17-mesa-work/turnip-a17-bp.tar.gz

# 确认无误后应用到源码树并编译：
bash scripts/mesa-a17/regen.sh ~/crdroid/external/mesa3d --allow-crdroid-tree --tree ~/crdroid
m vulkan.freedreno
```

## 1. Android 17源码树中的Mesa现状

| 项目 | 详情 |
| --- | --- |
| 版本与基线 | `VERSION` = `26.1.0-devel`，提交`875160ed32c`（2026-03快照） |
| 构建规则改动 | 提交`7d26db2ca2a`删除了`meson_to_hermetic/`；提交`34538425e1f`改为由`meson2hermetic`生成的90个按目录拆分的`Android.bp` |
| 配置文件缺失 | `README.aosp.md`记载的生成命令依赖`hermetic/android/{aosp_mesa3d,platforms,dependencies}.toml`，但上游直到MR !42932（2026-07）才将配置文件合入`src/gfxstream/hermetic/android/`，晚于Android 17快照 |
| 默认驱动范围 | 树内现成`Android.bp`仅包含`gfxstream`（`vulkan.ranchu`）和`lavapipe`（`vulkan.lvp`），不含`freedreno` / `turnip` |
| 手写兼容块 | 根`Android.bp:518-701`保留了供`device/generic/goldfish`与`hardware/google/gfxstream`按名引用的手写模块定义 |
| ANB延迟绑定 | `tu_image.cc:1013`中`VkNativeBufferANDROID`分支仍为`UNREACHABLE`，需继续应用本仓`patches/0004` |

## 2. 生成方案设计

采用上游`gurchetansingh/mesonbuild`的`meson2hermetic`分支（固定在提交`1217e6125d76`），将`gaokun_turnip`作为附加配置与上游`gfxstream` / `lavapipe`配置一起运行`meson convert`，一次性生成整棵树的`Android.bp`：
* **避免模块重名冲突**：Turnip依赖的`mesa_util`、`vulkan_runtime`、`nir`、`compiler`等公共模块与树内现成模块同名，单独生成Turnip再拼接会产生大量命名冲突；由生成器在同一平台下通过`select()`自动求并集可保持模块名天然闭合。
* **静态库依赖自动传递**：新版生成器包含提交`0c0522d`（"Propagate shared library dependencies from linked static targets"），并直接生成`cc_library_shared "vulkan.freedreno"`，后处理只需补充`compile_multilib: "64"`。

## 3. 生成管线（`scripts/mesa-a17/`）

| 步骤 | 操作内容 | 对应文件 |
| --- | --- | --- |
| 1 | 检查`VERSION`为`26.1.*`且`python3 >= 3.11`（需标准库`tomllib`） | [`scripts/mesa-a17/regen.sh`](../../scripts/mesa-a17/regen.sh) |
| 2 | 按固定SHA浅克隆生成器至`~/.cache/gaokun-mesa/mesonbuild-<SHA>`（或由`--generator`指定本地路径） | 同上 |
| 3 | 校验根`Android.bp`末尾的`gfxstream`手写兼容块与存档副本逐字一致，生成后原样贴回 | [`device/huawei/gaokun3/mesa/hermetic/android/gfxstream_workaround.txt`](../../device/huawei/gaokun3/mesa/hermetic/android/gfxstream_workaround.txt) |
| 4 | 幂等应用源码补丁：上游9个backport提交 + [`patches/0004`](../../patches/0004-mesa-turnip-handle-anb-rebind-with-null-memory.patch) + [`patches/0130`](../../patches/0130-mesa-turnip-support-p010.patch) | [`scripts/mesa-a17/backports/`](../../scripts/mesa-a17/backports/) |
| 5 | 清理带`The Magma GPU Project`头部的旧生成文件，执行`meson convert` | [`device/huawei/gaokun3/mesa/hermetic/android/`](../../device/huawei/gaokun3/mesa/hermetic/android/aosp_mesa3d.toml) |
| 6 | 后处理：为`vulkan.freedreno`补齐`compile_multilib: "64"` | [`scripts/mesa-a17/postprocess.py`](../../scripts/mesa-a17/postprocess.py) |
| 7 | 运行`bpfmt -l`检查语法，运行`check-output.py`验证模块闭合性与编译参数 | [`scripts/mesa-a17/check-output.py`](../../scripts/mesa-a17/check-output.py) |

### 3.1 关键配置项（`device/huawei/gaokun3/mesa/hermetic/android/`）

* **`aosp_mesa3d.toml`**：新增`gaokun_turnip`配置（仅`android_arm64`，`vulkan-drivers = "freedreno"`，`freedreno-kmds = "msm"`，关闭EGL / GBM / GL，GLES由ANGLE提供）。
  * `gaokun_turnip`与`aosp_reference_drivers`均显式设置`platform-sdk-version = 37`：一方面避免同一`Android.bp`的`"android"`与`("arm64","android")`分支出现`-DANDROID_API_LEVEL`宏重定义，另一方面确保`ANDROID_STRICT`下要求API≥36的扩展（`VK_KHR_compute_shader_derivatives`、`VK_KHR_maintenance7`、`VK_KHR_shader_relaxed_extended_instruction`）正常暴露。
* **`dependencies.toml`**：移除`libzstd`。生成器只要在`dependencies.toml`中查到条目就会无条件置`is_found = True`，而vendor分区的`libzstd`未标记`same_process_hal_file`，在SELinux enforcing下会导致应用进程加载Turnip失败。

### 3.2 源码补丁清单

| 补丁 | 来源 | 作用 |
| --- | --- | --- |
| Backport 01–04（`dd47c7c`、`b0e480d`、`f9a5524`、`1084ddd`） | 上游MR !41518 | 修复Soong全局`-Werror`下的编译警告 |
| Backport 05–06（`1fe5838`、`00261e5`） | 上游MR !41518 | 为寄存器XML `genrule`补齐`depend_files`声明，解决Soong沙箱内找不到`adreno_common.xml`的问题 |
| Backport 07–09（`cb614dd`、`12a0367`、`b6b0902`） | 上游MR !44278 | 向下游传递`vulkan_runtime`接口依赖，并启用`u_gralloc`的`IMapper5`后端（依赖`libui`） |
| `patches/0004` | 本仓适配至26.1 | Turnip `VkNativeBufferANDROID`（NULL-memory bind）延迟绑定至首个command buffer提交时导入gralloc buffer（详见[`gpu-turnip.md`](gpu-turnip.md)） |
| `patches/0130` | 上游MR !45721适配 | Turnip暴露`VK_FORMAT_G10X6_B10X6R10X6_2PLANE_420_UNORM_3PACK16`（P010），配合`minigbm`开启10-bit视频零拷贝渲染 |

## 4. 构建与实机验证要点

1. **符号导出检查**：`llvm-readelf --dyn-syms vulkan.freedreno.so`需包含`HMI`与`vk_icdGetInstanceProcAddr`。
2. **安装路径与SELinux标签**：产物位于`/vendor/lib64/hw/vulkan.freedreno.so`（32位不编译），由`sepolicy/file_contexts`标记为`same_process_hal_file`。
3. **上机验证**：运行`scripts/verify-turnip.sh`确认GLES渲染器为`Turnip Adreno (TM) 690`、SMMU fault为0、截屏正常，并按[`gaming.md`](gaming.md)回归主流3D游戏。
