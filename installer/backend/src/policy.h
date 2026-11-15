/* 两个平台共用的安装前门槛：机型与电量。Windows 在 win/sys.c、Linux 在 linux/sys.c 里读系统状态，判断都走这里。 */
#ifndef GK3W_POLICY_H
#define GK3W_POLICY_H

/* 没接电源时电量低于这个百分比就不开始写盘：写 12 GiB 的 super 要几分钟，半路断电会导致磁盘分区处于不完整状态 */
#define GK3W_BATTERY_MIN_PCT 15

/* SMBIOS 的产品名（Windows 的 SystemProductName、Linux 的 /sys/class/dmi/id/product_name）是不是支持的机型：
 * 华为 MateBook E Go（gaokun3）。 */
int gk3w_model_supported(const char *product_name);

/* ac：1 接着电源、0 没接、-1 不知道；battery：百分比，-1 不知道。电量确实低而且确实没接电源才算低 */
int gk3w_battery_low(int ac, int battery);

#endif
