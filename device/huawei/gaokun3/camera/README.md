# gaokun3相机HAL（AIDL，基于`libcamera`）

本模块将`libcamera`接入Android相机框架（实现`android.hardware.camera.provider-V3`）。画面来自`libcamera`的`qcom-ife`流水线：IFE完成去马赛克、白平衡、色彩矩阵与伽马，输出全范围BT.601的NV12，另可配传感器原始帧（RAW）供相位对焦。相机子系统的端到端架构、板级供电与传感器标定详见[`docs/dev/camera.md`](../../../../docs/dev/camera.md)。

## 一、为何采用自研AIDL HAL

1. **HIDL `provider@2.4-legacy`不可行**：Android 16/17已移除`hwservicemanager`（`/system/bin/hwservicemanager`为悬空符号链接），且FCM兼容性矩阵要求`camera.provider`必须为`format="aidl"`。
2. **AOSP `ExternalCameraProvider`不可行**：它只能直接读取V4L2节点给出的YUYV或MJPEG，而本机的传感器原始数据要经`libcamera`为IFE编程、由IPA做曝光、白平衡与对焦统计后才能成像。
3. **直接实现AIDL接口**：`CameraMetadata.aidl`底层即为`libcamera_metadata`序列化的`camera_metadata_t`缓冲区；裁剪与缩放使用AOSP内置`libyuv`，静态拍照JPEG编码使用`libjpeg-turbo`，EXIF由`Exif.cpp`自行生成（vendor进程不能链接平台的`libexif`）。

## 二、构建与依赖

- **`libcamera`同树Soong构建**：由于Android NDK的`std::__ndk1`与AOSP平台的`std::__1` C++ ABI不兼容，`libcamera`通过[`patches/libcamera/libcamera-Android.bp`](../../../../patches/libcamera/README.md)在AOSP树内编译为`libcamera_base_gk3`、`libcamera_gk3`与`qcom-ife`流水线的IPA模块（安装于`/vendor/lib64/libcamera/ipa/`）。
- **调优配置**：`ipa-data/qcom-ife/`下的`ov13b10.yaml`、`hi846.yaml`与`uncalibrated.yaml`安装至`/vendor/etc/libcamera/ipa/qcom-ife/`。
- **流水线能力**：本模块要求流水线给出NV12；不给时不探测帧率与3A控制，配置流时报错。其余依赖的流水线行为（两路输出、`ScalerCrop`、`FocusFoM`、时间戳、出错恢复）见`docs/dev/camera.md`。

## 三、硬件ISP路径

- **单路**：向流水线要一路与传感器阵列同宽高比、刚好覆盖每一路流的NV12画面（`Session::hwIspFrameSize()`），传感器模式与视场由流水线选，IFE缩放；各路流由CPU从这幅画面裁剪、缩放。帧率按探测到的传感器模式声明（后摄合并模式60 fps）。
- **两路（拍照模式）**：JPEG比其余各路大得多时，向流水线要两路NV12。显示路径由IFE缩放器缩小，给预览、录像、YUV流与对焦；FULL路径给JPEG，只有带JPEG的请求才带它的缓冲，IFE只在这些帧写出全尺寸画面。JPEG（含拍照降噪与锐化）在单独的线程上从FULL缓冲编码，预览、录像与结果元数据照常先交付，不因拍照断流。两路视场相同；流水线不支持两路时退回单路。
- **数码变焦**：两路且流水线支持`ScalerCrop`时交给IFE显示路径（只缩小不放大，超出它能放大的倍数时CPU按元数据里实际显示的区域再裁、再放大），照片的变焦仍由HAL在FULL上裁；否则显示路径上的裁剪比预览小（要放大）的请求也带FULL，预览等非JPEG流改从FULL裁剪、缩小。
- **对焦**：后摄`dw9714`马达由HAL直接驱动（见`Lens.h`）。全尺寸模式下按RAW里的相位像素对焦，再用对比度精调；对比度分数在增益不高时取AF区域亮度的梯度能量，暗处取IFE对焦统计的`FocusFoM`（见`AutoFocus.h`）。
- **画质**：IFE不做锐化与降噪。预览、录像与照片在输出分辨率上做亮度锐化（`EDGE_MODE`）；照片另做亮度ε滤波与色度模糊，强度按噪声估计（`Sharpen.cpp`）；预览、录像在增益高时只模糊色度。

## 四、闪光灯与手电筒

- **硬件节点**：机背LED连接在PM8350C闪光模块的1+4通道，由内核`patches/kernel/dts/0006`暴露为`/sys/class/leds/white:flash`（`ueventd.gaokun3.rc`设置权限为`0664 root camera`，SELinux标为`sysfs_leds`）。
- **手电筒模式**：`Device::setTorchMode`直接写入`brightness = max_brightness`；当相机存在活跃拍摄会话时返回`CAMERA_IN_USE`，并在`open()`与会话关闭时通过provider回调更新`TorchModeStatus`。
- **预闪常亮拍照**：因PMIC LED无硬件帧曝光同步信号，收到`AE_PRECAPTURE_TRIGGER_START`时点亮LED并上报`AE_STATE_PRECAPTURE`供AGC收敛，通过`firedPending_`统计在途拍照请求，直至静态JPEG帧完成采集后再熄灭LED。`ON_AUTO_FLASH`模式基于交付帧采样平均亮度（`kDarkLuma`）与模拟增益启发式判断是否需要开灯。

## 五、关键实现约定

- **固定相机ID顺序**：`libcamera`枚举顺序随内核media子设备注册先后浮动，`Provider::init`显式按`Location`排序（后摄`BACK`固定为`0`，前摄`FRONT`固定为`1`）。
- **优先从FMQ读取请求设置**：`AidlCamera3Device`默认将每帧`settings`写入`requestQueue_`（此时`fmqSettingsSize > 0`且内联`settings`为空），`Session::processCaptureRequest`同时支持从FMQ与内联字段读取参数。
- **缓冲句柄缓存与免校验导入**：按`StreamBuffer.aidl`契约维护`(streamId, bufferId) -> buffer_handle_t`缓存并处理`cachesToRemove`；使用`importBufferNoValidate()`导入Gralloc缓冲以避免框架私有usage位引发`BAD_BUFFER`校验失败。
- **调试属性**：`debug.gaokun3.camera.af.hold <位置>`把对焦马达钉在某个位置（标定用）；`debug.gaokun3.camera.sharpen`、`debug.gaokun3.camera.denoise`乘在锐化强度与照片降噪门限上（0为关）。
