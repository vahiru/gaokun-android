# Flag-only救援ECM候选（尚未安装）

这些文件仅用于独立救援启动项，不操作Android、不刷分区、不修改现有默认入口。
安装前由控制代理备份并核对救援p3上的实际unit/host脚本/睡眠hook和NetworkManager状态。

文件映射：

| 候选 | 救援内目标 |
|---|---|
| setup-device-ecm.py | /usr/local/sbin/gaokun3-hos-rescue-device，root:root，0755 |
| gaokun3-hos-rescue-device.service | /etc/systemd/system/同名文件，0644 |
| host-service-device-flag.conf | /etc/systemd/system/gaokun3-usb-role.service.d/hos-device.conf |
| host-script-flag-guard.sh.inc | 审核后插入现有/usr/local/bin/gaokun3-usb-role-host.sh的shebang之后 |

只启用新unit的multi-user.target链接。不要替换旧host脚本主体或直接取消普通救援服务。
片段拦截system-sleep hook直接调用host脚本的旁路；普通boot没有flag时不会退出host脚本。
实现的unit在NetworkManager/systemd-networkd启动任务之后检查其真实状态，和早期设计
中的Before排序不同：不停止它们，只在/run增加该ECM MAC专属udev规则避免新接口被接管。

## 专用入口参数

复制已验证救援entry到独立新名字，保留原linux/initrd/root等全部参数，追加：

```
gaokun3.hos_rescue_device=1 systemd.mask=sleep.target systemd.mask=suspend.target systemd.mask=hibernate.target systemd.mask=hybrid-sleep.target systemd.mask=suspend-then-hibernate.target systemd.mask=systemd-suspend.service systemd.mask=systemd-hibernate.service systemd.mask=systemd-hybrid-sleep.service systemd.mask=systemd-suspend-then-hibernate.service
```

使用oneshot选中此新entry，默认entry和普通救援entry不变。启动后核对这些unit的
runtime masked状态及旧host服务condition跳过；不要仅以cmdline字符串判断已经生效。

## 约束与幂等

- 程序没有任意路径/网口/gadget参数。只接受root、gaokun3-rescue、根p3和精确单一flag。
- 使用/run独占锁。foreign configfs gadget或已有legacy UDC驱动均在role变更前拒绝。
- 新建hos_transfer采用旧脚本相同VID/PID/序列号/MAC。完整属性核验后，记录本boot的
  root-only ownership JSON。已有gadget必须全配置及marker一致才复用；不接管无marker的
  旧手工gadget或部分创建状态，不解绑、不重绑已正确UDC。
- configfs MAC只规范化精确标准MAC后的可选单NUL/单LF，不吞中间NUL或垃圾。
  Linux composite在GET_DESCRIPTOR时会覆盖bcdUSB；已有同boot marker仅允许有证据的
  0000→0210变化：绑定同UDC、role=device、state=configured、maximum_speed为super-speed
  或super-speed-plus且current_speed为full/high-speed。其他任何属性变化仍拒绝；marker
  不被重写，未知/USB3速率暂不泛化。创建时设置0200不能阻止内核的无条件覆盖。
- 通过function ifname与唯一设备MAC交叉核对实际网口，不能把任意usb0当作目标。
- /run/udev/rules.d只给设备MAC02:48:4f:53:12:01设置NM_UNMANAGED，reload rules但
  不trigger其他设备。NetworkManager active时再查询并确认该接口managed=no；必要时
  只修改这个接口，且已有任何地址/路由时拒绝managed→unmanaged接管，避免清掉连接。
- 不启停网络管理器、不改持久Wi-Fi profiles。networkd active且规则尚未审查时停止，
  不与它竞争。其它地址/路由保留，只添加169.254.77.1/16并使此接口UP。
- 成功重跑不创建/重绑gadget或重复添加地址。没有ExecStop、没有失败切host或自动清场。

## 失败时人工恢复

1. 从已有Wi-Fi fallback查看新unit的journal和失败原因。原Wi-Fi profiles、默认路由、
   SSH配置均未修改；不要为修ECM重启整个NetworkManager。
2. foreign gadget、legacy UDC或marker不符时，不写空UDC、不删除gadget、不改role。
   先确认是谁创建、是否承载连接，再另行处理。helper不会自动收编它。
3. 新建中途失败且没有完整marker：保留现场。通过Wi-Fi安排再次进入同一专用entry，
   重启会清空configfs和/run状态；不要伪造marker强行通过校验。
4. 已有正确marker且仅IP/NM阶段失败：先只读查看地址/路由与管理状态。确认冲突解决后
   可重跑新unit，它会复用绑定；脚本不会为了接管删地址或覆盖不同prefix。
5. helper报告“configured”只证明设备端配置；还需Mac按ECM主机MAC识别新接口，取得当前
   169.254源地址，再以该源地址SSH到169.254.77.1，沿用HostKeyAlias=192.168.10.225。
   不沿用重启前en8/IP，不修改Mac默认路由或忽略host-key冲突。
6. 如正在进行镜像更新，必须由更新工具的实际完成/失败状态决定能否重启；不要在写盘中
   为修网络直接重启。此helper自身不参与写盘。

## 离线验证

```
python3 -m unittest discover -s scripts/hos-rescue -p 'test_*.py' -v
python3 -m py_compile scripts/hos-rescue/setup-device-ecm.py
sh -n scripts/hos-rescue/host-script-flag-guard.sh.inc
```

mock使用真实身份/所有权判断、网络地址保护和NM接管逻辑，覆盖拒绝错误系统、foreign
gadget、legacy绑定、部分/变更配置、重复运行、正确绑定不重绑、单接口NM变更与地址
冲突。它不证明真实configfs/udev/NetworkManager版本行为；首次专用boot仍需读回验收。

## 未认领会话限时回退候选

额外安装 `fallback-unclaimed.py` 为 `/usr/local/sbin/gaokun3-hos-rescue-fallback`
（root:root0755），并安装/启用 `gaokun3-hos-rescue-fallback.service`。默认无参数模式只有
专用flag、root身份、hostname=gaokun3-rescue、根p3全部通过才计时600秒。
之后没有认领且未发现更新器/p7/整盘打开句柄，才可能请求正常 `systemctl --no-block reboot`。
普通救援和Android不会通过身份检查；unit没有Restart/ExecStop回退动作。

### 控制器必须使用原子认领模式

控制器首次连接并验证SSH主机身份、hostname和根p3后，**在复制更新文件、归档旧marker、
preview或任何更新准备之前**执行：

```
sudo -n /usr/local/sbin/gaokun3-hos-rescue-fallback --claim
```

必须收到成功退出；不能用 `touch`、覆盖文件，或仅看“文件存在”就开始更新。
这个固定模式也检查root/hostname/p3/精确单一flag。它用O_CREAT|O_EXCL|O_NOFOLLOW创建
`/run/gaokun3-hos-rescue-session.claimed`，设root:root0600，写入精确的当前boot UUID和一个
换行，并fsync文件及目录。已有文件只有普通文件、root:root0600、同inode读回且内容严格
等于当前boot UUID+换行才可复用。旧boot、symlink、权限/owner不符、半写及其它异常都拒绝。

认领模式与fallback对**同一个文件**竞争O_EXCL：

- 控制器先赢：fallback看见任何claim（包括半写/损坏的文件）都保持救援，不会自动重启。
- fallback先赢：它写入 `reboot-requested:<当前boot UUID>\n` 的预留内容并fsync；晚到的
  `--claim`必须拒绝，不能把这份预留当成已有更新会话。由此关闭“最后检查之后才出现claim”
  的竞态。创建或fsync失败时保留不完整marker，绝不删除后继续。
- 预留后还会重查身份、boot ID、进程及打开的块设备。发现活动/检查失败时保留预留并停止。
  如果systemctl失败或超时，重启可能已经排队，日志明确结果不确定；预留仍不删除。

认领只禁止本helper的自动回退，**不是**更新器互斥锁；原更新器的started marker、独占p7、
大小/SHA/读回/保护区校验仍必须执行。保持claim直到主动结束本boot，不提供自动删除/释放模式。
遇到reboot预留或半写marker，应先确认设备/任务实际状态，不得为继续写盘而盲删。

### 进程与FD检查范围

存在 `update-isolated-super.py` argv时保持救援。检查每个 `/proc/PID/fd` 的块设备类型和
`st_rdev`，匹配真实 `/dev/nvme0n1p7`，并保守匹配整盘 `/dev/nvme0n1`；不依赖readlink显示
路径，因此别名、bind挂载或deleted设备节点不能仅靠名字逃过匹配。读者也会阻止回退。
目标设备不是块设备、目标或proc权限/检查异常都会hold；只有检查过程中消失的PID/FD可跳过。
claim存在性使用显式lstat，只有ENOENT算缺失，不能用会吞PermissionError的lexists判断。

这些是受控流程的保护，不能保证发现外部脚本任意底层写盘行为；所有受控更新仍须先认领。

### 默认启动目标是控制器前提，不是helper的证明

helper不写分区、BCB、EFI默认或oneshot，只请求正常systemd重启。控制器在启用专用入口
之前必须核对预期默认entry、BCB状态及oneshot消费/清除行为；不能因为helper执行reboot
就认定下次必然进入Android。默认本身指向救援、另有oneshot或待处理recovery请求时，
实际目的地由这些原有状态决定。本helper既不修正它们，也不绕过更新后的启动验收。

### 离线测试边界

`test_fallback.py`执行真实main和真实标记创建/读取流程（本地临时文件、mock root元数据及
系统调用），覆盖原PermissionError/late-claim反例、两种O_EXCL胜负、半写/fsync失败、已有
claim严格验证、预留后检查失败与systemctl结果不确定。proc parser用真实目录/argv解析及
模拟的block st_rdev覆盖别名、整盘与消失FD。它不会请求真实重启或访问设备；不能替代
实际systemd/proc/启动路径验收。
