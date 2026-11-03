# 相机诊断工具（V4L2 / media-controller 层）

设备上没有 `media-ctl` / `v4l2-ctl`，也没有相机 HAL。这两个小工具是 2026-09-11
把前摄通路打通时写的，**全部结构体来自内核树的 uapi 头，不手抄偏移**。

| 工具 | 干什么 |
|---|---|
| `mediatopo` | `media-ctl -p` 的最小替代：转储 `/dev/media0` 的实体、pad、链路及启用状态 |
| `camtest` | 接链 → 沿链传格式 → S_FMT → 抓帧；四轮对照（默认 / 满增益 / 彩条 / 分辨率图案）|

编译：`bash scripts/camera/build.sh ~/gk3-kernel`（构建机上），产物静态链接，
`adb push` 到 `/data/local/tmp/` 直接跑（要 root）。

## ⚠️★★ 用之前必须知道的三件事

1. ⚠️★ **只要 titan_top GDSC 真的塌缩过，再上电就会失败。**
   （⚠️ 本节原先写的是"开机后只有第一次能成功"——**那个模型是错的**，
   2026-09-12 用寄存器实测改正，见 `docs/stage4-findings.md` #83。）
   * **连着跑**不会失败：GDSC 来不及塌缩，后面几次根本不需要上电，实测 6/6 全过。
   * **隔一会儿再跑**必失败：`titan_top_gdsc status stuck at 'off'`
     （`gdsc.c:185` 的 WARN），STREAMON 返回 `-110`。
   失败后 GDSC 停在"请求了上电但没上电"的半状态（PWR_ON=0 而 SW_COLLAPSE=0），
   genpd 同时锁死 `runtime_error`，**之后所有报错都是假的**（一律 `-EINVAL`）。
   解绑重绑救不回来（`bind` 直接失败），**只有重启**。
   所以 `camtest` 把所有对照条件塞进【同一次流】里。

   ⚠️⚠️ **别用 `devmem` 去读 camcc 的寄存器来排查这个** ——
   camcc 一旦 runtime-suspend，它的寄存器块没时钟，**读**也会触发总线
   external abort 导致内核静默死亡（2026-09-12 这么弄挂过一次，要人按电源键）。
   非读不可时，先 `echo on > /sys/devices/platform/soc@0/ad00000.clock-controller/power/control`
   把 camcc 钉住。

2. **video 节点的像素格式必须与传感器总线码同族**（RDI 是裸转储，不转换）。
   选错了 STREAMON 报 `EPIPE`，错误信息完全不提"格式"。映射表出处
   `drivers/media/platform/qcom/camss/camss-vfe.c:59`，已抄进 `camtest.c` 的 `kBusToPix`。

3. **camss 用 multiplanar API。** 用单平面 `V4L2_BUF_TYPE_VIDEO_CAPTURE` 查它，
   `ENUM_FMT` 返回空、`S_FMT` 报 `EINVAL`，看起来像节点坏了 —— 其实只是问错了接口。

## 通路（前摄 hi846，实测）

```
hi846 2-0020 :0 → msm_csiphy3 :0    [DT 硬连，不可变]
msm_csiphy3  :1 → msm_csid0   :0    ← camtest 启用
msm_csid0    :1 → msm_vfe0_rdi0 :0  ← camtest 启用
msm_vfe0_rdi0:1 → msm_vfe0_video0   [不可变]  ⇒ /dev/video0（或被 Venus 挤开后的编号）
```

传感器只报一种总线码 `0x300e`（SGBRG10_1X10），尺寸 1280x720 / 1632x1224 /
3264x1836 / 3264x2448 —— **没有 640x480**。对应像素格式 `pGAA`（SGBRG10P）。

## 判据

`camtest` 第三轮打开传感器彩条（Test Pattern 2），从原始拜耳解出的序列应为
**黄 青 绿 品 红 蓝**，R/G/B 满量程 1023 或 0，上下行逐像素差 0 —— 与环境光无关。
2026-09-11 实测逐字命中，见 #81。

## 后摄色彩矫正矩阵：`gen-ov13b10-ccm.py`

生成`device/huawei/gaokun3/camera/ipa-data/softisp/ov13b10.yaml`。矩阵取自华为Windows相机驱动为后摄这款模组做的调校
（200.0.10.0驱动包里`qccamrearsensor_extension8280.cab`中的`com.qti.tuned.ofilm_ov13b10.bin`），
不是每台机器单独的标定（那个在模组的EEPROM里）。只用不开闪光灯时亮光那一档（lux index 1–180），因为libcamera的Ccm只按色温插值。
文件格式和取值方法写在脚本开头。

    python3 scripts/camera/gen-ov13b10-ccm.py com.qti.tuned.ofilm_ov13b10.bin > device/huawei/gaokun3/camera/ipa-data/softisp/ov13b10.yaml

脚本会检查数据位置、区间数和每行之和为1，驱动换版本导致结构变化时会直接报错。前摄hi846在Windows驱动里用的是高通的默认调校，没有可借的矩阵。
