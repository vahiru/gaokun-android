# 指纹HAL随镜像构建，服务报告CONVENIENCE（见README.md）。
# TA和Goodix配置模板是专有二进制，不入库；缺失或哈希不符时构建直接失败。
# Android构建不检查内核：CONFIG_GAOKUN3_FPCMD由scripts/kernel-config-android.sh
# 打开，scripts/sync-device-tree.sh第3a步和scripts/release.sh确认内核镜像带着它。
GAOKUN3_FP_TA := $(LOCAL_PATH)/firmware/fingerpr.mbn
ifeq ($(wildcard $(GAOKUN3_FP_TA)),)
$(error Fingerprint TA missing: see device/huawei/gaokun3/firmware/README.md)
endif
ifneq ($(shell sha256sum $(GAOKUN3_FP_TA) | cut -d' ' -f1),b081543c7b6ae4360969007836747601e8c48ec48a8335a5479e45ec0e34355a)
$(error Fingerprint TA SHA256 mismatch)
endif
# 同一个镜像同时支持FocalTech和Goodix的机器；Goodix初始化需要这份模板。
GAOKUN3_FP_GOODIX_CFG := $(LOCAL_PATH)/firmware/gaokun3-goodix-cfg.bin
ifeq ($(wildcard $(GAOKUN3_FP_GOODIX_CFG)),)
$(error Goodix configuration template missing: see device/huawei/gaokun3/firmware/README.md)
endif
ifneq ($(shell sha256sum $(GAOKUN3_FP_GOODIX_CFG) | cut -d' ' -f1),273537378e759cc19b829c6c5a3b5226da603b55f96be6bcdf9a2e66c6c26725)
$(error Goodix configuration template SHA256 mismatch)
endif
PRODUCT_PACKAGES += android.hardware.biometrics.fingerprint-service.gaokun3
# 解锁波纹从电源键处开始（rro/Gaokun3SystemUIFingerprintOverlay）。
PRODUCT_PACKAGES += Gaokun3SystemUIFingerprintOverlay
PRODUCT_COPY_FILES += \
    $(GAOKUN3_FP_TA):$(TARGET_COPY_OUT_VENDOR)/firmware/fingerpr.mbn \
    $(GAOKUN3_FP_GOODIX_CFG):$(TARGET_COPY_OUT_VENDOR)/firmware/gaokun3-goodix-cfg.bin \
    frameworks/native/data/etc/android.hardware.fingerprint.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.hardware.fingerprint.xml
