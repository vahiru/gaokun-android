# 前后摄照片逆时针旋转 90° —— 根因定位与修复

> 2026-09-19 · 仓库 `gaokun-android` · 涉及 `device/huawei/gaokun3/camera/`
> 症状：前摄、后摄都能正常打开，但拍出来的照片**画面固定被转了 90°（用户描述为逆时针）**。

> ### ⚠️ 方向的换算后来改了（2026-11，#23、#42）
>
> 这份记录里关于方向换算的内容大多已经过时，主要有：§5.3的判据`N == (360-M)%360`和"机身自然朝向是横屏"的前提（Android的自然方向是竖屏），§5.1里A7断言的旧式子，§6的标定表（那组值后来在§7.2和`patches/0049`里也被推翻了），以及§4.1、§8里标定用的调试属性`debug.gaokun3.camera.orientation.*`（已从HAL里去掉）。现在HAL先扣掉libcamera用传感器翻转消掉的角，再从横屏参照系换到竖屏自然方向：设备树仍是后摄180、前摄0，上报的SENSOR_ORIENTATION是后摄90、前摄270。现行说明在`device/huawei/gaokun3/camera/Device.cpp`的方向一段。

> ### ⚠️ 基线说明（2026-09-19 晚，后补）
>
> 本修复最初基于 `c9cae65`，那是**落后 `origin/main` 52 个提交**的旧树。
> 已 **rebase 到 `823585f`**（= `origin/main`），分支 `fix/photo-rotation-2026-09-19`。
>
> 起因：装上 ROM(09-16) 的设备上，HAL 二进制比本地源码多出**闪光灯/手电筒**与**静态照片降噪**两组功能。
> 一度判断为"构建机上有超集，需人工反向回收"——**该判断已被推翻**：
> 把 `origin/main` 的 camera 源码取出来重跑同一套二进制差集，"ROM 独有"从 **11 条塌到 2 条假阳性**
> ⇒ **`origin/main` 就是那版 ROM 的源码**，本地只是落后。结论：先 `git fetch`，再谈反推。
>
> rebase 过程有 4 处冲突，其中一处值得单独记：
> **远端新增了静态照片降噪，JPEG 的输入源已从 `rgb` 变成 `rgbStill`**（`Session.cpp:1072` 的降噪副本）。
> 所以合并时既不能"以我们为准"（会丢掉降噪），也不能"以远端为准"（会丢掉方向修复）——
> 两边的语义都对，必须合成 `deliverJpeg(rgbStill, …, jpegQuality, jpegOrientation)`。
>
> 合并后复跑两套自测：**55 + 19 = 74 项，0 失败**。
> 部署与构建的完整方案见工作区 `docs/build/构建机方案建议-2026-09-19.md`。

---

## 一、结论（先看这一段）

**根因是一条，而且是硬缺陷、可复现的：JPEG 交付路径里完全没有"方向"这个概念。**

`Session::deliverJpeg()`（`device/huawei/gaokun3/camera/Session.cpp`）**从来没有读过
`ANDROID_JPEG_ORIENTATION`**，既不旋转像素、也不写 EXIF 的 Orientation 标记。它唯一的
"方向来源"是传入的 `dstW/dstH`，而那是**传感器坐标系**里的尺寸、与朝向无关。

后果是确定的、与硬件无关：

> 无论用户怎么拿机器、无论应用算出多大的校正角，交付出去的 JPEG **恒等于传感器原始读出**。

前摄和后摄走的是**同一份代码、同一个函数**，所以症状必然一致 —— 这正是"两台都歪、而且歪
的角度看起来一样"的原因。

**次因（同一批，属隐性缺陷）**：`Device::init()` 把 libcamera 的 `properties::Rotation`
**直接当成** `ANDROID_SENSOR_ORIENTATION` 用了，缺 `(360 - x) % 360` 的换算。这两个量的
方向约定**相反**（下面第二节有出处）。它在本机当前是隐性的（现值为 0 / 180，互为反数），
但只要按第六节把设备树的值标定成 90 或 270，**立刻会差 180°** —— 所以必须一起修。

**修完之后，照片方向就完全取决于设备树里那两个 `rotation` 值**，而它们恰恰是猜的
（后摄那行自己就标着 `/* FIXME */`）。所以本文第六节给出了实机标定步骤 ——
**这是让"照片方向正确"真正落地的最后一环**。

---

## 二、完整的方向链条与出处

先把四个"角度"摆清楚。它们分布在四个不同的层次上，**约定并不统一，其中两个是反向的**：

| # | 名字 | 位置 | 方向约定 | 含义 |
|---|---|---|---|---|
| ① | 设备树 `rotation` | `dts/…-camera.dtsi` | **逆时针** | 传感器物理安装角 |
| ② | `properties::Rotation` | libcamera | **逆时针**（原样继承 ①） | 同上 |
| ③ | `ANDROID_SENSOR_ORIENTATION` | Android 元数据 | **顺时针** | 把画面转正所需的角 |
| ④ | `ANDROID_JPEG_ORIENTATION` | 每帧请求 | **顺时针** | 应用算好的 JPEG 校正角 |
| ⑤ | `libyuv::RotationMode` | libyuv | **顺时针** | 实际做旋转的算子 |

### ①→② 是通的，不是摆设（这条必须先确认）

曾经以为设备树里的 `rotation` 只是注释性质的东西（内核驱动不读它）—— **不是**。
链路是：

```
设备树 rotation 属性
  → 驱动 hi846.c / ov13b10.c 调用 v4l2_fwnode_device_parse() + v4l2_ctrl_new_fwnode_properties()
  → 注册只读控件 V4L2_CID_CAMERA_SENSOR_ROTATION（min == max == def）
  → libcamera CameraSensorLegacy::initProperties()
       properties_.set(properties::Rotation, 该控件默认值)
  → HAL Device::init()
```

两个驱动**都**调用了那两个函数（已在上游 `drivers/media/i2c/hi846.c` 与 `ov13b10.c` 里核对），
而本机内核是 **mainline v7.2-rc2**（`scripts/kernel-setup-resukisu.sh:13`），远新于引入这套
fwnode 属性的版本 ⇒ 设备树那两个值**确实会被消费**。
反过来说：libcamera 里 `V4L2_CID_ROTATION` 一次都没出现，只有
`V4L2_CID_CAMERA_SENSOR_ROTATION`——名字不同，别混。

### ②→③ 的约定是**相反**的（这是那条隐性 bug 的来源）

libcamera 自己的文档（`property_ids`）原文：

> The camera rotation property is then defined as the angular difference **in the
> counter-clockwise direction** between the camera reference system 'Rc' and the projected
> scene reference system 'Rp'.

内核 `Documentation/devicetree/bindings/media/video-interface-devices.yaml` 的 `rotation`
属性是**同一段文字**（libcamera 从这里继承）。而 Android 的 `ANDROID_SENSOR_ORIENTATION`
是**顺时针**校正角。

上游 libcamera 的 Android HAL 在 `src/android/camera_device.cpp` 里就是为这件事写的一行：

```c
/*
 * The Android orientation metadata specifies its rotation correction value in
 * clockwise direction whereas libcamera specifies the rotation property in
 * anticlockwise direction. ...
 */
const auto &rotation = properties.get(properties::Rotation);
if (rotation)
        orientation_ = (360 - *rotation) % 360;
```

**本 HAL 缺的就是这个 `(360 - x) % 360`。**

### ⑤ 是**同向**的（这条我一开始也搞错了，所以写进注释）

`include/libyuv/rotate.h` 原文把方向写死在注释和别名里了：

```c
kRotate90  = 90,   // Rotate 90 degrees clockwise.
kRotate270 = 270,  // Rotate 270 degrees clockwise.
kRotateClockwise = 90, kRotateCounterClockwise = 270,   // 历史别名
```

⇒ Android 的 90° **就是** `libyuv::kRotate90`，**不需要取反**。
⚠️ 别把 OpenCV / PIL 的直觉套过来：那些库的"旋转 90"是逆时针，照抄一遍会把前后摄一起转歪 180°。

---

## 三、根因（源码级证据）

### 3.1 主因：`deliverJpeg()` 里没有任何方向处理

修复前的函数签名就说明了一切：

```cpp
bool Session::deliverJpeg(const uint8_t *rgb, buffer_handle_t dst,
                          int32_t dstW, int32_t dstH, int32_t blobSize,
                          int quality);          /* ← 没有旋转参数 */
```

调用点（`onRequestCompleted`）：

```cpp
    ? deliverJpeg(rgb, pb.handle, pb.width, pb.height,
                  pb.blobSize, /*quality=*/90)   /* ← 质量也写死 */
```

- `ANDROID_JPEG_ORIENTATION`：全仓库搜不到（`CameraMetadata` 里那两条**请求键**一个都没被消费）。
- EXIF：`Session.cpp` 里没有 `jpeg_write_marker`、也没有 `libexif`；JPEG 里**根本没有
  APP1/Orientation 标记**。按 EXIF 规范，"没有 Orientation 标记"等价于 **1（正常方向）**
  ⇒ 所有看图程序都按"不用转"处理 ⇒ 显示的就是传感器原始朝向。

两件事叠加，结论就是：**交付的 JPEG 永远等于传感器原始读出，方向信息在 HAL 这一层被整个丢掉。**

### 3.2 次因：朝向换算缺反向

修复前（`Device.cpp`）：

```cpp
auto rot = props.get(libcamera::properties::Rotation);
facts_.orientation = rot ? *rot : 0;      /* ← 少了 (360 - x) % 360 */
```

`0` 和 `180` 互为反数，所以本机现在"看起来没错"；一旦是 90/270 就差 180°。

### 3.3 为什么"前后摄症状一致"

两台相机共用 `Session::deliverJpeg()` 这一条路径。传感器各自的安装角不同，但**被丢掉的动作相同**
——都是"把该转的角度丢掉"。用户看到的偏差角 = 各传感器自己的安装角，所以表现成"两台都歪、
看起来差不多"。这也解释了为什么**同一批证据里，预览与照片的表现可能不一致**：预览走的是
Android 框架的纹理变换（依赖 ③），照片走的是 HAL 的编码路径（依赖 ④）——**是两条独立的路径**。

---

## 四、改了什么

### 4.1 改动清单（5 个文件）

| 文件 | 改动 |
|---|---|
| `camera/Session.cpp` | 新增 `toLibyuvRotation()`；`deliverJpeg()` 增加 `jpegOrientation` 形参并在编码前用 `ARGBRotate` **真的转像素**；`onRequestCompleted()` 从本帧请求里解析 `ANDROID_JPEG_ORIENTATION` / `ANDROID_JPEG_QUALITY`（后者顺手夹到 1..100） |
| `camera/Session.h` | `deliverJpeg()` 声明同步 |
| `camera/Metadata.h/.cpp` | 新增 `requestEntryInt()`：从请求的 camera_metadata blob 里安全取整型条目（**类型必须匹配**：`ANDROID_JPEG_ORIENTATION` 是 int32、`ANDROID_JPEG_QUALITY` 是 byte，按错类型硬取会读出垃圾） |
| `camera/Device.cpp` | ① 补上 `(360 - ccw) % 360` 换算 + 非 90 倍数告警；② 加一个**仅供标定**的属性覆盖开关（见第六节）；③ 日志同时打出 libcamera 的原始角和换算后的角 |

### 4.2 为什么选"转像素"而不是"写 EXIF 的 Orientation"

HAL 的合同是"把这个角落到交付物上"，两个合法做法**二选一**：

- ① 旋转像素，交付物里不要再有方向标记；
- ② 不转像素，把这个值写进 EXIF `Orientation`
  （**上游 libcamera 的 Android HAL 走的就是这条**：`post_processor_jpeg.cpp` 里
  `exif.setOrientation(jpegOrientation)`）。

本 HAL 选 ①，理由是**它更强**：不依赖看图程序是否解析 EXIF。代价是每张照片多一次
ARGB 旋转（8 MP 量级，毫秒级；BLOB 流本来就声明了 200 ms 的 stall，吃得下）。

⚠️ **两者绝不能同时做**：又转像素又写 EXIF 的旋转值 = 转两次，反而歪。所以这里**不需要**
`libexif`，也**不要**事后补一个别样的标记（缺标记 ≡ Orientation 1，正是我们要的）。

### 4.3 两个容易踩的点（已处理）

- **90/270 必须交换宽高**：流的 `(dstW, dstH)` 是传感器坐标系里的尺寸，转完之后 JPEG 的实际
  尺寸是 `(dstH, dstW)`。Android 允许这样 —— BLOB 流只声明缓冲**字节数**，应用自己读 JPEG 头
  取真实尺寸。**不要**为了"跟流尺寸对上"把宽高改回去，那就等于没转。
- **目标 stride 必须用旋转后的宽度** `outW * 4`。沿用 `dstW * 4` 会让每一行都错位（画面斜切成条）。
  这条在纯数学的模型里看不出来，只能让数据真的流一遍 —— 所以下面有一套**真编译真运行**的自测专门盯它。

---

## 五、验证

### 5.1 源码级：`scripts/camera/test-jpeg-rotation.py`（55 项，全过）

它**直接解析 `.cpp`**（不是把代码再抄一遍），所以把方向改回去就会红：

- `A2b` `toLibyuvRotation` 的映射表必须是恒等的 `{0:0, 90:90, 180:180, 270:270}`
- `A2c` 表里不允许出现 `360-x` 这类反向换算
- `A4b` `ARGBRotate` 的第 4 个实参必须是 `outW*4`
- `A5b` `deliverJpeg` 调用点必须把 `jpegOrientation` 真的传下去
- `A7` `facts_.orientation = (360 - ccw) % 360` 必须存在，且老的 `rot ? *rot : 0` 必须消失
- `B*` 用"顺时针"的数学定义建模型走一遍：场景 → 传感器读出 → 元数据 → JPEG，
  并验证**修复前的行为恰好复现用户的症状**（`B3b`：安装角 270° 逆时针时，
  修复前看起来是顺时针 270° = **逆时针 90°**，与描述逐字吻合）

```bash
python3 scripts/camera/test-jpeg-rotation.py
```

### 5.2 运行级：`scripts/camera/test-jpeg-rotation.cc`（19 项，全过）

把同一段表达式**真编译真跑**，用严格按 libyuv 注释实现顺时针语义的**替身**，
验证调用形状（实参序、源/目标 stride、宽高互换）并逐像素反查。
覆盖 8×6 → 8×6 / 4×3 / 4×6 三种目标尺寸 × {0,90,180,270} 四种角度
—— **非正方形**是必须的，否则 stride 类错误会被掩盖。

```bash
g++ -std=c++20 -Wall -Wextra -Werror -O1 -o /tmp/tj \
    scripts/camera/test-jpeg-rotation.cc && /tmp/tj
```

### 5.3 真机验证（还没做）

1. 构建 ROM（`device.mk` 已引用本模块；`external/libcamera/generated/` 必须先就位，见 `camera/README.md`）。
2. `logcat -s CameraProvider` 里应看到两行：
   `… 就绪：阵列 … 朝向 N（libcamera rotation=M，逆时针）…`
   —— **M 是设备树的值，N 是换算后的 Android 值，两者应满足 `N == (360-M)%360`**。
3. 拍一张，`logcat | grep "JPEG "` 应看到
   `JPEG WxH（请求 w×h，旋转 R°）质量Q → n 字节`，其中 90/270 时 `W,H` 与请求的 `w,h` 互换。
4. 照片在**机身自然朝向**（横屏）下应当是正的。

---

## 六、设备树 `rotation`：**已实机标定（2026-09-19）**

### 6.1 实测结论

| 相机 | 安装角 φ | 需要的 `SENSOR_ORIENTATION` | **`rotation` 应为** | 改动前 | 判定 |
|---|---|---|---|---|---|
| 前摄 hi846（`internal/1`） | **270°** | 90° | **`<270>`** | `<0>` | ❌ 错 |
| 后摄 ov13b10（`internal/0`） | **0°** | 0° | **`<0>`** | `<180>` | ❌ 错 |

（φ = 设备处于自然朝向时，交付出来的照片看起来**顺时针**歪的度数；`rotation` 的数值**等于** φ，
因为 libcamera 的 rotation 就是**逆时针**校正角。`SENSOR_ORIENTATION = (360-φ) % 360`。）

**测量方法**（可用加速度计当参考系，不必改机重编 —— 详见工作区
`docs/camera-android-photo-orientation-measurement-2026-09-19.md`）：
读 `dumpsys sensorservice` 的加速度计拿到"世界的上"在设备坐标系里的方向，
再和照片里的重力线索（书竖着立在层板上、罐子立在地上）对照。
★ 差分技巧：**同一个物体**在两台相机里的姿态（那个装公仔的圆柱罐：前摄里**立着**、后摄里**躺着**）
⇒ 两台相差 90°，免掉"哪边是上"的主观判断。

⚠️ 两个把人带偏的坑，记在这里免得再犯：
1. **显示旋转 ≠ 设备物理朝向**：Aperture 把 UI 锁竖屏，于是设备明明是横屏，
   `DisplayDeviceInfo` 仍报 `1600 x 2560, rotation 0`。据它判断设备朝向会差 90°。
2. `settings get system user_rotation` 在自动旋转开着时恒为 0；要读 `dumpsys window displays` 的 `mRotation`。

### 6.2 已改的三处副本（B0 那个老毛病）

| 位置 | 状态 |
|---|---|
| `MateBookEgo-linux-kernel/dts/sc8280xp-huawei-gaokun3-camera.dtsi` | ✅ 前摄改为 `<270>`（该文件**只有前摄节点**） |
| 本仓 `patches/0032-*.patch` | ✅ 后摄改为 `<0>`（后摄节点是这个补丁加的） |
| `/home/ms/kernel-src-android/arch/arm64/boot/dts/qcom/…-camera.dtsi` | ✅ 已同步（WSL 里**真正编 DTB** 的那份） |

改 `rotation` 只影响 **boot 镜像**（内核/DTB 是 `BoardConfig.mk` 的 `TARGET_PREBUILT_KERNEL`），
**不需要重编 ROM** —— 但见下一条。

### 6.3 ⚠️ 与 HAL 修复的**部署顺序**（颠倒会更糟）

| 运行中的 HAL | 前摄 `rotation` | 后摄 |
|---|---|---|
| 老 HAL（θ = `rotation` 直接抄，即当前 ROM） | **90**（预览立刻正，照片仍不转） | 0 |
| **新 HAL**（θ = `(360-rotation)%360`，本文的修复） | **270** | **0** |

本文按**新 HAL** 给值。DT 在 boot 镜像、HAL 在 super.img，两者可以分别刷 ⇒ 存在这个中间态：
**要么一起刷，要么先按"老 HAL"那一列临时填 90。**
（后摄的 0 是自反的，两种 HAL 下都对；只有前摄要看这个表。）

### 6.4 顺带可查的一件事：前摄是否镜像

另一处记录（Ubuntu 侧的 GStreamer 桥）写过"**hi846 的 readout + SoftISP 输出的是镜像画面
（自拍视角）**"，因此那边的管线里加了 `videoflip`。**Android 的前摄 JPEG 不应该镜像**
（镜像只该出现在预览里，由应用自己做）。这不是旋转问题、你没报它，但如果标定时顺手看到了，
拍一行有字的纸即可分辨：字是反的 ⇒ 前摄缺一次水平翻转，那是另一处改动。

---

## 七、其余建议（本次未改，按需处理）

1. **`ANDROID_JPEG_SIZE` 没回填**。结果元数据里现在没有它。上游 libcamera 会填
   （`resultMetadata->addEntry(ANDROID_JPEG_SIZE, jpeg_size)`）。框架多数情况下靠
   `CameraBlob` 拿长度，但补上更规范。
2. **没有声明 `ANDROID_REQUEST_AVAILABLE_REQUEST_KEYS` / `_RESULT_KEYS`**。
   `buildCharacteristics()` 里目前完全没这两条。相机现在能用，但按 `INFO_SUPPORTED_HARDWARE_LEVEL_LIMITED`
   的要求应当声明；顺带把 `ANDROID_JPEG_ORIENTATION` / `ANDROID_JPEG_QUALITY` / `ANDROID_JPEG_SIZE`
   列进去，语义更清楚。
3. **静态元数据里的 `ANDROID_JPEG_QUALITY` 默认 90 现在是活的**：请求里没带该键时我们用 90，
   带上就用应用的。若哪天要改默认值，记得两边一致。
4. **`buildResult()` 会把请求的键原样回显**，所以 `ANDROID_JPEG_ORIENTATION` 自动出现在结果里
   （与上游行为一致），不需要额外代码 —— 但**不要**因此在 EXIF 里再写一次角度（见 4.2）。
5. **`deliver()`（预览/YUV 路径）刻意不旋转**：预览的方向由框架按 `SENSOR_ORIENTATION` 做
   纹理变换，HAL 这里转了就是双重校正。改这一段之前先想清楚这一点。

---

## 八、改动文件与自测

```
device/huawei/gaokun3/camera/Device.cpp       朝向换算 + 标定开关
device/huawei/gaokun3/camera/Metadata.cpp     requestEntryInt()
device/huawei/gaokun3/camera/Metadata.h
device/huawei/gaokun3/camera/Session.cpp      JPEG 旋转（主修复）
device/huawei/gaokun3/camera/Session.h
scripts/camera/test-jpeg-rotation.py          源码级断言（55 项）
scripts/camera/test-jpeg-rotation.cc          运行级自测（19 项）
docs/TODO.md                                  `rotation` FIXME 条目指向本文
~~docs/camera-photo-rotation-2026-09-19.patch~~  （2026-09-24 删：代码已在合并 PR #6 时改过，这份快照会把修复回退；
                                               构建机同步现在用 scripts/sync-device-tree.sh）
```

> ⚠️（已过时，见上）补丁文件是给构建机用的：本仓的 `~/crdroid/device/huawei/gaokun3` 与本仓靠人手拷贝
> （见 `docs/TODO.md` B0，已经因此付过四次账）。`git apply` 一次即可，不用再逐文件对齐。

**未做的一件事要说清楚**：这里没有 AOSP 源码树与工具链，**HAL 本身没有经过一次真实编译**。
上面两套自测覆盖了改动的逻辑（源码级断言 + 真编译真运行，`-Wall -Wextra -Werror -std=c++20`），
但 Soong 侧的链接与真机行为尚未验证。
第一次构建时请留意 `Metadata.h` 的签名、`<system/camera_metadata_tags.h>` 的包含路径，
以及 `<sys/system_properties.h>` 里的 `PROP_VALUE_MAX`。

---

## 七、2026-09-20 实机验证更新（推翻 6.3 的预测）

HAL 已在本机构建机上**真实编译**（此前只是源码级 + 逻辑级自测），热替换上机后做了端到端实测：

```
build completed successfully   android.hardware.camera.provider-service.gaokun3
                               136,704 字节  sha256 4acf8ecc89eac7e8…
```

### 7.1 修复被证实生效

| 证据 | 数值 |
|---|---|
| logcat（新代码独有的诊断串） | `朝向 0（libcamera rotation=0，逆时针）` |
| HAL 交付日志 | `JPEG 720x1280（请求 1280x720，旋转 90°）质量95` |
| 旧 HAL 同机同景 | `JPEG 2560x1440`（从不旋转） |

⇒ **HAL 现在真的按请求角转像素了**，且 90° 方向正确。

### 7.2 前摄照片为正 ⇒ 6.3 的"两台都 270"预测**错误**

设备物理横屏（加速度计 `(9.53,-0.03,2.11)`，重力沿 +X），应用请求 90°，交付照片
（`front-probe-001834.jpg`）**是正的**：置物架物品坐在层板上、书平摞、玩偶架在桌子上方、桌面在下方。

⇒ 前摄 **θ=0 正确** ⇒ 设备树 `rotation = <0>`（ROM 原值就对）。
**6.3 的"两台都先试 270"作废**；09-19 那次"加速度计+差分"标定把前摄标成 270 是误标
（被"显示旋转 ≠ 物理朝向"坑带偏，且差分法混入了两台各自的安装角）。
三处设备树已纠正为 **前摄 0 / 后摄 0**。

### 7.3 后摄尚待一张有结构的照片

后摄当拍到的画面是**完全均匀的亮场**（p1=p99=252，零结构 —— 镜头贴近桌面/墙，失焦成一片白），
判读不了。后摄两轮分析一致指向 θ=0（⇒ `rotation=0`，已就位），但**以拍到有结构的照片为准**。

### 7.4 热替换的可行解（/vendor 100% 满 + ETXTBSY）

> ⚠️★ 2026-09-24 补（合并 PR #6 时实测踩到）：**换第二版时必须 `stop` → `umount`（确认卸掉）→ 再 push → `mount --bind` → `start`。**
> 直接往已被 bind 的 `/data/local/tmp/hal-new.bin` 上 `adb push`，push 会换一个新 inode，而挂载点仍指着
> 被删掉的旧 inode（tombstone 里是 `Executable: … (deleted)`）；provider 一重启就在
> `GraphicBufferMapper()` 里 abort：`gralloc-mapper is missing`（AIDL 的 Gralloc5 没加载上，
> 退回 HIDL 而本机 hwservicemanager 是死的）。provider 在跑时 `umount` 会因目标忙静默失败 —— 所以要先 stop。

`/vendor` 257 MB **用满（0 可用）**，且 provider 正运行该二进制 ⇒ 直接 push（权限）、
cp（ETXTBSY）、rename（ENOSPC）**三条路都实测失败**。唯一可行解是 **bind mount 单文件**
（不占 /vendor 空间、不动运行中的 inode）：

```bash
adb push <新二进制> /data/local/tmp/hal-new.bin
adb shell su -c 'chmod 755 /data/local/tmp/hal-new.bin'
adb shell su -c 'chcon u:object_r:hal_camera_default_exec:s0 /data/local/tmp/hal-new.bin'
adb shell su -c 'mount --bind /data/local/tmp/hal-new.bin /vendor/bin/hw/android.hardware.camera.provider-service.gaokun3'
adb shell su -c 'setprop ctl.restart vendor.camera-provider-gaokun3'
adb shell su -c 'sha256sum /vendor/bin/hw/android.hardware.camera.provider-service.gaokun3'   # ★ 必须核对
```

⚠️ **bind mount 重启即失效**；要持久化需做 KernelSU 模块（设备上有 meta-overlayfs）或刷 super。
工具化：`scripts/hal-hotswap.sh`（工作区，已改 bind-mount 法）。

### 7.5 首次真实编译暴露的三处问题（都已修，见 commit c474de6）

1. `<libcamera/libcamera.h>` ~~上游/AOSP 都没有 —— 当年构建机手工放的，已重建入仓~~
   ⚠️ 2026-09-24 更正（合并 PR #6 时）：上游**会生成**它（`include/libcamera/meson.build:127-135`，
   `custom_target('gen-header')`），构建机 `generated/include/libcamera/libcamera.h` 就是生成的那份
   （文件头 "auto-generated"，比手写的多 7 个头）。手写副本已删，改由
   `scripts/camera/gen-libcamera-sources.sh` 生成并断言。
2. `tracepoints.h` 是 meson `custom_target('tp_header')` 产物，生成清单漏了它
   （编到 90% 才报 file not found）。
3. `Session.cpp` 重复包含无 guard 的 `camera_metadata_tags.h` ⇒ 20 个 redefinition。
