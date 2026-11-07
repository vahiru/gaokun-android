#!/usr/bin/env python3
"""Offline integration checks; do not substitute for an Android HAL build."""
import hashlib
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import xml.etree.ElementTree as ET
import zipfile

ROOT = Path(__file__).resolve().parents[2]
FP = ROOT / 'device/huawei/gaokun3/fingerprint'

with tempfile.TemporaryDirectory(prefix='gk3-fp-integration-') as tmp:
    tmp = Path(tmp)
    dest = tmp / 'kernel/drivers/firmware/qcom'
    dest.mkdir(parents=True)
    for name in ('Makefile', 'Kconfig'):
        shutil.copyfile(ROOT / 'refs/linux-v7.2-rc2-git/drivers/firmware/qcom' / name, dest / name)
    for name in ('kernel/fingerprint/0002-firmware-qcom-gaokun3-fingerprint-ioctl.patch',
                 'kernel/fingerprint/0003-firmware-qcom-gaokun3-fpcmd-add-the-Goodix-GDIX5125-sensor.patch'):
        patch = ROOT / 'patches' / name
        subprocess.run(['git', 'apply', '--check', str(patch)], cwd=tmp / 'kernel', check=True)
        subprocess.run(['git', 'apply', str(patch)], cwd=tmp / 'kernel', check=True)
    assert (dest / 'gaokun3_fpcmd.h').read_bytes() == (FP / 'gaokun3_fpcmd.h').read_bytes()
    source = (dest / 'qcom_qseecom_fpcmd.c').read_text()
    assert '/vendor/firmware/fingerpr.mbn' in source
    assert '/vendor/firmware/gaokun3-goodix-cfg.bin' in source
    assert 'static int fp_open(' in source and 'static int fp_release(' in source
    assert 'fp_shutdown_failed = true;' in source
    assert (dest / 'gaokun3_fpcmd_goodix.h').exists()
    print('PASS: kernel patch application (fingerprint/0002 + 0003), UAPI equality, exclusive owner/shutdown guard')

    # Check implemented methods against the real frozen Android V4 API snapshot.
    api = (ROOT / 'refs/a17/hardware/interfaces/biometrics/fingerprint/aidl/aidl_api'
           '/android.hardware.biometrics.fingerprint/4/android/hardware/biometrics/fingerprint/ISession.aidl')
    required = set(re.findall(r'^  [\w.]+ (\w+)\(', api.read_text(), re.M))
    session = (FP / 'Service.cpp').read_text().split('class Session final')[1].split('class Fingerprint final')[0]
    implemented = set(re.findall(r'ScopedAStatus (\w+)\(', session))
    assert required <= implemented, required - implemented
    manifest = ET.parse(FP / 'fingerprint-gaokun3.xml').getroot()
    assert manifest.find('hal/version').text == '4'
    print('PASS: all frozen AIDL V4 session methods present; VINTF version agrees (source check only)')

    archive = ROOT / 'gaokun3-fingerprint-driver-handoff-20261101.zip'
    gfspi = ROOT / 'gfspi.dll'
    if archive.exists() and gfspi.exists():
        tree = tmp / 'device'
        script = ROOT / 'scripts/fingerprint/prepare-crdroid.py'
        subprocess.run(['python3', str(script), '--device-tree', str(tree)], check=True)
        ta = tree / 'firmware/fingerpr.mbn'
        expected = hashlib.sha256(ta.read_bytes()).hexdigest()
        bad = tmp / 'bad.zip'
        with zipfile.ZipFile(archive) as src, zipfile.ZipFile(bad, 'w') as out:
            for member in src.namelist():
                data = src.read(member)
                if member == 'firmware/fingerpr.mbn':
                    data = bytes([data[0] ^ 1]) + data[1:]
                out.writestr(member, data)
        result = subprocess.run(['python3', str(script), '--device-tree', str(tree),
                                 '--handoff', str(bad)], capture_output=True)
        assert result.returncode != 0
        assert hashlib.sha256(ta.read_bytes()).hexdigest() == expected
        cfg = tree / 'firmware/gaokun3-goodix-cfg.bin'
        staged = cfg.read_bytes()
        dll = gfspi.read_bytes()
        # The template bytes occur more than once in gfspi.dll: damage them all.
        damaged = staged[:100] + bytes([staged[100] ^ 1]) + staged[101:]
        wrong = tmp / 'wrong.dll'
        for data in (bytes([dll[0] ^ 1]) + dll[1:], dll.replace(staged, damaged)):
            assert data != dll
            wrong.write_bytes(data)
            result = subprocess.run(['python3', str(script), '--device-tree', str(tree),
                                     '--gfspi', str(wrong)], capture_output=True)
            assert result.returncode != 0 and cfg.read_bytes() == staged
        print('PASS: handoff/template staging and corrupt input rejection leave existing input intact')
        makefile = tmp / 'gate.mk'
        makefile.write_text('LOCAL_PATH := ' + str(tree) + '\n'
                            'TARGET_COPY_OUT_VENDOR := vendor\n'
                            'include ' + str(FP / 'fingerprint.mk') + '\n'
                            '.PHONY: check\ncheck:\n\t@test "$(PRODUCT_PACKAGES)" = "$(EXPECTED)"\n')
        def make(*args):
            return subprocess.run(['make', '-f', str(makefile), 'check', *args], capture_output=True)
        assert make().returncode == 0
        assert make('GAOKUN3_FINGERPRINT=true').returncode != 0
        boot = tree / 'prebuilt-boot'
        boot.mkdir()
        image = boot / 'vmlinuz.efi'
        image.write_bytes(b'synthetic kernel fixture, not bootable')
        proof = boot / 'fingerprint-kernel.sha256'
        proof.write_text(hashlib.sha256(image.read_bytes()).hexdigest() + '  vmlinuz.efi\n')
        assert make('GAOKUN3_FINGERPRINT=true',
                    'EXPECTED=android.hardware.biometrics.fingerprint-service.gaokun3 '
                    'Gaokun3SystemUIFingerprintOverlay').returncode == 0
        cfg_bytes = cfg.read_bytes()
        cfg.unlink()
        assert make('GAOKUN3_FINGERPRINT=true').returncode != 0
        cfg.write_bytes(cfg_bytes[:-1] + bytes([cfg_bytes[-1] ^ 1]))
        assert make('GAOKUN3_FINGERPRINT=true').returncode != 0
        cfg.write_bytes(cfg_bytes)
        image.write_bytes(b'stale kernel fixture')
        assert make('GAOKUN3_FINGERPRINT=true').returncode != 0
        print('PASS: product disabled by default, missing/stale kernel and Goodix template rejected, pinned inputs accepted')
        # Synthetic extractor/image fixtures exercise staging rollback logic only.
        # They do not establish that any real EFI image has the required config.
        extractor = tmp / 'fake-kernel/scripts/extract-ikconfig'
        extractor.parent.mkdir(parents=True)
        extractor.write_text("#!/bin/sh\nprintf '%s\\n' CONFIG_QCOM_SCM=y "
                             "CONFIG_QCOM_QSEECOM=y CONFIG_GAOKUN3_FPCMD=y CONFIG_GPIO_CDEV=y\n")
        candidate = tmp / 'synthetic.efi'
        candidate.write_bytes(b'new synthetic EFI fixture')
        command = ['python3', str(script), '--device-tree', str(tree),
                   '--kernel-tree', str(extractor.parents[1]), '--kernel-image', str(candidate)]
        old = image.read_bytes()
        result = subprocess.run(command, capture_output=True)
        assert result.returncode == 0, result.stderr.decode()
        backup = boot / 'vmlinuz.efi.before-fingerprint'
        assert backup.read_bytes() == old
        assert image.read_bytes() == candidate.read_bytes()
        candidate.write_bytes(b'another synthetic EFI fixture')
        assert subprocess.run(command, capture_output=True).returncode != 0
        assert backup.read_bytes() == old
        assert image.read_bytes() == b'new synthetic EFI fixture'
        print('PASS: staging preserves rollback kernel and refuses to overwrite an existing backup')


    else:
        print('SKIP: private handoff archive or gfspi.dll unavailable; staging tests require local input')
print('PASS: offline integration checks (no Android compile or device validation)')
