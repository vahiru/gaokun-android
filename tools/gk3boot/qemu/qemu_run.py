#!/usr/bin/env python3
"""无头跑一次 QEMU aarch64 + AAVMF，收串口输出，直到虚拟机关机或超时。

  qemu_run.py --disk DISK --vars VARS.fd --log OUT.log [--code AAVMF_CODE.fd] [--timeout 600]
              [--inject-on 'keyscan.begin' --inject 'g']
              [--machine-opts acpi=off] [--blkdebug RULES.cfg] [--stop-on PATTERN --stop-delay 8]
              [--cpu cortex-a76] [--trace-int INT.log]
  qemu_run.py --dumpdtb OUT.dtb [--machine-opts acpi=off]     只导出这套机器配置的 dtb 就退出

- 串口接 stdio：原样（含 ANSI 转义）写进 --log；
- 看到 --inject-on 那一行后，往串口敲 --inject 的字符（测探针的按键记录；TerminalDxe 把它变成 ConIn 按键）；
  只在第一次出现时敲 —— 第二次启动（假 Android）不受影响；
- 退出码：QEMU 正常退出（客体 ResetSystem(Shutdown) / 内核关机）→ 0；超时 → 124（并杀掉 QEMU）。
- --stop-on：串口出现这一行后再等 --stop-delay 秒就杀掉 QEMU、按"正常结束"算（退出码 0）。给真 gaokun3 内核用：
  它在 QEMU 的 virt 机器上没有串口驱动（CONFIG_SERIAL_AMBA_PL011 没开），EFI stub 在这里也一行不打，
  只能配合 --trace-int（QEMU -d int）看 CPU 有没有跑进内核虚拟地址。
- --blkdebug：用 QEMU 的 blkdebug 驱动按规则给盘注入读错误（测 misc 读失败时的 fail-open）。
- --machine-opts acpi=off：AAVMF 不发 ACPI、改装固件自己的 DTB 配置表 —— gk3boot 的 DTB 要盖掉它才算数。
- --net-fwd PORT：加一块 virtio-net（QEMU user 网络，客体 10.0.2.15），宿主 127.0.0.1:PORT 转到客体 5554
  （执行端 gk3-fastbootd 的 TCP 传输，端到端场景里宿主 fastboot 从这里连）。
- --on PATTERN --run CMD：串口每【新】出现一次 PATTERN，就在后台 sh -c CMD（环境变量 GK3_ON_N = 第几次，从 1 数）；
  CMD 的退出码不影响 QEMU，测试自己看它留下的文件。
不带 -no-reboot：探针的 ResetSystem(EfiResetCold) 在 QEMU 里就是一次真复位，接着跑下一次启动。
"""
import argparse
import os
import re
import select
import subprocess
import sys
import time

ANSI = re.compile(rb"\x1b\[[0-9;?]*[A-Za-z]|\x1b[()][A-Za-z0-9]|\x1b[=>]")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--disk")
    ap.add_argument("--vars")
    ap.add_argument("--log")
    ap.add_argument("--machine-opts", default="")
    ap.add_argument("--blkdebug")
    ap.add_argument("--stop-on")
    ap.add_argument("--stop-delay", type=float, default=8)
    ap.add_argument("--dumpdtb")
    ap.add_argument("--cpu", default="cortex-a76")
    ap.add_argument("--trace-int")
    ap.add_argument("--code", default="/usr/share/AAVMF/AAVMF_CODE.no-secboot.fd")
    ap.add_argument("--timeout", type=int, default=600)
    ap.add_argument("--inject-on")
    ap.add_argument("--inject", default="g")
    ap.add_argument("--net-fwd", type=int)
    ap.add_argument("--on")
    ap.add_argument("--run")
    a = ap.parse_args()

    machine = "virt" + ("," + a.machine_opts if a.machine_opts else "")
    if a.dumpdtb:
        # 同一套 CPU / 内存 / 设备，只导出 QEMU 生成的 dtb（PCI 设备不进 dtb，所以不用带盘和 pflash）
        subprocess.run(["qemu-system-aarch64", "-M", machine + ",dumpdtb=" + a.dumpdtb, "-cpu", a.cpu,
                        "-smp", "2", "-m", "2048", "-nodefaults", "-no-user-config", "-display", "none"],
                       check=True, capture_output=True)
        print("  dtb：%s（%d 字节）" % (a.dumpdtb, os.path.getsize(a.dumpdtb)))
        return
    if not (a.disk and a.vars and a.log):
        ap.error("--disk --vars --log 都要给")
    drive = "if=none,id=nvm,format=raw,file=" + a.disk
    if a.blkdebug:
        drive = "if=none,id=nvm,format=raw,file.driver=blkdebug,file.config=%s,file.image.filename=%s" % (
            a.blkdebug, a.disk)
    cmd = [
        "qemu-system-aarch64", "-M", machine, "-cpu", a.cpu, "-smp", "2", "-m", "2048",
        "-nodefaults", "-no-user-config", "-display", "none",
        "-drive", "if=pflash,format=raw,unit=0,readonly=on,file=" + a.code,
        "-drive", "if=pflash,format=raw,unit=1,file=" + a.vars,
        # 本机是 NVMe（docs/dev/hardware.md）：设备路径的形状 …/NVMe(…)/HD(…) 与实机一致
        "-drive", drive,
        "-device", "nvme,serial=gk3fixture,drive=nvm",
        # 给 GOP（virtio-gpu）和一个 USB 键盘（ConIn 多一个物理源），探针的显示 / 按键两节才有东西可看
        "-device", "virtio-gpu-pci", "-device", "qemu-xhci", "-device", "usb-kbd",
        "-serial", "stdio", "-monitor", "none",
    ]
    if a.net_fwd:
        cmd += ["-netdev", "user,id=n0,hostfwd=tcp:127.0.0.1:%d-:5554" % a.net_fwd,
                "-device", "virtio-net-pci,netdev=n0,romfile="]   # 不要 iPXE 选项 ROM（容器里没装，固件也用不上）
    if a.trace_int:
        # 记下每一次异常（含 ELR）：真 gaokun3 内核在 virt 上没有串口驱动，"进没进到内核虚拟地址"只能这样看
        cmd += ["-d", "int", "-D", a.trace_int]
    t0 = time.time()
    p = subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, bufsize=0)
    injected = False
    on_seen = 0          # PATTERN 在累计串口文字里出现过几次
    on_text = b""
    kids = []
    stop_at = None
    pending = b""
    rc = None
    with open(a.log, "wb") as log:
        log.write(("# " + " ".join(cmd) + "\n").encode())
        while True:
            if stop_at and time.time() >= stop_at:
                p.kill()
                p.wait()
                log.write(("\n# STOPPED %.0fs after %r (as requested)\n" % (a.stop_delay, a.stop_on)).encode())
                rc = 0
                break
            if time.time() - t0 > a.timeout:
                p.kill()
                p.wait()
                log.write(b"\n# TIMEOUT\n")
                print("✗ QEMU 超时（%d 秒）" % a.timeout, file=sys.stderr)
                rc = 124
                break
            r, _, _ = select.select([p.stdout], [], [], 0.5)
            if r:
                b = os.read(p.stdout.fileno(), 65536)
                if not b:
                    p.wait()
                    rc = 0
                    break
                log.write(b)
                log.flush()
                pending = (pending + ANSI.sub(b"", b))[-8192:]
                if a.stop_on and not stop_at and a.stop_on.encode() in pending:
                    stop_at = time.time() + a.stop_delay
                if a.on:
                    on_text += ANSI.sub(b"", b)
                    n = on_text.count(a.on.encode())
                    while on_seen < n:
                        on_seen += 1
                        env = dict(os.environ, GK3_ON_N=str(on_seen))
                        kids.append(subprocess.Popen(["sh", "-c", a.run], env=env))
                        log.write(("\n# RUN #%d (%r seen at %.1fs)\n" % (on_seen, a.on, time.time() - t0)).encode())
                    # 只留最后一段，免得无限长；保证跨块的 PATTERN 还能被数到
                    keep = len(a.on) - 1
                    if len(on_text) > 65536:
                        cut = len(on_text) - keep
                        on_seen -= on_text[:cut].count(a.on.encode())
                        on_text = on_text[cut:]
                if a.inject_on and not injected and a.inject_on.encode() in pending:
                    time.sleep(0.5)
                    p.stdin.write(a.inject.encode())
                    p.stdin.flush()
                    injected = True
                    log.write(("\n# INJECTED %r at %.1fs\n" % (a.inject, time.time() - t0)).encode())
            elif p.poll() is not None:
                rc = 0
                break
        for k in kids:
            try:
                k.wait(timeout=30)
            except subprocess.TimeoutExpired:
                k.kill()
        log.write(("\n# qemu exit=%s after %.1fs\n" % (p.returncode, time.time() - t0)).encode())
    print("  QEMU 结束：%.1f 秒，退出码 %s%s%s" % (time.time() - t0, p.returncode, "，已注入按键" if injected else "",
                                             "，按 --stop-on 主动停下" if stop_at and rc == 0 else ""))
    sys.exit(rc)


if __name__ == "__main__":
    main()
