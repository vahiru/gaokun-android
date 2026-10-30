# R17基线上的指纹接口候选与启动对照

独立候选目录 `/home/vahiru/hos-r19-fingerprint-kernel`，基线目录
`/home/vahiru/hos-r17-kernel-thermal`。内核EFI和匹配指纹探针模块均已构建并完成审计/本地归档。候选已通过独立一次性入口启动HyperOS；随后为传感器问题回到R18原R17内核做对照。默认启动项和super/userdata分区未改。

## 已独立回读的摘要

| 项目 | R17与候选 |
|---|---|
| .config | 均为bf30f6266230d568732647d09293b5d7f6447d67dbed97fc8a475daa2ba57976 |
| gaokun3 DTB | 均为a1f7abfcaaf2e6e2f3a28b94d631b101ee09ac06b847325c679287e27de215c6 |
| Himax核心源码 | 均为38bd1198688e5d53c1e036489da03c55c560fd88292227717b472e9d622ee502 |
| ADC_TM5源码 | 均为315f5fdea7485e7e19cb4826b745b8e93a41df280d031b9cfc7322894f621cc8 |
| 原R17 EFI | d22e364af4fb403a65e03b1545e847765865c5c28012473779dd6b42cf3b60bc |
| 候选EFI | 4b846caac792764a734eee43cb271b299f19a050ac3990c8b7f0118e40a5cdd6 |

补丁0050只涉及qcom_scm.c及头文件；应用结果与本轮已实测实验树对应两文件一致。候选保留R17的ADC_TM5内建、触摸/物理尺寸及USB相关改动，不能以旧crDroid实验EFI替代此构建。

## 构建问题与解决

`vmlinuz.efi`目标生成了新的vmlinux.symvers，但顶层Module.symvers仍旧，首次外部模块MODPOST因此报9个QSEECOM/TZMEM导出缺失。核对CONFIG_TRIM_UNUSED_KSYMS未启用，实际ELF与vmlinux.symvers均已有导出。通过标准make modules刷新顶层清单，再构建外部模块成功；没有手填符号或放宽modpost。

随后审计脚本引用了不存在的ucsi_glink.o路径而停止，这是审计脚本错误，不是编译失败。正在按实际基线清单修正路径并继续剩余审计；未重复复制/覆盖候选树或修改原基线。旧失败日志保留在远端hos-r19-fingerprint-kernel-audit。

## 验收范围

构建通过不是HyperOS真机验收。0050影响共享SCM DMA及listener处理，详见
[兼容审计](hyperos-r17-fingerprint-kernel-audit.md)和[整合计划](hyperos-fingerprint-integration-plan.md)。冷启动后的UEFI只读、温控、USB、网络/ADB、IMU和指纹空载回归尚未执行。真实手指触发、图像质量、录入和Android HAL也未完成。

## 最终离线审计

基线111644条目before/after一致，手写源码仅两个SCM文件；真实EFI解压Image的嵌入配置与R17一致。匹配模块全部33个未定义导入都有候选真实导出，SHA256 dbce66b694d1c000253dc2ecdf642d27a39aae024fd3f7e83787d237baf27247。

initramfs仅dev/dev/console/root三个mtime变化，内容和其他字段不变。六个USB对象属于本次标准make modules重编的未部署loadable模块：五项仅strtab变化，ftdi_sio另有结构字段偏移机器码变化（旧对象早于当前autoconf）；不声称它们全部代码相同。这些模块未打包部署，modules阶段前后候选EFI哈希一致。实际UCSI/Himax/ADC、其他受保护USB对象与DTB/config按hash保持。

## ESP与冷启动

首次安装因ESP仅剩约12MiB而在写入前拒绝。R15独立内核及唯一引用它的启动项已拉回cache/hos-r19-fingerprint-deploy/archive-r15，按原SHA验证后归档移除；其他全部ESP文件内容保持，包括默认Android B、R16/R17/R18及当前cr实验入口。随后只添加r19-fingerprint/vmlinuz.efi和gaokun3-hos-hyperos-r19-fp.conf，全部既有文件回读保持。super_hos整块仍为R18 ed290c3c83b4f0f88ec6d1a0a86c1c10f07a210a864cb260db85c373dcfc6276。

候选boot ID 9199749b-07f9-4c17-b273-d560c622c20b，build r19-fingerprint-kernel，super_hos隔离参数及userdata p12/370G确认，boot_completed1、SELinux Enforcing、USBdevice/device_only=true、persist.adb.tcp.port5555；实际无线ADB连接成功、单次公网ping成功（不代表吞吐/稳定性验收）。UEFI选中入口读回成功，热温控native合约RESULT PASS，真实skin约31.46°C。

## 未通过的传感器回归与基线对照

候选三轮strict：raw accel/gyro全finite、timestamp正常，但Gravity/GameRotation全NaN。未加载任何指纹模块，先保留证据并切回旧R17 EFI（R18entry），同一userdata/姿态/APK三轮复现同样结果。基线boot ID 698f3f48-9474-47e1-875a-b17304394bf7。因此不能把它称为仅候选新增回归，也不能以原始IMU正常判完整传感器通过。

当前留在R18基线HOS，probe已force-stop、无加载模块；测试APK暂留供下一轮观测升级。实际HyperOS libsensorservice.so存在初始acc零norm归一化未守卫路径，后续正在捕获早期原始样本并准备精确修复。融合问题未修、新内核指纹空载复测未完成、真实手指/图像/HAL均未完成，整合未宣称验收通过。

证据：~/.cache/hos-r19-fingerprint-kernel、hos-r19-fingerprint-deploy、hos-r19-fusion。
