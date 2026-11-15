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
    # A checkout that builds the ROM already has both inputs staged here (TA from the public
    # driver package, QcTreeExtOem8280.cab); the private handoff ZIP is optional.
    repo_fw = ROOT / 'device/huawei/gaokun3/firmware'
    script = ROOT / 'scripts/fingerprint/prepare-crdroid.py'
    tree = tmp / 'device'
    ta = tree / 'firmware/fingerpr.mbn'
    cfg = tree / 'firmware/gaokun3-goodix-cfg.bin'
    if gfspi.exists() and (archive.exists() or (repo_fw / 'fingerpr.mbn').exists()):
        ta_args = [] if archive.exists() else ['--ta', str(repo_fw / 'fingerpr.mbn')]
        subprocess.run(['python3', str(script), '--device-tree', str(tree), *ta_args], check=True)
        expected = hashlib.sha256(ta.read_bytes()).hexdigest()
        if archive.exists():
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
        staged = cfg.read_bytes()
        dll = gfspi.read_bytes()
        # The template bytes occur more than once in gfspi.dll: damage them all.
        damaged = staged[:100] + bytes([staged[100] ^ 1]) + staged[101:]
        wrong = tmp / 'wrong.dll'
        for data in (bytes([dll[0] ^ 1]) + dll[1:], dll.replace(staged, damaged)):
            assert data != dll
            wrong.write_bytes(data)
            result = subprocess.run(['python3', str(script), '--device-tree', str(tree),
                                     '--gfspi', str(wrong), *ta_args], capture_output=True)
            assert result.returncode != 0 and cfg.read_bytes() == staged
        # --ta takes the TA straight from the driver package; the handoff ZIP is then not read.
        ta_file = tmp / 'fingerpr.mbn'
        ta_bytes = ta.read_bytes()
        ta_file.write_bytes(ta_bytes)
        tree_ta = tmp / 'device-ta'
        command = ['python3', str(script), '--device-tree', str(tree_ta), '--ta', str(ta_file),
                   '--handoff', str(tmp / 'absent.zip')]
        subprocess.run(command, check=True)
        assert (tree_ta / 'firmware/fingerpr.mbn').read_bytes() == ta_bytes
        ta_file.write_bytes(bytes([ta_bytes[0] ^ 1]) + ta_bytes[1:])
        assert subprocess.run(command, capture_output=True).returncode != 0
        assert (tree_ta / 'firmware/fingerpr.mbn').read_bytes() == ta_bytes
        print('PASS: handoff/--ta/template staging and corrupt input rejection leave existing input intact')
    elif (repo_fw / 'fingerpr.mbn').exists() and (repo_fw / 'gaokun3-goodix-cfg.bin').exists():
        cfg.parent.mkdir(parents=True)
        shutil.copyfile(repo_fw / 'fingerpr.mbn', ta)
        shutil.copyfile(repo_fw / 'gaokun3-goodix-cfg.bin', cfg)
        print('SKIP: gfspi.dll unavailable; staging tests skipped, using the staged firmware/ inputs')

    if ta.exists() and cfg.exists():
        makefile = tmp / 'gate.mk'
        makefile.write_text('LOCAL_PATH := ' + str(tree) + '\n'
                            'TARGET_COPY_OUT_VENDOR := vendor\n'
                            'include ' + str(FP / 'fingerprint.mk') + '\n'
                            '.PHONY: check copies\ncheck:\n\t@test "$(PRODUCT_PACKAGES)" = "$(EXPECTED)"\n'
                            'copies:\n\t@echo $(PRODUCT_COPY_FILES)\n')
        def make():
            # With the expected package list, only fingerprint.mk's $(error) can fail the run.
            return subprocess.run(['make', '-f', str(makefile), 'check',
                                   'EXPECTED=android.hardware.biometrics.fingerprint-service.gaokun3 '
                                   'Gaokun3SystemUIFingerprintOverlay'], capture_output=True)
        assert make().returncode == 0
        copies = subprocess.run(['make', '-f', str(makefile), 'copies'], capture_output=True,
                                check=True).stdout.decode().split()
        assert sorted(copies) == sorted([
            f'{ta}:vendor/firmware/fingerpr.mbn',
            f'{cfg}:vendor/firmware/gaokun3-goodix-cfg.bin',
            'frameworks/native/data/etc/android.hardware.fingerprint.xml:'
            'vendor/etc/permissions/android.hardware.fingerprint.xml']), copies
        for pinned in (ta, cfg):
            data = pinned.read_bytes()
            pinned.unlink()
            result = make()
            assert result.returncode != 0 and b'missing' in result.stderr, result.stderr
            pinned.write_bytes(data[:-1] + bytes([data[-1] ^ 1]))
            result = make()
            assert result.returncode != 0 and b'SHA256 mismatch' in result.stderr, result.stderr
            pinned.write_bytes(data)
        assert make().returncode == 0
        print('PASS: product always installs the HAL, TA, template and feature XML; missing or altered TA/Goodix template rejected')
    else:
        print('SKIP: no fingerprint TA/Goodix template available (firmware/ or handoff + gfspi.dll); '
              'fingerprint.mk checks require local input')
print('PASS: offline integration checks (no Android compile or device validation)')
