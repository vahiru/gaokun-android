# denoise/ —— 麦克风降噪服务gk3denoised

在CDSP的NPU（HTP v68）上跑DeepFilterNet3（48 kHz，每跳480帧），给音频HAL的采集钩子（`micarray/`）用：
通话（VOICE_COMMUNICATION，在钩子的回声消除之后，单声道）默认降噪，录音（MIC、DEFAULT）和录像（CAMCORDER）
场景开关打开时降噪（两路）。

钩子经`/dev/socket/gk3denoise`（`SOCK_SEQPACKET`，协议`micarray/denoise_protocol.h`）把一块块的跳发过来，
服务按连接、按声道保留滤波器的状态，返回同样多的跳，固定晚3跳（30毫秒）。

## 组成

| 文件 | 说明 |
|---|---|
| `service/` | 服务本身（Apache-2.0）：引擎（CPU上做分析、特征、掩码、深度滤波和合成，网络交给QNN）、服务端、`socid_override.c` |
| `model/` | 编好的QNN上下文二进制（入库），来历和验证见`model/README.md` |
| `prebuilt/` | 构建前由`scripts/fetch-denoise-blobs.py`取的东西（不入库）：QNN运行库和头文件、CDSP外壳和DSP侧C++库、FastRPC源码 |
| `Android.bp` | `gk3denoised`，和用FastRPC源码编的`libcdsprpc`（装在`/vendor/lib64/gk3denoise/`） |
| `gk3denoise.rc` | init服务`vendor.gk3denoise`，user system，套接字0660 system:audio |
| `../sepolicy/gk3denoise.te` | 服务的域；音频HAL连它的规则 |
| `../ueventd.gaokun3.rc` | `/dev/fastrpc-cdsp`给system |
| `../device.mk` | 装服务、库、DSP侧文件、模型和许可文件 |
| 内核`patches/0205` | CDSP的FastRPC上下文标成dma-coherent，否则在CDSP上加载skel几乎每次都失败（AEE_EUNABLETOLOAD，与时序有关） |

## 几个不显然的地方

- **SoC识别**：QNN的Android库只认它知道的SoC，启动时用libc的`open()`读`/sys/devices/soc0/soc_id`，本机是449（SC8280XP），
  它不认，直接失败。服务在自己的可执行文件里定义并导出`open`一族（`socid_override.c`，`Android.bp`的`--export-dynamic-symbol`），
  只对这一个文件回答415（SM8350，同为v68），别的照常走系统调用。只影响这个进程，不用挂载。
- **预增益**：网络前固定加18 dB、输出再除回去（`--pre-gain-db`）。量化模型在人声低于约−40 dBFS时会把人声也压掉，
  内置麦的人声大约在−55到−15 dBFS之间；18 dB把它挪到模型表现好的范围（实测数据在gaokun-workspace的
  experiments/denoise-hal-20261103/lowlevel/NOTES.md）。
- **CDSP外壳的位置**：libcdsprpc只在编译时写进去的路径里找外壳（`DEFAULT_DSP_SEARCH_PATHS`，再加`/vendor/dsp`），
  不看`ADSP_LIBRARY_PATH`，所以`Android.bp`里写的是`/vendor/dsp/gk3denoise`。它先试`/dev/fastrpc-cdsp-secure`，
  打不开（root 0600，策略里只dontaudit）再用`/dev/fastrpc-cdsp`。
- **出错时**：模型加载失败时服务照常监听，对开流回答“不可用”，钩子就不降噪；服务每30秒自己再试（`--retry-s`），一次加载卡住超过60秒就退出，由init重启（`--load-timeout-s`）。
- **功耗档**：有连接时投HTP的low档，最后一个连接断开时撤掉。

## 验证到哪一步

- 同一份源码用NDK编出来，以KernelSU模块gk3denoise在本机跑过（#15内核，强制模式，服务在自己的域里，没有拒绝记录）：
  输出和离线参考逐位相同；音频HAL经测试shim连上服务，每85毫秒一块往返约14毫秒。
- SELinux策略在ROM树外按`lineage_gaokun3-cp2a-userdebug`的方式整套编过：neverallow、vendor策略只用公开的平台类型、
  file_contexts和sepolicy_tests都通过。
- ⬜ 在ROM树里还没编过：`Android.bp`、`gk3denoise.rc`和`device.mk`的这一段都是照模块的实测写的。
