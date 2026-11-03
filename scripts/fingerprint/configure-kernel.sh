#!/usr/bin/env bash
# Run in a kernel source tree after the normal Android configuration.
# No builds, module insertion, or device access.
set -euo pipefail
fp_out=${1:?Usage: configure-kernel.sh <kernel-output-directory>}
test -f drivers/firmware/qcom/qcom_qseecom_fpcmd.c || {
    echo 'Apply kernel-apply-patches.sh <tree> --with-fp first.' >&2; exit 1;
}
./scripts/config --file "$fp_out/.config" \
    --enable QCOM_SCM --enable QCOM_QSEECOM --enable GAOKUN3_FPCMD \
    --enable GPIO_CDEV --enable IKCONFIG
make O="$fp_out" ARCH=arm64 olddefconfig
for fp_option in QCOM_SCM QCOM_QSEECOM GAOKUN3_FPCMD GPIO_CDEV IKCONFIG; do
    grep -qx "CONFIG_${fp_option}=y" "$fp_out/.config" || {
        echo "CONFIG_${fp_option} did not remain built-in" >&2; exit 1;
    }
done
