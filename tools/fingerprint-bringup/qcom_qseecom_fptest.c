// SPDX-License-Identifier: GPL-2.0
/*
 * gaokun3 指纹 TA 加载【冒烟测试】模块（一次性诊断，不进发版内核）。
 *
 * 目的：回答唯一的 make-or-break 问题 —— 在本机 Linux 上到底能不能把华为签名的
 *       指纹 TA（fingerpr.mbn，单一 secelf，TA 名 "fingerprint"）加载进 QSEE 并拿到
 *       app_id。用厂商自己的 QSEECOM SMC 路径加载厂商自己签名的 TA，TZ 仍逐段验签。
 *
 * ⚠️ 只在写 debugfs 触发文件时才动作，加载/insmod 本身【什么 SMC 都不发】。
 * ⚠️ 安全阀：镜像缓冲的物理地址若不在 32 位以内，【拒绝】发 LOAD（APP_START 的物理
 *    地址是 SMC32 裸参数，高位截断会让 TZ 收到错地址、可能整机静默挂死）。宁可不测。
 *
 * 用法（设备旁有人能按电源键时）：
 *   把 fingerpr.mbn 放到 /data/local/tmp/fingerpr.mbn
 *   insmod qcom_qseecom_fptest.ko
 *   echo load > /sys/kernel/debug/gaokun3_fptest/trigger
 *   dmesg | tail
 */
#include <linux/module.h>
#include <linux/debugfs.h>
#include <linux/firmware/qcom/qcom_scm.h>
#include <linux/firmware/qcom/qcom_tzmem.h>
#include <linux/dma-mapping.h>
#include <linux/kernel_read_file.h>
#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include "fp_app_compat.h"

#define FP_IMG_ALIGN	(4 * 1024 * 1024)	/* TZ 要 4 MiB 对齐的物理地址 */
#define FP_IMG_PATH	"/data/local/tmp/fingerpr.mbn"
#define FP_APP_NAME	"fingerprint"
#define FP_IMG_MAX	(16 * 1024 * 1024)

static struct dentry *fp_dir;

static int fp_do_load(void)
{
	struct qcom_tzmem_pool_config cfg = { .policy = QCOM_TZMEM_POLICY_STATIC };
	struct qcom_tzmem_pool *pool;
	void *stage, *aligned, *filebuf = NULL;
	phys_addr_t stage_phys, img_phys;
	size_t stage_len;
	size_t img_len = 0;
	u32 app_id = 0, look_id = 0;
	int ret;

	/* 同 0050：先收窄 DMA 到 32 位，再建池（事后改 mask 没用） */
	fpq_constrain_scm_dma();

	ret = kernel_read_file_from_path(FP_IMG_PATH, 0, &filebuf, FP_IMG_MAX,
					 &img_len, READING_FIRMWARE);
	if (ret < 0) {
		pr_err("fptest: 读不到 %s: %d\n", FP_IMG_PATH, ret);
		return ret;
	}
	pr_info("fptest: 读入 %s，%zu 字节\n", FP_IMG_PATH, img_len);

	/*
	 * TZ 要镜像落在 4 MiB 对齐的物理地址。参考实现用 img+4MiB 再找对齐子区，
	 * 但那对 3.67MB 的镜像是 ~7.7MB ⇒ 单次连续分配到 order-11(8MB)，超过
	 * MAX_PAGE_ORDER(4MB)、被 buddy 拒绝（实测 #124：page_alloc.c WARN + -ENOMEM）。
	 * 本机镜像 <4MiB，直接分配【正好 4MiB 的整数倍】：order-10 的连续块可分（buddyinfo
	 * 有），且 4MiB 分配天然 4MiB 对齐 ⇒ 池基址即对齐地址，不用额外 slack。
	 * ⚠️ 镜像若将来 >4MiB，这条走不通，得改走 CMA（qcom_tzmem 侧）。
	 */
	stage_len = ALIGN(img_len, FP_IMG_ALIGN);
	cfg.initial_size = stage_len;
	cfg.max_size = stage_len;

	pool = qcom_tzmem_pool_new(&cfg);
	if (IS_ERR(pool)) {
		ret = PTR_ERR(pool);
		pr_err("fptest: tzmem 池创建失败: %d（%zu 字节；>4MiB 会超 MAX_PAGE_ORDER）\n",
		       ret, stage_len);
		goto out_file;
	}

	stage = qcom_tzmem_alloc(pool, stage_len, GFP_KERNEL);
	if (!stage) {
		pr_err("fptest: tzmem 分配 %zu 失败\n", stage_len);
		ret = -ENOMEM;
		goto out_pool;
	}

	stage_phys = qcom_tzmem_to_phys(stage);
	if (!IS_ALIGNED(stage_phys, FP_IMG_ALIGN)) {
		pr_err("fptest: 池基址 %pa 未 4MiB 对齐，放弃（本机不该发生）\n", &stage_phys);
		ret = -EINVAL;
		goto out_stage;
	}
	img_phys = stage_phys;
	aligned = stage;

	/* ★ 安全阀：物理地址必须整段落在 32 位以内，否则拒发 LOAD。 */
	if (upper_32_bits(img_phys) ||
	    upper_32_bits(img_phys + img_len - 1)) {
		pr_err("fptest: 镜像物理地址 %pa(+%lld) 超出 32 位 —— 拒绝发 LOAD（会截断挂机）。"
		       "需要把 scm 设备约束到 32 位 DMA（patch 0050）后再试。\n",
		       &img_phys, (unsigned long long)img_len);
		ret = -ERANGE;
		goto out_stage;
	}

	memcpy(aligned, filebuf, img_len);
	pr_info("fptest: 镜像就位于物理 %pa（4MiB 对齐、<4GB），准备 LOAD app '%s'\n",
		&img_phys, FP_APP_NAME);

	/* 单一 secelf ⇒ mdt_len = 0 */
	ret = fpq_app_load(aligned, 0, img_len, &app_id);
	if (ret) {
		pr_err("fptest: ★ LOAD 失败: %d（TA 未加载）\n", ret);
		goto out_stage;
	}
	pr_info("fptest: ★★★ LOAD 成功！app_id=%u\n", app_id);

	/* 确认 LOOKUP 现在也能查到它 */
	ret = qcom_scm_qseecom_app_get_id(FP_APP_NAME, &look_id);
	if (ret)
		pr_warn("fptest: LOAD 成功但 LOOKUP '%s' 失败: %d\n", FP_APP_NAME, ret);
	else
		pr_info("fptest: LOOKUP '%s' = %u（%s app_id）\n", FP_APP_NAME, look_id,
			look_id == app_id ? "== " : "!= 注意 ");

	/* 收工：卸载，免得二次加载被拒 */
	ret = fpq_app_shutdown(app_id);
	if (ret)
		pr_warn("fptest: app_shutdown(%u) 失败: %d（下次加载可能要重启）\n", app_id, ret);
	else
		pr_info("fptest: app_shutdown(%u) ok，已清理\n", app_id);
	ret = 0;

out_stage:
	qcom_tzmem_free(stage);
out_pool:
	qcom_tzmem_pool_free(pool);
out_file:
	vfree(filebuf);
	return ret;
}

static ssize_t fp_trigger_write(struct file *f, const char __user *buf,
				size_t len, loff_t *off)
{
	char cmd[16] = {};
	int ret;

	if (len == 0 || len >= sizeof(cmd))
		return -EINVAL;
	if (copy_from_user(cmd, buf, len))
		return -EFAULT;
	if (strncmp(cmd, "load", 4)) {
		pr_info("fptest: 只认 'load'\n");
		return -EINVAL;
	}
	ret = fp_do_load();
	return ret ? ret : len;
}

static const struct file_operations fp_trigger_fops = {
	.owner = THIS_MODULE,
	.write = fp_trigger_write,
};

static int __init fptest_init(void)
{
	fp_dir = debugfs_create_dir("gaokun3_fptest", NULL);
	if (IS_ERR(fp_dir))
		return PTR_ERR(fp_dir);
	debugfs_create_file("trigger", 0200, fp_dir, NULL, &fp_trigger_fops);
	pr_info("fptest: 就绪。echo load > /sys/kernel/debug/gaokun3_fptest/trigger 触发（不会自动发 SMC）\n");
	return 0;
}

static void __exit fptest_exit(void)
{
	debugfs_remove_recursive(fp_dir);
}

module_init(fptest_init);
module_exit(fptest_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("gaokun3 fingerprint TA load smoke test");
