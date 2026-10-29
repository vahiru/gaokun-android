# R18 平台兼容修复部署记录

当前阶段：R18隔离super已部署，整块回读及冷启动验收通过。boot ID `1d6d8c09-52a8-4a2a-81fc-f93f17898df8`，ro.boot.hos_build=r18-platform-compat。

## 已验证变化

Thermal HAL仅相对R17增加forecastSeconds负数参数拒绝及缺皮温时可读unsupported消息；温区、读数、阈值逻辑未变。主机提取实际方法的ASan/UBSan测试通过，完整Android构建通过。

只读native Binder探针在原R17上得到两个预期FAIL：forecast(-1/INT_MIN)错误返回OK和当前温度。绑定候选后二者均EX_ILLEGAL_ARGUMENT且out为NaN；0/60合法调用、SKIN温度与门限均通过，RESULT PASS failures=0、exit0。没有伪造温度或修改阈值。

平台属性上下文候选仅新增四项exact libc_debug_prop映射，不改值/allow/dontaudit。五分区上下文与现用平台CIL重新核对，四项值均空。system镜像4,807条目核验仅一文件变化。20秒息屏基线中四项各20条libc拒绝，共80条；需冷启动后同条件复测。

## 产物

- HAL `cd4d3cfa117705c7004b5b1b14597d74d6e74a0be849f110f21183032a6212ca`。
- native probe `d833edf7ad5096cc0d142fc1ccb91e507bbcb4cd172aa36ce5154b2c41da55ae`，不加入PRODUCT_PACKAGES，构建树临时源码/安装文件已清理。
- system EROFS `d76882116fec9ce910afa252083462f2d9c49e6fe01ec59980c66fa85061544b`。
- super `ed290c3c83b4f0f88ec6d1a0a86c1c10f07a210a864cb260db85c373dcfc6276`，解包6分区逐一哈希通过，仅system/vendor与R17不同。vendor仅thermal HAL replacement不同，USB三文件和全部旧修复保持。
- 继续使用R17内核d22e364a…3b60bc，不构建/复制另一份内核；拟新增R18启动项指向原R17 EFI。设置配色候选不包含在本镜像。

## 升级保护

实际GPT两个副本与R17部署时逐字节一致；救援前后ESP66文件清单/哈希一致。旧R17升级marker没有盲删：验证其精确记录、当前super仍为R17新SHA、GPT/bootA/B及新审阅ESP后，独占硬链接归档原记录并删除活跃标记。旧证据保留在救援/var/lib/gaokun-hos-reviewed-updates。

新manifest SHA256 e8134d511dbbcc77989423018215f68f3159e2c34a2de3fe622d011584f54982，preview通过后才apply。工具只流写固定p7，完整大小/EOF/流SHA/整区回读和保护区复核通过前不得重启或改ESP。

证据：~/.cache/hos-r18-hal、hos-r18-libc、hos-r18-deploy。构建VM已deallocated回读。尚不宣称镜像已生效或日志已消失。


## 冷启动结果

升级进程退出0并输出VERIFIED，完成12GiB流/EOF/SHA、整块回读、GPT/bootA/B/ESP全集保护复核。之后新增R18启动项，复用原R17 EFI；默认启动项保留。boot_completed=1，SELinux Enforcing，USB服务running且device_only=true，无线ADB5555重启后无需再次开启即可连接。/data仍p12约370G、可用360G。Millet持久false，disabled监控服务未启动；导航资源保持MIUI组件。

从镜像启动的新HAL SHA与候选cd4d3cfa…6212ca一致。native Binder探针冷启动再次RESULT PASS failures=0、exit0；真实skin-temp约27.98°C，55/58/73阈值未变，负数参数均EX_ILLEGAL_ARGUMENT、合法0/60成功。缺传感器的unsupported消息分支仅由主机假传感器测试验证，没有为了制造失败而停用真实温控。

getprop -Z对四个精确名字均返回libc_debug_prop，四值仍空。相同息屏状态(debug.tracing.screen_state=1)、20秒logcat过滤窗口，四项各20条→各0条，总80→0。确认真实服务名是init.svc.smmustall（不是gaokun3_smmustall），PID717仍running、SELinux域gaokun3_smmustall，sleep子进程在采样前后轮换；未通过停轮询/关闭审计获得零日志。该结论只覆盖指定四项，不代表所有SELinux拒绝已消失。

测试native探针及旧临时HAL状态文件已清理。按更新前状态重新应用设置双资源临时浅蓝按压覆盖；没有加入镜像，仍待用户反馈和持久化决策。构建VM已deallocated，原AOSP源码及探针安装文件已恢复/清理。

仍未关闭OEM板温监视器与59号等级协议适配、真实手势/物理USB插拔、外设和休眠等验收；目标保持继续，未宣称HyperOS全功能完成。
