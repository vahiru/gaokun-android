<#
    Stage 0 -- Windows preflight (run on the Ego, not on the build machine)

    Purpose, now that the plan is USB-boot rather than an internal install:
      1. Pin down the machine variant (year / LTE / panel vendor / BIOS revision).
         These are easy to read in Windows and awkward to get afterwards.
      2. Confirm Secure Boot state -- the only blocker for booting our USB image.

    Read-only. Touches nothing.

    Usage -- run as Administrator:
        powershell -ExecutionPolicy Bypass -File windows-preflight.ps1

    NOTE: this file must stay UTF-8 *with BOM*. Windows PowerShell 5.1 reads .ps1
    as the system ANSI codepage when the BOM is absent, which mangles every
    non-ASCII character and breaks string parsing.
#>

$ErrorActionPreference = 'Continue'
$stamp  = Get-Date -Format 'yyyyMMdd-HHmmss'
$report = Join-Path $PSScriptRoot "ego-preflight-$stamp.txt"
$lines  = New-Object System.Collections.ArrayList

function Say($t)  { Write-Host $t; [void]$lines.Add([string]$t) }
function Head($t) { Say ""; Say ("=" * 66); Say "  $t"; Say ("=" * 66) }

$isAdmin = ([Security.Principal.WindowsPrincipal] `
    [Security.Principal.WindowsIdentity]::GetCurrent()
    ).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)

Say "Stage 0 Windows preflight -- $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss')"
if (-not $isAdmin) {
    Say "!! 不是管理员权限运行：Secure Boot 检查会失败。请用管理员 PowerShell 重跑。"
}

# ------------------------------------------------------------ 1. 机器身份
Head "1. 机器身份"
$cs   = Get-CimInstance Win32_ComputerSystem
$bios = Get-CimInstance Win32_BIOS
$os   = Get-CimInstance Win32_OperatingSystem

Say ("厂商      : " + $cs.Manufacturer)
Say ("型号      : " + $cs.Model)
Say ("SKU       : " + $cs.SystemSKUNumber)
Say ("序列号    : " + $bios.SerialNumber)
Say ("内存GB    : " + [math]::Round($cs.TotalPhysicalMemory / 1GB, 1))
Say ("架构      : " + $os.OSArchitecture)
Say ""
Say ("BIOS 版本 : " + $bios.SMBIOSBIOSVersion + "     <<< 关键: 216 还是 217")
Say ("BIOS 日期 : " + $bios.ReleaseDate)
Say ""
Say "  refs/matebook-e-go-linux/docs/ 里有 216 和 217 两套 DSDT 和差异分析"
Say "  (ACPI_TOUCH_DIFF_216_vs_217.md)。两个版本触摸行为不同，这个号一定要记下。"

# -------------------------------------------------------------- 2. 变体
Head "2. 机器变体"

Say "--- 摄像头: 前摄型号决定年份 ---"
Say "    hi846  = 2022 款"
Say "    s5k4h7 = 2023 款"
Say ""
$cams = Get-PnpDevice -Class 'Camera','Image' -ErrorAction SilentlyContinue |
        Where-Object { $_.Present }
if ($cams) {
    foreach ($c in $cams) {
        Say ("    " + $c.FriendlyName)
        Say ("        " + $c.InstanceId)
    }
} else {
    Say "    未枚举到。可在设备管理器的『相机』/『图像设备』下人工确认。"
}

Say ""
Say "--- 蜂窝: 判断是不是 LTE 版 ---"
# 注意 \b 词边界：没有它的话 'LTE' 会命中 "FocaLTEch"（指纹驱动），造成误报。
$wwan = Get-PnpDevice -ErrorAction SilentlyContinue | Where-Object {
    $_.Present -and $_.Class -ne 'Biometric' -and
    ($_.Class -eq 'Modem' -or
     $_.FriendlyName -match 'WWAN|\bLTE\b|Mobile Broadband|\bWCDMA\b|Cellular')
}
if ($wwan) {
    foreach ($w in $wwan) { Say ("    " + $w.FriendlyName + "  [" + $w.Class + "]") }
    Say ""
    Say "    !! 检出蜂窝设备，可能是 LTE 版。"
    Say "       『无 modem，libqril/qrild/qrtr 全跳过』假设需要复核。"
} else {
    Say "    未检出蜂窝设备 -> 非 LTE 版，与预期一致。"
}

Say ""
Say "--- 触摸固件 cfg 版本: 定年份 + 面板厂 ---"
Say "    43=2023  41=2022  3e=2022LTE ; 次字节 07=CSOT 08=BOE 09=CSOT-new"
Say ""
$thp = 'C:\ProgramData\Huawei\HuaweiTHP'
if (Test-Path $thp) {
    $logs = Get-ChildItem $thp -Filter 'hx_hal_log_*' -ErrorAction SilentlyContinue |
            Sort-Object LastWriteTime -Descending | Select-Object -First 3
    if ($logs) {
        foreach ($l in $logs) {
            Say ("    " + $l.Name + "   " + $l.LastWriteTime)
            $hit = Get-Content $l.FullName -TotalCount 200 -ErrorAction SilentlyContinue |
                   Select-String -Pattern 'cfg|version|fw_ver|panel|CSOT|BOE' |
                   Select-Object -First 8
            foreach ($h in $hit) { Say ("        | " + $h.Line.Trim()) }
        }
    } else {
        Say "    目录在，但没有 hx_hal_log_* 文件。"
    }
} else {
    Say ("    " + $thp + " 不存在（华为触控服务可能未装，或路径不同）。")
}

# --------------------------------------------------- 3. USB 启动的前提
Head "3. USB 启动前提"

Say "--- Secure Boot ---"
try {
    $sb = Confirm-SecureBootUEFI -ErrorAction Stop
    if ($sb) {
        Say "    已启用 -> 必须先关掉才能引导我们的 USB 镜像。"
        Say "    开机按 F2 进 UEFI -> Secure Boot 设为 Disable -> 保存重启。"
    } else {
        Say "    已关闭 -> 满足 USB 启动条件。"
    }
} catch {
    Say ("    查询失败（需要管理员权限）: " + $_.Exception.Message)
}

Say ""
Say "--- 磁盘现状（仅记录，USB 启动不会改动它）---"
Get-Disk | ForEach-Object {
    Say ("    磁盘{0}: {1}  {2} GB  {3}" -f `
        $_.Number, $_.FriendlyName, [math]::Round($_.Size / 1GB, 1), $_.PartitionStyle)
}
Say ""
Say "    走 USB 启动就不碰内置盘，所以 BitLocker 和分区都不构成阻碍。"
Say "    仅当 USB 启动失败、退回内置安装时，才需要先处理 BitLocker。"

# BitLocker：仅作记录，USB 路线下不是阻塞项
try {
    $bl = Get-BitLockerVolume -ErrorAction Stop
    Say ""
    Say "    BitLocker 现状（仅供参考）:"
    foreach ($v in $bl) {
        Say ("      {0}  保护={1}  加密={2}%" -f `
            $v.MountPoint, $v.ProtectionStatus, $v.EncryptionPercentage)
    }
} catch {
    Say ""
    Say "    BitLocker 状态未知（需要管理员权限）。USB 路线下无影响。"
}

# ------------------------------------------------------------ 4. 结论
Head "4. 下一步"
Say "  1) 若 Secure Boot 还开着 -> F2 进 UEFI 关掉"
Say "  2) 在编译机上把 ubuntu-26.04-gaokun3.img.zst 解压并写入 U 盘（>=32GB, USB3）"
Say "  3) Ego 关机 -> 插 U 盘 -> 开机选 USB 启动"
Say "  4) 进系统后运行 collect-hw-inventory.sh"
Say ""
Say "  已知风险：linux-gaokun README:86-87 提到开机前插 Type-C 设备可能触发"
Say "  UCSI 缺陷导致自动重启。USB 启动必然是开机前插着的，若反复自动重启，"
Say "  基本可判定是这个问题，那时再退回内置安装方案。"

# --------------------------------------------------------------- 存盘
# UTF8 with BOM，保证拷回去用任何编辑器打开都不乱码
$lines | Out-File -FilePath $report -Encoding utf8
Write-Host ""
Write-Host ("报告已保存: " + $report)
Write-Host "把它拷回项目机。"
