// SPDX-License-Identifier: GPL-2.0
/*
 * Single-shot FTE7001 version/info and vendor initialization probe, not a HAL.
 * No arbitrary commands: LOAD, LOOKUP, 0x8031, fixed MCU queries, real frame-state wait and one SRAM read, SHUTDOWN. Legacy host
 * init/sleep experiments are not dispatched by this module;
 * no enrollment, template operations or firmware download.
 * Requires the kernel's patch 0050 exports; never issues a raw SMC.
 * A listener-round limit cannot interrupt an SMC that never returns. Run only
 * with physical recovery available. No claim that this cannot wedge firmware.
 *
 * Any failed secure call after LOAD has begun retains every shared buffer and
 * a module reference until reboot. Do not retry a potentially parked TA.
 */
#include <linux/module.h>
#include <linux/delay.h>
#include <linux/debugfs.h>
#include <linux/fs.h>
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
#define FP_RESPONSE_CAPACITY 16384

static DEFINE_MUTEX(probe_lock);
static struct dentry *probe_dir;
static struct qcom_tzmem_pool *image_pool, *io_pool;
static void *image_raw, *request, *response;
static bool attempted;
static bool active_scan;
static bool status_only;
static unsigned int contact_wait_ms;
module_param(contact_wait_ms, uint, 0444);
MODULE_PARM_DESC(contact_wait_ms, "Bounded post-version contact delay in ms (0..15000); active_scan only");
static unsigned int contact_poll_count;
module_param(contact_poll_count, uint, 0444);
MODULE_PARM_DESC(contact_poll_count, "Immediate bounded contact polling (0=original10, otherwise10..200); excludes contact_wait_ms");
module_param(status_only, bool, 0444);
MODULE_PARM_DESC(status_only, "Only poll fixed MCU frame-status field; do not configure or capture");
module_param(active_scan, bool, 0444);
MODULE_PARM_DESC(active_scan, "Wait for real MCU frame flag and read one frame");
static bool secure_pending;
module_param(secure_pending, bool, 0444);
MODULE_PARM_DESC(secure_pending, "Secure call begun without confirmed shutdown; retain GPIO until reboot");

#include "fp-init-sequence.h"
#include "fp-active-scan.h"
#include "fp-frame-status.h"

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

	cfg.initial_size = cfg.max_size = 4096 + FP_RESPONSE_CAPACITY;
	pool = qcom_tzmem_pool_new(&cfg);
	if (IS_ERR(pool)) {
		ret = PTR_ERR(pool);
		goto out;
	}
	io_pool = pool;
	request = qcom_tzmem_alloc(io_pool, 4096, GFP_KERNEL);
	response = qcom_tzmem_alloc(io_pool, FP_RESPONSE_CAPACITY, GFP_KERNEL);
	if (!request || !response) {
		ret = -ENOMEM;
		goto out;
	}
	req_phys = qcom_tzmem_to_phys(request);
	rsp_phys = qcom_tzmem_to_phys(response);
	if (!req_phys || !rsp_phys || upper_32_bits(req_phys + 4095) ||
	    upper_32_bits(rsp_phys + FP_RESPONSE_CAPACITY - 1)) {
		ret = -ERANGE;
		goto out;
	}
	memset(request, 0, 4096);
	memset(response, 0, 4096);
	pr_info("fpversion: prepared image %zu bytes at %pa, request/response 12/16 bytes\n",
		*image_len, &aligned_phys);
	ret = 0;
out:
	vfree(file);
	return ret;
}

/* Exact Windows QSEE path: MCU GET_SC_FIRMWARE_VERSION 02 00 01 30 31.
 * A valid TA ACK alone does not establish an SPI reply. */
static int query_version(u32 app_id, bool *transport_uncertain)
{
	static const u8 write_command[17] = {
		0x21, 0x80, 0, 0, 5, 0, 0, 0, 0, 0, 0, 0,
		2, 0, 1, 0x30, 0x31
	};
	static const u8 read_command[12] = {
		0x20, 0x80, 0, 0, 0, 0, 0, 0, 69, 0, 0, 0
	};
	u8 *rx = (u8 *)response + 12;
	u8 check = 0;
	unsigned int length, i;
	int ret;

	memset(request, 0, 4096);
	memset(response, 0xa5, 4096);
	memcpy(request, write_command, sizeof(write_command));
	ret = qcom_scm_qseecom_app_send(app_id, request, 17, response, 12);
	if (ret) {
		*transport_uncertain = true;
		return ret;
	}
	/* TA_Commication waits 10ms after each completed IOCTL (0xc360). */
	msleep(10);
	if (le32_to_cpu(((__le32 *)request)[0]) != 0x5aa5 ||
	    le32_to_cpu(((__le32 *)request)[2]))
		return -EPROTO;
	msleep(20);
	memset(request, 0, 4096);
	memset(response, 0xa5, 4096);
	memcpy(request, read_command, sizeof(read_command));
	ret = qcom_scm_qseecom_app_send(app_id, request, 12, response, 81);
	if (ret) {
		*transport_uncertain = true;
		return ret;
	}
	msleep(10);
	print_hex_dump(KERN_INFO, "fpversion MCU reply: ", DUMP_PREFIX_OFFSET,
		       16, 1, rx, 69, false);
	if (le32_to_cpu(((__le32 *)request)[0]) != 0x5aa5 ||
	    le32_to_cpu(((__le32 *)request)[2]))
		return -EPROTO;
	length = ((unsigned int)rx[1] << 8) | rx[2];
	if (length < 2 || length > 65 || rx[3] != 4) {
		pr_err("fpversion: invalid MCU frame length=%u status=%u; no SPI success claim\n",
		       length, rx[3]);
		return -EPROTO;
	}
	for (i = 1; i <= length + 2; ++i)
		check ^= rx[i];
	if (check != rx[length + 3])
		return -EBADMSG;
	pr_info("fpversion: valid MCU reply, firmware payload %u bytes\n", length - 1);
	return 0;
}

/* Fixed dimensions/status and bounded raw snapshots. TA_Commication adds
 * 10ms after EACH send; ReadInfo_directly adds a further 1ms after the read. */
static int query_info(u32 app_id, bool *transport_uncertain)
{
	static const u8 indexes[] = {0x14, 0x15, 0x20, 0x01, 0x30};
	static const u8 counts[] = {1, 1, 2, 1, 1};
	static const u8 capacities[] = {1, 1, 2, 4, 4};
	u8 result[5][4] = {{0}};
	unsigned int k, i;
	int ret;
	for (k = 0; k < ARRAY_SIZE(indexes); ++k) {
		u8 command[19] = {0x21, 0x80, 0, 0, 7, 0, 0, 0, 0, 0, 0, 0,
			2, 0, 3, 0x88, indexes[k], counts[k], 0};
		u8 *rx = (u8 *)response + 12;
		unsigned int length;
		u8 check = 0;
		command[18] = 3 ^ 0x88 ^ indexes[k] ^ counts[k];
		memset(request, 0, 4096); memset(response, 0xa5, 4096);
		memcpy(request, command, sizeof(command));
		ret = qcom_scm_qseecom_app_send(app_id, request, 19, response, 12);
		if (ret) { *transport_uncertain = true; return ret; }
		msleep(10);
		if (le32_to_cpu(((__le32 *)request)[0]) != 0x5aa5 ||
		    le32_to_cpu(((__le32 *)request)[2])) return -EPROTO;
		memset(request, 0, 4096); memset(response, 0xa5, 4096);
		((__le32 *)request)[0] = cpu_to_le32(0x8020);
		((__le32 *)request)[2] = cpu_to_le32(capacities[k] + 5);
		ret = qcom_scm_qseecom_app_send(app_id, request, 12,
					response, capacities[k] + 17);
		if (ret) { *transport_uncertain = true; return ret; }
		msleep(10);
		msleep(1);
		print_hex_dump(KERN_INFO, "fpversion info reply: ", DUMP_PREFIX_OFFSET,
			       16, 1, rx, capacities[k] + 5, false);
		if (le32_to_cpu(((__le32 *)request)[0]) != 0x5aa5 ||
		    le32_to_cpu(((__le32 *)request)[2])) return -EPROTO;
		length = ((unsigned int)rx[1] << 8) | rx[2];
		if (length < 2 || length > capacities[k] + 1 || rx[3] != 4 ||
		    (k < 3 && length != counts[k] + 1)) return -EPROTO;
		for (i = 1; i <= length + 2; ++i) check ^= rx[i];
		if (check != rx[length + 3]) return -EBADMSG;
		memcpy(result[k], rx + 4, length - 1);
		pr_info("fpversion: info index=0x%02x bytes=%u value=%02x %02x\n",
			indexes[k], length - 1, result[k][0], result[k][1]);
	}
	if (!result[0][0] || !result[1][0] ||
	    result[2][0] != 0xa5 || result[2][1] != 0x5a) return -EPROTO;
	if (active_scan && (result[0][0] != 40 || result[1][0] != 196))
		return -EPROTO;
	pr_info("fpversion: dimensions=%u,%u product=%u compat_marker=0x5aa5 framing_verified\n",
		result[0][0], result[1][0], result[0][0] * result[1][0]);
	return 0;
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
	bool transport_uncertain = false;

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
	secure_pending = true;
	pr_info("fpversion: beginning LOAD; no automatic retry\n");
	ret = fpq_app_load(image, 0, image_len, &app_id);
	if (ret)
		goto uncertain;
	pr_info("fpversion: LOAD returned app_id=%u\n", app_id);
	ret = qcom_scm_qseecom_app_get_id("fingerprint", &looked_up);
	/* #125 already observed LOAD id=5, LOOKUP -ENOENT, SHUTDOWN(5)
	 * success. The assumed name is not authoritative; APP_START's
	 * validated APP_ID response is. Only that known name miss is allowed.
	 */
	if (ret == -ENOENT) {
		pr_warn("fpversion: known LOOKUP name miss; using successful LOAD app_id=%u\n",
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
	msleep(10);
	print_hex_dump(KERN_INFO, "fpversion ack: ", DUMP_PREFIX_OFFSET,
		       16, 1, request, FP_REQ_SIZE, false);
	print_hex_dump(KERN_INFO, "fpversion rsp: ", DUMP_PREFIX_OFFSET,
		       16, 1, response, FP_RSP_SIZE, false);
	/* The handler sets req[0]=0x5aa5 and req[2]=0. A zero status alone
	 * is not proof of execution because the response starts zeroed.
	 */
	if (le32_to_cpu(((__le32 *)request)[0]) != 0x5aa5 ||
	    le32_to_cpu(((__le32 *)request)[2]) != 0)
		validation = -EPROTO;
	pr_info("fpversion: transport returned; ack_valid=%d status=0x%08x\n",
		!validation, le32_to_cpu(((__le32 *)response)[3]));
	if (!validation && le32_to_cpu(((__le32 *)response)[3]) != 0)
		validation = -EPROTO;
	if (!validation) {
		validation = query_version(app_id, &transport_uncertain);
		if (!validation)
			validation = query_info(app_id, &transport_uncertain);
		if (!validation && active_scan && !status_only && contact_wait_ms) {
			pr_info("fpversion: ready-for-contact wait_ms=%u; version/info complete, awaiting contact before frame gate\n",
				contact_wait_ms);
			msleep(contact_wait_ms);
			pr_info("fpversion: contact-window-complete; starting original bounded frame gate\n");
		}
		if (!validation && active_scan && !status_only && contact_poll_count)
			pr_info("fpversion: ready-for-contact active-polling max_samples=%u interval_ms=100; first query starts now\n",
				contact_poll_count);
		if (!validation)
			validation = status_only ? fp_frame_status(app_id, &transport_uncertain) :
				active_scan ? (contact_poll_count ?
					fp_active_scan_bounded(app_id, &transport_uncertain, contact_poll_count) :
					fp_active_scan(app_id, &transport_uncertain)) : 0;
		if (transport_uncertain) {
			ret = validation;
			goto uncertain;
		}
	}
	ret = fpq_app_shutdown(app_id);
	if (ret)
		goto uncertain;
	secure_pending = false;
	release_buffers();
	module_put(THIS_MODULE);
	pr_info("fpversion: SHUTDOWN succeeded; buffers released; validation=%d\n",
		validation);
	return validation;

uncertain:
	fp_scan_valid = false;
	pr_err("fpversion: secure call/state uncertain: %d; buffers/module retained, reboot before any further fingerprint experiment\n",
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

static ssize_t capture_read(struct file *f, char __user *buf,
			    size_t count, loff_t *off)
{
	ssize_t ret;
	mutex_lock(&probe_lock);
	if (!fp_scan_valid)
		ret = -ENODATA;
	else
		ret = simple_read_from_buffer(buf, count, off, fp_scan_data,
					      sizeof(fp_scan_data));
	mutex_unlock(&probe_lock);
	return ret;
}

static const struct file_operations capture_fops = {
	.owner = THIS_MODULE,
	.read = capture_read,
	.llseek = default_llseek,
};

static int validate_contact_window(void)
{
	if (contact_wait_ms > 15000 ||
	    (contact_poll_count && (contact_poll_count < 10 || contact_poll_count > 200)) ||
	    ((contact_wait_ms || contact_poll_count) && (!active_scan || status_only)) ||
	    (contact_wait_ms && contact_poll_count))
		return -EINVAL;
	return 0;
}

static int __init fpversion_init(void)
{
	struct dentry *file;
	int ret = validate_contact_window();

	if (ret) {
		pr_err("fpversion: contact window requires active_scan without status_only; wait<=15000 or polls10..200, mutually exclusive\n");
		return ret;
	}

	probe_dir = debugfs_create_dir("gaokun3_fpversion", NULL);
	if (IS_ERR(probe_dir))
		return PTR_ERR(probe_dir);
	file = debugfs_create_file("probe", 0200, probe_dir, NULL, &probe_fops);
	if (IS_ERR_OR_NULL(file)) {
		debugfs_remove_recursive(probe_dir);
		return file ? PTR_ERR(file) : -ENOMEM;
	}
	file = debugfs_create_file("capture", 0400, probe_dir, NULL, &capture_fops);
	if (IS_ERR_OR_NULL(file)) {
		debugfs_remove_recursive(probe_dir);
		return file ? PTR_ERR(file) : -ENOMEM;
	}
	pr_info("fpversion: idle; write 'probe' once to gaokun3_fpversion/probe\n");
	return 0;
}

static void __exit fpversion_exit(void)
{
	debugfs_remove_recursive(probe_dir);
}

module_init(fpversion_init);
module_exit(fpversion_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("FTE7001 fixed version/info and init sleep-wake probe");
