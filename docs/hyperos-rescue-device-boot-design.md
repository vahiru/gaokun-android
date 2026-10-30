# 专用救援启动项的USB device连接设计审查

本次只读审查仓库旧脚本与提供的救援服务信息，没有挂载分区、操作设备、安装服务或
修改网络。以下是待父线程实施/验证的最小配置，不是已上机结果。

## 结论

独立救援boot entry追加 `gaokun3.hos_rescue_device=1`，通过条件阻止旧host服务、
只在此入口启动ECM，适合保留普通救援行为。**仅给host服务加Condition仍不完整**：
仓库system-sleep hook在pre直接调用host脚本，会绕过服务条件。

建议专用入口同时追加以下临时systemd生成器参数，使传输期间不触发该睡眠路径：

```
gaokun3.hos_rescue_device=1 systemd.mask=sleep.target systemd.mask=suspend.target systemd.mask=hibernate.target systemd.mask=hybrid-sleep.target systemd.mask=suspend-then-hibernate.target
```

这些只属于新entry，不改变旧救援entry的cmdline/default。实施前应检查救援systemd
支持 `systemd.mask=` 生成器；启动后读回这些target的runtime masked状态。
若另有udev规则或其他服务直接强制host，需要按同flag约束；不能只检查一个unit名。
不要把普通救援的host/suspend设计也一起永久删除。

## 服务配置

旧服务drop-in，例如 `gaokun3-usb-role.service.d/hos-device.conf`：

```ini
[Unit]
ConditionKernelCommandLine=!gaokun3.hos_rescue_device=1
```

新unit示意：

```ini
[Unit]
Description=gaokun3 isolated HOS rescue ECM transport
ConditionKernelCommandLine=gaokun3.hos_rescue_device=1
After=local-fs.target systemd-udevd.service gaokun3-usb-role.service
Before=NetworkManager.service systemd-networkd.service

[Service]
Type=oneshot
RemainAfterExit=yes
TimeoutStartSec=45
ExecStart=/usr/local/sbin/gaokun3-hos-rescue-device

[Install]
WantedBy=multi-user.target
```

不依赖network-online.target，否则会等待自己正在建立的网络。保留默认systemd
依赖，45秒内对role/UDC/interface做有界等待。Before只排序已被启动的网络管理服务，
不额外启动/停止它们。没有ExecStop解绑，也不在失败时自动切回host；SSH可继续依靠
已有Wi-Fi配置恢复现场。成功重复启动应只核验并复用自己完全匹配的gadget。

## 旧setup脚本的范围与缺口

`out/hyperos-r13/setup-rescue-usb.sh` 验证hostname和root p3，加载libcomposite，
必要时挂载configfs，要求gadget目录为空，然后创建ECM并给usb0地址。
它没有刷分区、解绑控制器或删除网络。但它是一次性脚本，不能直接充当可重试的
开机服务：

- 已有任何gadget即失败；mkdir/ip addr add重复会EEXIST，不是幂等。
- 没有设置/等待role=device或UDC出现；写UDC可能在controller未就绪时失败。
- 绑定后再配置IP，后续失败会留下已绑定但未完整配置的gadget。
- 硬编码usb0，未证明它就是此次ECM接口，未处理重命名。
- 不防NetworkManager为新接口自动配置并覆盖手工地址。

因此采用旧脚本已测参数，不应原样自动调用或遇错先解绑重建。

## helper的最小操作约束

1. 验证root、hostname=`gaokun3-rescue`、findmnt `/` 来源=`/dev/nvme0n1p3`、
   cmdline精确flag，再持有/run下独占锁。拒绝重复/矛盾flag配置。
2. 加载libcomposite，检查/sys/kernel/config确实为configfs。改变role前检查gadget：
   foreign目录存在立即失败；已有hos_transfer必须核对VID/PID/序列号、MAC、配置
   symlink、functions类型与UDC绑定。未知或部分不一致状态不擅自解绑/重建。
3. 保留已测身份：VID1d6b、PID0104、序列号gaokun3-hos-transfer，ECM设备MAC
   `02:48:4f:53:12:01`，主机MAC `02:48:4f:53:12:02`。
4. 精确role路径 `/sys/class/usb_role/a600000.usb-role-switch/role`；已device不重复写。
   否则只请求device并读回，等待 `/sys/class/udc/a600000.usb`。不执行driver unbind。
5. 新gadget全部属性和symlink就绪后才绑定。UDC为空才写a600000.usb；已匹配则保留，
   已绑定其他UDC则失败。不要写空UDC“重置连接”。
6. 从ECM function的ifname读取实际接口，或按唯一设备MAC解析，验证归属；不要假定usb0。
7. 地址仅添加 `169.254.77.1/16`，已精确存在则跳过。同地址但不同prefix应报告冲突。
   不flush地址、路由或Wi-Fi，不添加默认路由、不启动DHCP服务器，不重启整个网络栈。
8. 配置完成后核对role、UDC、接口MAC/address/link状态，记录日志。部分失败保留现场，
   不自动释放已绑定gadget。重复成功执行不改变现有连接。

## 网络管理器与fallback

应先确认救援实际使用的管理器；提供的信息显示已有NetworkManager Wi-Fi profile。
若需阻止自动接管ECM，可以由flag-only helper在网络管理器启动前生成**/run**下的
单设备配置，例如NetworkManager：

```ini
[device-hos-transfer]
match-device=mac:02:48:4f:53:12:01
managed=0
```

只匹配该固定ECM设备MAC，不覆盖全局unmanaged-devices集合，不改保存的Wi-Fi profile。
若systemd-networkd也启用，要核对它的匹配规则；不要为处理新ECM盲停任何管理器。
普通boot没有flag便不会生成/run配置，原救援网络行为保持。

Mac侧当前en8/192.168.10.239属于当前系统连接，不能假定救援重启后仍是同一个接口。
应按本次ECM主机MAC/硬件端口识别接口，取得其当前169.254地址，然后显式绑定该
源地址SSH到169.254.77.1，继续使用已保存的 `HostKeyAlias=192.168.10.225`。
不覆盖known_hosts以掩盖不匹配；也不调整Mac默认路由。

既有救援Wi-Fi配置作为独立fallback保留。ECM无须互联网即可传镜像与SSH；不可
因ECM不提供默认网关而把本地链路误判失败。
