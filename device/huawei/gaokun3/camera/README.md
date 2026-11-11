# gaokun3相机HAL（AIDL，基于`libcamera`）

本模块将`libcamera`接入Android相机框架（实现`android.hardware.camera.provider-V3`），按`libcamera`给出的输出格式选择路径：硬件ISP（`qcom-ife`流水线，IFE输出NV12，另可配RAW流）或`simple`流水线与软件ISP（输出RGB888）。相机子系统的端到端架构、板级供电与传感器标定详见[`docs/dev/camera.md`](../../../../docs/dev/camera.md)。

## 一、为何采用自研AIDL HAL

1. **HIDL `provider@2.4-legacy`不可行**：Android 16/17已移除`hwservicemanager`（`/system/bin/hwservicemanager`为悬空符号链接），且FCM兼容性矩阵要求`camera.provider`必须为`format="aidl"`。
2. **V4L2垫片 + AOSP `ExternalCameraProvider`不可行**：`libcamera`的CPU软件ISP仅输出RGB格式（`debayer_cpu.cpp:436-441`），而`ExternalCameraProvider`要求节点提供YUYV或MJPEG。
3. **直接实现AIDL接口**：`CameraMetadata.aidl`底层即为`libcamera_metadata`序列化的`camera_metadata_t`缓冲区；色彩空间转换使用AOSP内置`libyuv`（`RGB24ToJ420`全范围BT.601），静态拍照JPEG编码使用`libjpeg-turbo`。

## 二、构建与依赖

- **`libcamera`同树Soong构建**：由于Android NDK的`std::__ndk1`与AOSP平台的`std::__1` C++ ABI不兼容，`libcamera`通过[`patches/libcamera/libcamera-Android.bp`](../../../../patches/libcamera/README.md)在AOSP树内编译为`libcamera_base_gk3`、`libcamera_gk3`与`libcamera_ipa_softisp_gk3`（安装于`/vendor/lib64/libcamera/ipa/`）。
- **调优配置**：`hi846.yaml`、`ov13b10.yaml`与`uncalibrated.yaml`安装至`/vendor/etc/libcamera/ipa/softisp/`。
- **硬件ISP路径**：`qcom-ife`流水线与IPA（及其调优配置）尚未接入同树构建；本模块在`libcamera`给出NV12输出时走该路径，传感器模式与视场由流水线选择。JPEG比其余各路大得多时（拍照模式），向流水线要两路NV12：显示路径由IFE缩放器缩小，给预览、录像与YUV流；FULL路径给JPEG，只有带JPEG的请求才带它的缓冲，IFE只在这些帧写出全尺寸画面；数码变焦后显示路径上的裁剪比预览小（要放大）时，请求也带FULL，预览等非JPEG流改从FULL裁剪、缩小。两路视场相同；流水线不支持两路时（如双IFE）退回单路，由CPU从同一幅画面裁剪、缩放出各路流。

## 三、闪光灯与手电筒

- **硬件节点**：机背LED连接在PM8350C闪光模块的1+4通道，由内核`patches/kernel/dts/0006`暴露为`/sys/class/leds/white:flash`（`ueventd.gaokun3.rc`设置权限为`0664 root camera`，SELinux标为`sysfs_leds`）。
- **手电筒模式**：`Device::setTorchMode`直接写入`brightness = max_brightness`；当相机存在活跃拍摄会话时返回`CAMERA_IN_USE`，并在`open()`与会话关闭时通过provider回调更新`TorchModeStatus`。
- **预闪常亮拍照**：因PMIC LED无硬件帧曝光同步信号，收到`AE_PRECAPTURE_TRIGGER_START`时点亮LED并上报`AE_STATE_PRECAPTURE`供软件ISP的AGC收敛，通过`firedPending_`统计在途拍照请求，直至静态JPEG帧完成采集后再熄灭LED。`ON_AUTO_FLASH`模式基于交付帧采样平均亮度（`kDarkLuma`）启发式判断是否需要开灯。

## 四、关键实现约定

- **固定相机ID顺序**：`libcamera`枚举顺序随内核media子设备注册先后浮动，`Provider::init`显式按`Location`排序（后摄`BACK`固定为`0`，前摄`FRONT`固定为`1`）。
- **优先从FMQ读取请求设置**：`AidlCamera3Device`默认将每帧`settings`写入`requestQueue_`（此时`fmqSettingsSize > 0`且内联`settings`为空），`Session::processCaptureRequest`同时支持从FMQ与内联字段读取参数。
- **缓冲句柄缓存与免校验导入**：按`StreamBuffer.aidl`契约维护`(streamId, bufferId) -> buffer_handle_t`缓存并处理`cachesToRemove`；使用`importBufferNoValidate()`导入Gralloc缓冲以避免框架私有usage位引发`BAD_BUFFER`校验失败。
