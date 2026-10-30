// SPDX-License-Identifier: GPL-2.0
/*
 * Single-shot FTE7001 transport probe, not a fingerprint HAL.
 * No arbitrary commands: LOAD, LOOKUP, wire 0x8031 (12/16 bytes), SHUTDOWN.
 * Requires the kernel's patch 0050 exports; never issues a raw SMC.
 * A listener-round limit cannot interrupt an SMC that never returns. Run only
 * with physical recovery available. No claim that this cannot wedge firmware.
 *
 * Any failed secure call after LOAD has begun retains every shared buffer and
 * a module reference until reboot. Do not retry a potentially parked TA.
 */
#include <linux/module.h>
#include <linux/debugfs.h>
#include <linux/firmware/qcom/qcom_scm.h>
#include <linux/firmware/qcom/qcom_tzmem.h>
#include <linux/kernel_read_file.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/vmalloc.h>
#include "fp_app_compat.h"

#define FP_IMAGE_PATH "/data/local/tmp/fingerpr.mbn"
#define FP_ALIGN (4 * 1024 * 1024)
#define FP_MAX_IMAGE (16 * 1024 * 1024)
#define FP_REQ_SIZE 12
#define FP_RSP_SIZE 16

static DEFINE_MUTEX(probe_lock);
static struct dentry *probe_dir;
static struct qcom_tzmem_pool *image_pool, *io_pool;
static void *image_raw, *request, *response;
static bool attempted;

static void release_buffers(void)
{
	if (request)
		qcom_tzmem_free(request);
	if (response)
		qcom_tzmem_free(response);
	if (io_pool)
		qcom_tzmem_pool_free(io_pool);
	if (image_raw)
		qcom_tzmem_free(image_raw);
	if (image_pool)
		qcom_tzmem_pool_free(image_pool);

	request = response = image_raw = NULL;
	io_pool = image_pool = NULL;
}

static int prepare_buffers(void **image, size_t *image_len)
{
	struct qcom_tzmem_pool_config cfg = { .policy = QCOM_TZMEM_POLICY_STATIC };
	struct qcom_tzmem_pool *pool;
	phys_addr_t raw_phys, aligned_phys, req_phys, rsp_phys;
	size_t span;
	void *file = NULL;
	int ret;

	ret = kernel_read_file_from_path(FP_IMAGE_PATH, 0, &file, FP_MAX_IMAGE,
					image_len, READING_FIRMWARE);
	if (ret < 0)
		return ret;
	if (!*image_len || *image_len > FP_MAX_IMAGE) {
		ret = -EINVAL;
		goto out;
	}

	fpq_constrain_scm_dma();
	/* Allow a physically aligned subrange, never assume virtual alignment.
	 * This may fail on kernels unable to allocate an 8 MiB coherent chunk;
	 * failure happens before LOAD. Do not replace it with unchecked memory.
	 */
	span = ALIGN(*image_len, FP_ALIGN) + FP_ALIGN;
	cfg.initial_size = cfg.max_size = span;
	pool = qcom_tzmem_pool_new(&cfg);
	if (IS_ERR(pool)) {
		ret = PTR_ERR(pool);
		goto out;
	}
	image_pool = pool;
	image_raw = qcom_tzmem_alloc(image_pool, span, GFP_KERNEL);
	if (!image_raw) {
		ret = -ENOMEM;
		goto out;
	}
	raw_phys = qcom_tzmem_to_phys(image_raw);
	if (!raw_phys || upper_32_bits(raw_phys)) {
		ret = -ERANGE;
		goto out;
	}
	aligned_phys = ALIGN(raw_phys, FP_ALIGN);
	if (aligned_phys < raw_phys || aligned_phys - raw_phys > span - *image_len ||
	    upper_32_bits(aligned_phys + *image_len - 1)) {
		ret = -ERANGE;
		goto out;
	}
	*image = (u8 *)image_raw + (aligned_phys - raw_phys);
	if (qcom_tzmem_to_phys(*image) != aligned_phys) {
		ret = -EINVAL;
		goto out;
	}
	memcpy(*image, file, *image_len);

	cfg.initial_size = cfg.max_size = 8192;
	pool = qcom_tzmem_pool_new(&cfg);
	if (IS_ERR(pool)) {
		ret = PTR_ERR(pool);
		goto out;
	}
	io_pool = pool;
	request = qcom_tzmem_alloc(io_pool, 4096, GFP_KERNEL);
	response = qcom_tzmem_alloc(io_pool, 4096, GFP_KERNEL);
	if (!request || !response) {
		ret = -ENOMEM;
		goto out;
	}
	req_phys = qcom_tzmem_to_phys(request);
	rsp_phys = qcom_tzmem_to_phys(response);
	if (!req_phys || !rsp_phys || upper_32_bits(req_phys + 4095) ||
	    upper_32_bits(rsp_phys + 4095)) {
		ret = -ERANGE;
		goto out;
	}
	memset(request, 0, 4096);
	memset(response, 0, 4096);
	pr_info("fpread: prepared image %zu bytes at %pa, request/response 12/16 bytes\n",
		*image_len, &aligned_phys);
	ret = 0;
out:
	vfree(file);
	return ret;
}

static int run_probe(void)
{
	static const u8 command[FP_REQ_SIZE] = {
		0x31, 0x80, 0, 0, 0, 0, 0, 0, 4, 0, 0, 0
	};
	void *image = NULL;
	size_t image_len = 0;
	u32 app_id = 0, looked_up = 0;
	int ret, validation = 0;

	ret = prepare_buffers(&image, &image_len);
	if (ret) {
		release_buffers();
		return ret;
	}
	if (!try_module_get(THIS_MODULE)) {
		release_buffers();
		return -ENODEV;
	}
	/* Pin before the first call. A hung/uncertain secure call cannot be
	 * recovered by unloading its caller or freeing its shared memory.
	 */
	pr_info("fpread: beginning LOAD; no automatic retry\n");
	ret = fpq_app_load(image, 0, image_len, &app_id);
	if (ret)
		goto uncertain;
	pr_info("fpread: LOAD returned app_id=%u\n", app_id);
	ret = qcom_scm_qseecom_app_get_id("fingerprint", &looked_up);
	/* #125 already observed LOAD id=5, LOOKUP -ENOENT, SHUTDOWN(5)
	 * success. The assumed name is not authoritative; APP_START's
	 * validated APP_ID response is. Only that known name miss is allowed.
	 */
	if (ret == -ENOENT) {
		pr_warn("fpread: known LOOKUP name miss; using successful LOAD app_id=%u\n",
			app_id);
	} else if (ret) {
		goto uncertain;
	} else if (looked_up != app_id) {
		ret = -EPROTO;
		goto uncertain;
	}
	memcpy(request, command, sizeof(command));
	ret = qcom_scm_qseecom_app_send(app_id, request, FP_REQ_SIZE,
					response, FP_RSP_SIZE);
	if (ret)
		goto uncertain;
	print_hex_dump(KERN_INFO, "fpread ack: ", DUMP_PREFIX_OFFSET,
		       16, 1, request, FP_REQ_SIZE, false);
	print_hex_dump(KERN_INFO, "fpread rsp: ", DUMP_PREFIX_OFFSET,
		       16, 1, response, FP_RSP_SIZE, false);
	/* The handler sets req[0]=0x5aa5 and req[2]=0. A zero status alone
	 * is not proof of execution because the response starts zeroed.
	 */
	if (le32_to_cpu(((__le32 *)request)[0]) != 0x5aa5 ||
	    le32_to_cpu(((__le32 *)request)[2]) != 0)
		validation = -EPROTO;
	pr_info("fpread: transport returned; ack_valid=%d status=0x%08x\n",
		!validation, le32_to_cpu(((__le32 *)response)[3]));
	ret = fpq_app_shutdown(app_id);
	if (ret)
		goto uncertain;
	release_buffers();
	module_put(THIS_MODULE);
	pr_info("fpread: SHUTDOWN succeeded; buffers released; validation=%d\n",
		validation);
	return validation;

uncertain:
	pr_err("fpread: secure call/state uncertain: %d; buffers/module retained, reboot before any further fingerprint experiment\n",
	       ret);
	return ret;
}

static ssize_t probe_write(struct file *f, const char __user *buf,
			   size_t len, loff_t *off)
{
	char command[6];
	int ret;

	if (len != 5 && len != 6)
		return -EINVAL;
	if (copy_from_user(command, buf, len))
		return -EFAULT;
	if (memcmp(command, "probe", 5) || (len == 6 && command[5] != '\n'))
		return -EINVAL;
	mutex_lock(&probe_lock);
	if (attempted) {
		ret = -EALREADY;
	} else {
		attempted = true;
		ret = run_probe();
	}
	mutex_unlock(&probe_lock);
	return ret ? ret : len;
}

static const struct file_operations probe_fops = {
	.owner = THIS_MODULE,
	.write = probe_write,
};

static int __init fpread_init(void)
{
	struct dentry *file;

	probe_dir = debugfs_create_dir("gaokun3_fpread", NULL);
	if (IS_ERR(probe_dir))
		return PTR_ERR(probe_dir);
	file = debugfs_create_file("probe", 0200, probe_dir, NULL, &probe_fops);
	if (IS_ERR_OR_NULL(file)) {
		debugfs_remove_recursive(probe_dir);
		return file ? PTR_ERR(file) : -ENOMEM;
	}
	pr_info("fpread: idle; write 'probe' once to gaokun3_fpread/probe\n");
	return 0;
}

static void __exit fpread_exit(void)
{
	debugfs_remove_recursive(probe_dir);
}

module_init(fpread_init);
module_exit(fpread_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("FTE7001 fixed readonly 0x8031 transport probe");
