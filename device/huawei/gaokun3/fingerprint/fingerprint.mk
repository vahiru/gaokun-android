# Experimental Class 1 host matcher. Enable explicitly for candidate images.
# The regular release remains unchanged until Android device validation.
ifeq ($(GAOKUN3_FINGERPRINT),true)
GAOKUN3_FP_TA := $(LOCAL_PATH)/firmware/fingerpr.mbn
ifeq ($(wildcard $(GAOKUN3_FP_TA)),)
$(error Fingerprint TA missing: run scripts/fingerprint/prepare-crdroid.py)
endif
ifneq ($(shell sha256sum $(GAOKUN3_FP_TA) | cut -d' ' -f1),b081543c7b6ae4360969007836747601e8c48ec48a8335a5479e45ec0e34355a)
$(error Fingerprint TA SHA256 mismatch)
endif
ifneq ($(shell cd $(LOCAL_PATH)/prebuilt-boot && sha256sum -c fingerprint-kernel.sha256 >/dev/null 2>&1 && echo ok),ok)
$(error Fingerprint kernel missing/stale: prepare a kernel with 0050 + 0212 and CONFIG_GAOKUN3_FPCMD=y)
endif
PRODUCT_PACKAGES += android.hardware.biometrics.fingerprint-service.gaokun3
PRODUCT_COPY_FILES += \
    $(GAOKUN3_FP_TA):$(TARGET_COPY_OUT_VENDOR)/firmware/fingerpr.mbn \
    frameworks/native/data/etc/android.hardware.fingerprint.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.hardware.fingerprint.xml
endif
