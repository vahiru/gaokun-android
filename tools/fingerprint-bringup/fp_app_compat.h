/* SPDX-License-Identifier: GPL-2.0 */
#ifndef FP_APP_COMPAT_H
#define FP_APP_COMPAT_H
/*
 * 指纹 TA 加载/卸载 —— 【只走内核导出符号】（内核需带 patch 0050）。
 *
 * ⚠️ 2026-10-30 事故记录：初版在本头文件里手搋 SMC（fpq_smc_app）直发 APP_START，
 * SMC32 convention 一试就把 CPU 楔死在 TrustZone —— RCU stall 循环、两个
 * echo 1 > load 永久卡 D 状态、rmmod EBUSY，只能断电恢复。手搋 SMC 已【整体删除】：
 * 内核 __scm_smc_call 的 convention 探测、调用锁、listener 服务都是绕不过去的
 * 一层，绕过它的每一次错误尝试都要付一次重启的代价。
 *
 * 现在要求内核带 patch 0050（qcom_scm_qseecom_app_load / app_shutdown 两个
 * EXPORT_SYMBOL_GPL），模块直接调用；跑在没打 0050 的内核上 insmod 会因未知
 * 符号失败 —— 这是刻意的安全失败，好过再发一次裸 SMC。
 *
 * 加载语义（与 #125 在真机验证过的一致）：
 *   LOAD     qcom_scm_qseecom_app_load(img, mdt_len, img_len, &app_id)；
 *            单一 secelf（fingerpr.mbn）⇒ mdt_len=0，元数据由 TZ 自解析；
 *            成功 ⇒ app_id 可用，TZ 已把镜像拷入安全世界，staging 可释放。
 *   SHUTDOWN qcom_scm_qseecom_app_shutdown(app_id)；不卸载则二次加载被拒。
 */
#include <linux/device.h>
#include <linux/dma-mapping.h>
#include <linux/errno.h>
#include <linux/firmware/qcom/qcom_scm.h>
#include <linux/firmware/qcom/qcom_tzmem.h>
#include <linux/platform_device.h>
#include <linux/printk.h>
#include <linux/types.h>

/* 建池前把 scm 设备收窄到 32 位 DMA（同 0050 的 qcom_scm_qseecom_init；
 * 0050 内核开机时已做过，这里兜底重复一次无害。APP_START 的物理地址是 SMC32
 * 裸参数，>4GB 会被截断 ⇒ 轻则拒绝、重则整机静默挂死）。失败只告警，由调用方
 * 对 qcom_tzmem_to_phys() 结果的 <4GB 校验兜底。 */
static inline void fpq_constrain_scm_dma(void)
{
	struct device *dev;
	int ret;

	dev = bus_find_device_by_name(&platform_bus_type, NULL, "firmware:scm");
	if (!dev) {
		pr_warn("fpq: 找不到 firmware:scm 设备，跳过 32 位 DMA 收窄\n");
		return;
	}
	ret = dma_set_coherent_mask(dev, DMA_BIT_MASK(32));
	if (ret)
		pr_warn("fpq: scm 32 位 DMA 收窄失败 %d（大镜像可能拿到 >4GB 地址）\n", ret);
	else
		pr_info("fpq: scm 设备 DMA 已收窄到 32 位\n");
	put_device(dev);
}

/*
 * 0050 qcom_scm_qseecom_app_load() 的薄封装。img = 镜像的【tzmem 虚拟址】
 * （调用方负责 4MiB 物理对齐与 <4GB 校验 —— 对齐要在物理空间算，vaddr 对齐
 * 不代表 paddr 对齐）；mdt_len=0 表示单一 secelf。
 */
static inline int fpq_app_load(void *img, size_t mdt_len, size_t img_len, u32 *app_id)
{
	return qcom_scm_qseecom_app_load(img, mdt_len, img_len, app_id);
}

/* 0050 qcom_scm_qseecom_app_shutdown() 的薄封装 */
static inline int fpq_app_shutdown(u32 app_id)
{
	return qcom_scm_qseecom_app_shutdown(app_id);
}

#endif /* FP_APP_COMPAT_H */
