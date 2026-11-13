<#
.SYNOPSIS
  gaokun3（华为 MateBook E Go）免 U 盘安装：在 Windows 里把 Android 安装器放上内置盘，下次开机进安装器。
  gaokun3 USB-free install: put the Android installer on the internal disk from Windows; the next boot starts it.

.DESCRIPTION
  以管理员身份运行（双击同目录的 gaokun3-setup.cmd）。每一步之前先检查、先说清楚，真动盘之前要你输入 YES：
    1. 预检：型号 GK-W7X、Windows on ARM、UEFI；BitLocker 开着时先要你确认拿得到恢复密钥、再暂停它 2 次重启
       （排在安全启动之前：关安全启动本身就可能触发恢复密钥）；然后要求安全启动已关
    2. 让 Windows 自己"压缩卷"（默认 D:），【只】缩出安装器自己要的那一点（按安装包的实际内容算，约 0.5–2 GiB）。
       给 Android 的空间到安装器里再分（用户 2026-09-27："更改磁盘应该在安装的时候进行，安装安装器应该仅划分自己需要的空间"）。
       例外：D: 加了密（BitLocker / 设备加密）时安装器缩不了它、只有 Windows 能 —— 那时会问你要不要现在就缩出给 Android 的空间。
       安装器要缩 D: 的话 Windows 的"快速启动"必须关（它让分区停在休眠状态，安装器会拒绝缩）—— 开着就问你、关掉，-Uninstall 恢复
    3. 在缩出来的空间【开头】建 FAT32 分区 GK3LIVE，放 live 系统（和可选的安装载荷、WiFi 配置）
    4. ESP 上放 systemd-boot、内核、dtb、initramfs 和一个启动项（\EFI\gaokun3\、\loader\entries\gaokun3-live.conf）
    5. bcdedit 设"只下一次"从它启动 —— 不改默认启动项；不想装了，重启就回 Windows
  之后在安装器里先"缩小现有分区腾出空间"（缩 D:），再选"保留现有系统"；已经替 Android 缩出了空间的话直接选后者。

  -Uninstall 撤掉以上全部（在 Android 装上之前；装上之后只撤启动项与 ESP 上的安装器文件，
  【不碰】EFI\gk3boot、EFI\systemd、Android 的启动项，也不重新打开快速启动 —— 双系统时它必须关着）。

  同一个脚本也是常驻的【Windows 伴随工具】（见 tools/gk3boot/README.md）——【预览】：
    尚未在真机 Windows 上实测（已在容器里跑过单元测试）。
    装到 %ProgramFiles%\gaokun3、开始菜单 gaokun3 文件夹、计划任务 \gaokun3\*（SYSTEM）。免 U 盘安装的最后一步会自动装；
    从 U 盘装的用户双击 U 盘上 gaokun3-windows\gaokun3-setup.cmd（不带参数 = 安装伴随工具）。
      -InstallCompanion           安装 / 刷新伴随工具（快速启动一律关；休眠按 -Hibernate 问你）
      -RepairBoot [-Check]        体检；\EFI\Boot\bootaa64.efi 被 Windows 换回 bootmgfw 时，先暂停 BitLocker 1 次重启、再拷回 systemd-boot
      -RebootToAndroid            写 LoaderEntryOneShot = loader.conf 的 default，然后重启（Windows 能否写这个变量尚未在真机验证）
      -SetDefault android|windows 开机默认进哪个系统（写 / 删 LoaderEntryDefault）
      -SuspendBitLocker           只暂停 BitLocker 2 次重启，别的都不碰（U 盘路径"关安全启动之前"那一步用）
      -RemoveAndroid              卸载 Android、回到纯 Windows（先还原引导，再删 ESP 上的东西、变量、分区，再把 D: 扩回去）
      -WindowsPreset on|off       Windows 里的重启都回 Windows（默认关）
    计划任务与开始菜单内部用（不要手动传）：-BootCheck、-Notify、-Trigger <动作>、-TaskAction <动作>

.NOTES
  这个脚本是在【没有 Windows 的机器上】写的（仓库作者那台的 Windows 已在 2026-08-20 抹掉）。
     纯逻辑（WiFi 配置转换、PSK 推导、大小计算、bcdedit 输出解析）在 PowerShell 容器里有单元测试
     （scripts/windows/test-setup.ps1）；Resize-Partition / New-Partition / mountvol / bcdedit 这些
     Windows 专有的步骤尚未经真机实测。见 scripts/live/README.md。
  必须兼容 Windows 自带的 PowerShell 5.1（不用 ?:、??、&& 这些 7 才有的语法），
     文件必须是 UTF-8 带 BOM —— 否则 5.1 按系统代码页读，中文全是乱码。
#>
[CmdletBinding()]
param(
    # 现在就替 Android 缩出多少 GiB。默认 0：只划安装器自己的空间，Android 的到安装器里分。
    # 不给这个参数、而 D: 加了密时会问（安装器缩不了加密卷）。给的话至少 24（双系统约要 21.2 GiB，gk3_plan 实测）
    [ValidateScript({ $_ -eq 0 -or ($_ -ge 24 -and $_ -le 2048) })][int]$AndroidGiB = 0,
    # 从哪个卷缩。出厂有独立的 D:（Data，336.6 GiB，见 docs/dev/hardware.md），缩它比缩 C: 稳
    [ValidatePattern('^[A-Za-z]$')][string]$ShrinkDrive = 'D',
    # 安装器分区（GK3LIVE）的大小（MiB）。默认 0 = 按安装包里的实际内容算（Get-LiveMiB：live 约 210 MiB，带载荷再加 1.3 GiB）
    [ValidateScript({ $_ -eq 0 -or ($_ -ge 256 -and $_ -le 16384) })][int]$LiveMiB = 0,
    # current：只带当前连着的 WiFi；all：带上所有保存过的；none：不带（到安装器里再连）
    [ValidateSet('current', 'all', 'none')][string]$Wifi = 'current',
    # 不用 bcdedit 的"只下一次"，改为接管 ESP 的回落路径 \EFI\Boot\bootaa64.efi（原件留 .before-gaokun3）
    # —— 这是本机 2026-08-20 在 Windows 还在盘上时实测过的机制（见 docs/dev/hardware.md）；
    #    bcdedit 的 bootsequence 在这台机器的固件上还没验证过。开机会出 systemd-boot 菜单，Windows 在菜单里
    [switch]$UseFallbackPath,
    [switch]$Uninstall,
    [switch]$SkipModelCheck,
    # 不问 YES（给自动化用；人来跑请别加）
    [switch]$Yes,
    [switch]$NoReboot,
    # ── Windows 伴随工具（§4.9.15，预览）──
    [switch]$InstallCompanion,
    [switch]$RepairBoot,
    # 与 -RepairBoot 连用：只出报告，什么都不改
    [switch]$Check,
    [switch]$RebootToAndroid,
    [ValidateSet('android', 'windows')][string]$SetDefault,
    [switch]$SuspendBitLocker,
    [switch]$RemoveAndroid,
    # 与 -RemoveAndroid 连用：删分区之后不把 D: 扩回去
    [switch]$NoExtend,
    [ValidateSet('on', 'off')][string]$WindowsPreset,
    # U24：双系统时要不要关掉整个休眠。ask = 问你（-Yes 时按 keep）；off = 关；keep = 不动（只关快速启动）
    [ValidateSet('ask', 'off', 'keep')][string]$Hibernate = 'ask',
    # 以下给计划任务 / 开始菜单项用
    [switch]$BootCheck,
    [switch]$Notify,
    [ValidateSet('RebootToAndroid', 'DefaultAndroid', 'DefaultWindows')][string]$Trigger,
    [ValidateSet('RebootToAndroid', 'DefaultAndroid', 'DefaultWindows', 'WindowsShutdown')][string]$TaskAction
)
Set-StrictMode -Version 2
$ErrorActionPreference = 'Stop'
# 函数里的 $PSBoundParameters 是函数自己的 —— 在脚本层先记下"用户有没有给 -AndroidGiB"
$script:AndroidGiBGiven = $PSBoundParameters.ContainsKey('AndroidGiB')

$script:UseZh = (Get-UICulture).Name -like 'zh*'
$EspType    = '{c12a7328-f81f-11d2-ba4b-00a0c93ec93b}'
$BasicType  = '{ebd0a0a2-b9e5-4433-87c0-68b6b72699c7}'
$LiveLabel  = 'GK3LIVE'
$EntryTitle = 'gaokun3 installer'
# ESP 要留的空间：Android 150 MiB（installer-lib.sh 的 GK3_ESP_NEED_MIB）+ 这里放的安装器约 20 MiB
$EspNeedMiB = 170
# 压缩之后 Windows 至少还要剩这么多空闲（在"最小能缩到"之上）—— 缩到贴底，Windows 就没地方更新了
$WindowsKeepGiB = 10
# C:\ProgramData\gaokun3（用 .NET 取而不用 $env:ProgramData：后者在 Linux 上是空的，容器里的单元测试一 dot-source 就会炸）
$DataDir    = Join-Path ([Environment]::GetFolderPath('CommonApplicationData')) 'gaokun3'
$StateFile  = Join-Path $DataDir 'windows-setup.json'
# 快速启动的开关（1 = 开）。关掉它不影响休眠本身
$FastStartupKey = 'HKLM:\SYSTEM\CurrentControlSet\Control\Session Manager\Power'
# 休眠总开关（HibernateEnabled，1 = 开；改它走 powercfg /hibernate on|off，不直接写注册表）
$PowerKey   = 'HKLM:\SYSTEM\CurrentControlSet\Control\Power'

# ── Windows 伴随工具（§4.9.15）的常量 ──
$ToolVersion = '1.0-preview'
# systemd-boot 的厂商 GUID（LoaderEntryDefault / LoaderEntryOneShot 都在它下面；与 gk3boot.c 的 gk3_guid_loader、
# scripts/boot-oneshot.sh 同一个）。属性 7 = NV | BS | RT
$LoaderGuid = '{4a67b082-0a4c-41cf-b6c7-440b29bb8c4f}'
$LoaderVarAttr = 7
# Android 的分区：安装器建的名字与类型（installer-lib.sh:409 的名单；GK3_TYPE_DATA=8300 = Linux filesystem）
$AndroidPartNames = @('misc', 'metadata', 'boot_a', 'boot_b', 'super', 'gk3rescue', 'userdata')
$LinuxFsType = '0fc63daf-8483-4772-8e79-3d69d8477de4'
# misc 认不出内容（新装机器可能全零），只按名字 + 大小认：不超过 GK3_MISC_MIB（installer-lib.sh:50）
$MiscMaxMiB = 4
function Get-SpecialDir([string]$Name, [string]$Fallback) {
    # Linux 上（容器里的单元测试）这些特殊目录是空串，Join-Path 碰空串会抛 —— 给个占位，测试里再改
    $p = [Environment]::GetFolderPath($Name)
    if (-not $p) { $p = Join-Path ([IO.Path]::GetTempPath()) $Fallback }
    return $p
}
$InstallDir    = Join-Path (Get-SpecialDir 'ProgramFiles' 'gk3-ProgramFiles') 'gaokun3'
$StartMenuDir  = Join-Path (Get-SpecialDir 'CommonPrograms' 'gk3-StartMenu') 'gaokun3'
$CompanionFile = Join-Path $DataDir 'companion.json'
$StatusFile    = Join-Path $DataDir 'boot-check.json'
$LogFile       = Join-Path $DataDir 'companion.log'
$TaskFolder    = '\gaokun3\'
$TaskNames     = @('BootCheck', 'Notify', 'RebootToAndroid', 'DefaultAndroid', 'DefaultWindows', 'WindowsShutdown')
# 这个脚本自己在哪（被 dot-source 时就是被 source 的那个文件）
$SelfPath = $PSCommandPath
$FwPrivOk = $false
# 固件变量与读裸盘：P/Invoke。只用 C# 5 的语法（Windows PowerShell 5.1 的 Add-Type 用 .NET Framework 自带的编译器）。
#   GetFirmwareEnvironmentVariableExW / SetFirmwareEnvironmentVariableExW：
#     https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-setfirmwareenvironmentvariableexw
#     要 SE_SYSTEM_ENVIRONMENT_NAME 特权（SeSystemEnvironmentPrivilege，管理员令牌里有但默认没启用 → AdjustTokenPrivileges）；
#     nSize = 0 表示删除。⚠️ 本机（高通 sc8280xp，变量服务经 TZ）上 Windows 能不能写 systemd-boot 厂商 GUID 下的变量【未验证】（D4/D6）
#   读裸盘：.NET Framework 的 FileStream 不肯打开 \\.\ 设备路径，所以先 CreateFileW 拿句柄；按 4 KiB 对齐读。【未在 Windows 上跑过】
$NativeSource = @'
using System;
using System.IO;
using System.Runtime.InteropServices;
using Microsoft.Win32.SafeHandles;

public static class Gk3Native {
    [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    static extern uint GetFirmwareEnvironmentVariableExW(string lpName, string lpGuid, byte[] pBuffer, uint nSize, out uint pdwAttribubutes);
    [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    [return: MarshalAs(UnmanagedType.Bool)]
    static extern bool SetFirmwareEnvironmentVariableExW(string lpName, string lpGuid, byte[] pValue, uint nSize, uint dwAttributes);
    [DllImport("kernel32.dll")]
    static extern IntPtr GetCurrentProcess();
    [DllImport("kernel32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    static extern bool CloseHandle(IntPtr h);
    [DllImport("advapi32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    static extern bool OpenProcessToken(IntPtr process, uint access, out IntPtr token);
    [DllImport("advapi32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    [return: MarshalAs(UnmanagedType.Bool)]
    static extern bool LookupPrivilegeValueW(string system, string name, out long luid);
    [StructLayout(LayoutKind.Sequential, Pack = 4)]
    struct TokenPrivileges { public uint Count; public long Luid; public uint Attributes; }
    [DllImport("advapi32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    static extern bool AdjustTokenPrivileges(IntPtr token, bool disableAll, ref TokenPrivileges state, uint len, IntPtr prev, IntPtr retLen);
    [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    static extern SafeFileHandle CreateFileW(string name, uint access, uint share, IntPtr sa, uint disposition, uint flags, IntPtr template);

    // 0 = 启用了；1300 = ERROR_NOT_ALL_ASSIGNED（令牌里没有这个特权）；其余 = Win32 错误码
    public static int EnablePrivilege(string name) {
        IntPtr tok;
        if (!OpenProcessToken(GetCurrentProcess(), 0x28 /* TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY */, out tok)) return Marshal.GetLastWin32Error();
        try {
            long luid;
            if (!LookupPrivilegeValueW(null, name, out luid)) return Marshal.GetLastWin32Error();
            TokenPrivileges tp = new TokenPrivileges();
            tp.Count = 1; tp.Luid = luid; tp.Attributes = 2 /* SE_PRIVILEGE_ENABLED */;
            if (!AdjustTokenPrivileges(tok, false, ref tp, 0, IntPtr.Zero, IntPtr.Zero)) return Marshal.GetLastWin32Error();
            return Marshal.GetLastWin32Error();
        } finally { CloseHandle(tok); }
    }

    // 0 = 读到了；203 = ERROR_ENVVAR_NOT_FOUND；其余 = Win32 错误码
    public static int GetVar(string name, string guid, out byte[] data, out uint attr) {
        byte[] buf = new byte[4096];
        uint a;
        uint n = GetFirmwareEnvironmentVariableExW(name, guid, buf, (uint)buf.Length, out a);
        if (n == 0) { data = new byte[0]; attr = 0; return Marshal.GetLastWin32Error(); }
        data = new byte[n];
        Array.Copy(buf, data, (int)n);
        attr = a;
        return 0;
    }

    // data 为 null / 空 = 删除（nSize 0）
    public static int SetVar(string name, string guid, byte[] data, uint attr) {
        uint n = data == null ? 0u : (uint)data.Length;
        if (SetFirmwareEnvironmentVariableExW(name, guid, data, n, attr)) return 0;
        return Marshal.GetLastWin32Error();
    }

    public static byte[] ReadDisk(int disk, long offset, int length) {
        const int A = 4096;
        long start = offset / A * A;
        int pre = (int)(offset - start);
        int total = (pre + length + A - 1) / A * A;
        using (SafeFileHandle h = CreateFileW("\\\\.\\PhysicalDrive" + disk, 0x80000000 /* GENERIC_READ */, 3 /* SHARE_READ|WRITE */,
                                              IntPtr.Zero, 3 /* OPEN_EXISTING */, 0, IntPtr.Zero)) {
            if (h.IsInvalid) throw new IOException("CreateFile PhysicalDrive" + disk + ": Win32 " + Marshal.GetLastWin32Error());
            using (FileStream fs = new FileStream(h, FileAccess.Read, A)) {
                fs.Seek(start, SeekOrigin.Begin);
                byte[] buf = new byte[total];
                int got = 0;
                while (got < total) { int r = fs.Read(buf, got, total - got); if (r <= 0) break; got += r; }
                if (got < pre + length) throw new IOException("short read on PhysicalDrive" + disk);
                byte[] outb = new byte[length];
                Array.Copy(buf, pre, outb, 0, length);
                return outb;
            }
        }
    }
}
'@

# ── 纯逻辑（test-setup.ps1 在容器里测这些）──────────────────────────────────

function T([string]$Zh, [string]$En) { if ($script:UseZh) { return $Zh } return $En }

function Get-BcdGuid([string]$Text) {
    # bcdedit /copy 的回显是本地化的（中文 Windows："已将该项成功复制到 {…}。"），只认 GUID
    $m = [regex]::Match($Text, '\{[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}\}')
    if ($m.Success) { return $m.Value }
    return $null
}

function Get-ShrinkTarget([long]$CurrentBytes, [long]$MinBytes, [long]$WantBytes, [long]$KeepBytes) {
    # 压缩之后的大小（字节，按 1 MiB 向下对齐）；缩不出这么多返回 $null
    $target = [long]([math]::Floor(($CurrentBytes - $WantBytes) / 1MB)) * 1MB
    if ($target -lt ($MinBytes + $KeepBytes)) { return $null }
    return $target
}

function Get-LiveMiB([long]$ContentBytes) {
    # GK3LIVE 只要装得下自己：内容 ×1.25（FAT32 的簇、以后写回的日志与分区表备份）+ 128 MiB，按 256 MiB 向上取整，至少 512 MiB
    $mib = [math]::Ceiling(($ContentBytes * 1.25 / 1MB + 128) / 256) * 256
    if ($mib -lt 512) { $mib = 512 }
    return [int]$mib
}

function Test-VolumeEncrypted($BitLockerVolume) {
    # 只要不是"完全解密"，卷上就是 BitLocker 的格式 —— 设备加密"等待激活"时也是（保护是关的，数据已经加密）。
    # 那样的卷 Linux 那边 blkid 认作 BitLocker，安装器缩不了（installer-lib.sh 的 why=bitlocker）
    if (-not $BitLockerVolume) { return $false }
    return ([string]$BitLockerVolume.VolumeStatus -ne 'FullyDecrypted')
}

function ConvertFrom-AndroidGiBAnswer([string]$Answer, [int]$Default) {
    # 问"现在缩多少 GiB 给 Android"的回答：空 = 默认；0 = 现在不缩；24–2048；别的返回 $null（再问一次）
    $a = $Answer.Trim()
    if ($a -eq '') { return $Default }
    $n = 0
    if (-not [int]::TryParse($a, [ref]$n)) { return $null }
    if ($n -eq 0 -or ($n -ge 24 -and $n -le 2048)) { return $n }
    return $null
}

function ConvertTo-HexString([byte[]]$Bytes) {
    return (($Bytes | ForEach-Object { $_.ToString('x2') }) -join '')
}

function ConvertFrom-HexString([string]$Hex) {
    $out = New-Object byte[] ($Hex.Length / 2)
    for ($i = 0; $i -lt $out.Length; $i++) { $out[$i] = [Convert]::ToByte($Hex.Substring($i * 2, 2), 16) }
    return ,$out
}

function Get-WpaPsk([string]$Passphrase, [byte[]]$Ssid) {
    # IEEE 802.11i：PSK = PBKDF2-HMAC-SHA1(passphrase, ssid, 4096, 32)。
    # 自己写而不用 Rfc2898DeriveBytes：它（.NET Framework）要求盐至少 8 字节，而 SSID 常常更短。
    # ★ 写进配置的是这个 64 位十六进制的 PSK，不是明文密码。
    $hmac = [System.Security.Cryptography.HMACSHA1]::new([Text.Encoding]::UTF8.GetBytes($Passphrase))
    $out = New-Object byte[] 40
    for ($block = 1; $block -le 2; $block++) {
        $salt = [byte[]]($Ssid + [byte[]](0, 0, 0, $block))
        $u = $hmac.ComputeHash($salt)
        $t = [byte[]]$u.Clone()
        for ($i = 1; $i -lt 4096; $i++) {
            $u = $hmac.ComputeHash($u)
            for ($j = 0; $j -lt 20; $j++) { $t[$j] = $t[$j] -bxor $u[$j] }
        }
        [Array]::Copy($t, 0, $out, ($block - 1) * 20, 20)
    }
    return (ConvertTo-HexString $out).Substring(0, 64)
}

function ConvertTo-WpaNetwork([xml]$WlanProfile) {
    # netsh wlan export profile key=clear 导出的一份 WLAN 配置 → wpa_supplicant 的一个 network 块。
    # 不支持的（企业网 802.1X、WEP）返回 $null。SSID 一律写成十六进制，免得中文/特殊字符出错。
    # （参数不叫 $Profile：它不分大小写地撞上自动变量 $PROFILE）
    $ns = New-Object System.Xml.XmlNamespaceManager($WlanProfile.NameTable)
    $ns.AddNamespace('w', 'http://www.microsoft.com/networking/WLAN/profile/v1')
    $hexNode = $WlanProfile.SelectSingleNode('//w:SSIDConfig/w:SSID/w:hex', $ns)
    $nameNode = $WlanProfile.SelectSingleNode('//w:SSIDConfig/w:SSID/w:name', $ns)
    if ($hexNode) { $ssid = ConvertFrom-HexString $hexNode.InnerText.Trim() }
    elseif ($nameNode) { $ssid = [Text.Encoding]::UTF8.GetBytes($nameNode.InnerText) }
    else { return $null }
    $ssidHex = ConvertTo-HexString $ssid
    $authNode = $WlanProfile.SelectSingleNode('//w:MSM/w:security/w:authEncryption/w:authentication', $ns)
    $encNode = $WlanProfile.SelectSingleNode('//w:MSM/w:security/w:authEncryption/w:encryption', $ns)
    $oneX = $WlanProfile.SelectSingleNode('//w:MSM/w:security/w:authEncryption/w:useOneX', $ns)
    $keyNode = $WlanProfile.SelectSingleNode('//w:MSM/w:security/w:sharedKey/w:keyMaterial', $ns)
    if (-not $authNode) { return $null }
    if ($oneX -and $oneX.InnerText -eq 'true') { return $null }
    # ⚠️ WEP 的 authentication 也写作 open（或 shared）—— 不先看 encryption，就会当成开放网络写出错的配置
    if ($encNode -and $encNode.InnerText -eq 'WEP') { return $null }
    $key = $null; if ($keyNode) { $key = $keyNode.InnerText }
    $lines = @('network={', "    ssid=$ssidHex")
    switch ($authNode.InnerText) {
        'open' { $lines += '    key_mgmt=NONE' }
        { $_ -eq 'WPA2PSK' -or $_ -eq 'WPAPSK' } {
            if (-not $key) { return $null }
            if ($key -match '^[0-9a-fA-F]{64}$') { $lines += "    psk=$($key.ToLower())" }
            elseif ($key.Length -ge 8 -and $key.Length -le 63) { $lines += "    psk=$(Get-WpaPsk $key $ssid)" }
            else { return $null }
            $lines += '    key_mgmt=WPA-PSK'
        }
        'WPA3SAE' {
            # SAE 用不了预先推导的 PSK，只能写明文；带双引号的密码写不进 wpa_supplicant 的引号串
            if (-not $key -or $key.Contains('"')) { return $null }
            $lines += "    sae_password=`"$key`""
            $lines += '    key_mgmt=SAE'
            $lines += '    ieee80211w=2'
        }
        default { return $null }
    }
    $lines += '}'
    return ($lines -join "`n")
}

function Format-LoaderConf {
    # 只在 ESP 上【还没有】loader.conf 时写（Android 装上之后 gk3_apply 会改写它，原件留 .before-gaokun3）
    return (@('timeout 5', 'default gaokun3-live.conf', 'editor no', 'console-mode keep') -join "`n") + "`n"
}

# ── 纯逻辑：伴随工具（§4.9.15）────────────────────────────────────────────────

function ConvertTo-LoaderVarBytes([string]$Value) {
    # systemd-boot 的字符串变量：UTF-16LE + 双字节 NUL（与 gk3boot.c 的 loader_var_set、scripts/boot-oneshot.sh 同一格式；
    # efivarfs 那边前面多 4 字节属性，Windows 的 API 不带，属性单独传）
    return ,([Text.Encoding]::Unicode.GetBytes($Value + [char]0))
}

function ConvertFrom-LoaderVarBytes([byte[]]$Bytes) {
    # 与 gk3boot 的 gk3_ucs2_to_ascii（efi/lib/gk3efi.c:376-384）一致：到第一个 NUL 为止，0x20–0x7e 以外记成 '?'
    $sb = New-Object Text.StringBuilder
    for ($i = 0; $i + 1 -lt $Bytes.Length; $i += 2) {
        $c = [int]$Bytes[$i] -bor ([int]$Bytes[$i + 1] -shl 8)
        if ($c -eq 0) { break }
        if ($c -ge 0x20 -and $c -lt 0x7f) { [void]$sb.Append([char]$c) } else { [void]$sb.Append('?') }
    }
    return $sb.ToString()
}

function Get-DefVarClass($Value) {
    # LoaderEntryDefault 的分类，与 libgk3core 的 gk3_defvar_classify（tools/gk3boot/core/src/dual.c:27-35）逐条相同：
    #   不存在 → absent；auto-windows / gk3-windows.conf（只按 ASCII 不分大小写）→ windows；其余（含空串）→ other（= 不合法，要删）
    # 合法值只有这两种（§4.9.3"两条禁令"）：精确的 Android 条目 id 会让计数用完的入口照样被选中，回落链就断了
    if ($null -eq $Value) { return 'absent' }
    $v = [string]$Value
    # 非 ASCII 先排除：gk3boot 读进来时已经变成 '?'，这里不让 .NET 的大小写折叠把 "auto-wındows" 认成合法
    if ($v -cmatch '[^\x20-\x7e]') { return 'other' }
    $v = $v.ToLowerInvariant()
    if ($v -ceq 'auto-windows' -or $v -ceq 'gk3-windows.conf') { return 'windows' }
    return 'other'
}

function Get-LoaderConfDefault([string]$Text) {
    # loader.conf 的 default：后出现的覆盖先出现的（systemd v257 boot.c:1242-1248）；没有返回 $null
    $d = $null
    foreach ($line in ($Text -split "`r?`n")) {
        $l = $line.Trim()
        if ($l -match '^default\s+(\S+)$') { $d = $Matches[1] }
    }
    return $d
}

function Test-AndroidEntryPattern([string]$Value) {
    # "重启到 Android"写进 OneShot 的值：loader.conf 的 default（HAL 维护成 *-android-<槽>.conf，EspSlot.cpp:157-192）。
    # 只接受指向某个槽的 Android 条目的写法，别的（live、救援、Windows、@saved）一律不写
    return ($Value -match '^[0-9A-Za-z*?._+-]*-android-[ab]\.conf$')
}

function Test-OurEntryName([string]$Name) {
    # loader\entries 里哪些是我们写的：gaokun3-live*、gk3*（统一启动入口 gk3boot-* / gk3prev-* / gk3boot-tools、
    # 双系统的 gk3-windows.conf）、<machine-id>-android|rescue|recovery[-槽]；带不带启动计数后缀 +N[-M]、
    # 带不带 .disabled（安装器停用别的目录的条目时加的）都算
    $n = $Name.ToLowerInvariant() -replace '\.disabled$', ''
    if ($n -notmatch '\.conf$') { return $false }
    $base = $n -replace '(\+\d+(-\d+)?)?\.conf$', ''
    return ($base -match '^gaokun3-live(-\d+)?$' -or $base -match '^gk3' -or $base -match '^[0-9a-f]{32}-(android|rescue|recovery)(-[ab])?$')
}

function Test-OurVarValue([string]$Value) {
    # 共用 ESP 上还有别的 Linux 时，卸载只删"看得出是我们写的"Loader 变量
    if ($null -eq $Value) { return $false }
    if ((Get-DefVarClass $Value) -eq 'windows') { return $true }
    return (Test-AndroidEntryPattern $Value) -or (Test-OurEntryName $Value)
}

function Get-ShutdownKind([string]$Type) {
    # 系统日志 Event 1074（User32）的"关机类型"（第 5 个参数）是本地化的字串 —— 只认得出中英文；
    # 认不出 = unknown（调用方按"关机"处理：删掉 Windows 侧的预置，下次冷开机回默认系统 = 今天的行为，安全的一侧）。
    # ⚠️ 这个判据就是设计稿 D4 ⑨ 要找的东西，【未验证】
    if (-not $Type) { return 'unknown' }
    $t = $Type.ToLowerInvariant()
    if ($t -match 'restart|reboot|重新启动|重启') { return 'restart' }
    if ($t -match 'power ?off|shut ?down|关机') { return 'poweroff' }
    return 'unknown'
}

function Get-ContentKind([byte[]]$Head) {
    # 分区开头 8 KiB 认内容。签名：NTFS / BitLocker 在偏移 3（"NTFS    " / "-FVE-FS-"，scripts/live/test-shrink.sh:129）、
    # FAT32 在 82、FAT12/16 在 54、exFAT 在 3；Android boot.img 在 0（"ANDROID!"，gk3core.h:521）；
    # ext4 超级块在 1024、s_magic 在其中 0x38（0xEF53，小端 53 EF）；LP 元数据几何区在 4096（0x616c4467，fastbootd/lp.c:83）
    $a = [Text.Encoding]::ASCII
    if ($Head.Length -ge 11) {
        $s3 = $a.GetString($Head, 3, 8)
        if ($s3 -ceq 'NTFS    ') { return 'ntfs' }
        if ($s3 -ceq '-FVE-FS-') { return 'bitlocker' }
        if ($s3 -ceq 'EXFAT   ') { return 'exfat' }
    }
    if ($Head.Length -ge 8 -and $a.GetString($Head, 0, 8) -ceq 'ANDROID!') { return 'android-boot' }
    if ($Head.Length -ge 90 -and $a.GetString($Head, 82, 8) -ceq 'FAT32   ') { return 'fat' }
    if ($Head.Length -ge 62 -and ($a.GetString($Head, 54, 8) -ceq 'FAT16   ' -or $a.GetString($Head, 54, 8) -ceq 'FAT12   ')) { return 'fat' }
    if ($Head.Length -ge 1082 -and $Head[1080] -eq 0x53 -and $Head[1081] -eq 0xEF) { return 'ext4' }
    if ($Head.Length -ge 4100 -and [BitConverter]::ToUInt32($Head, 4096) -eq 0x616c4467) { return 'lp' }
    foreach ($b in $Head) { if ($b -ne 0) { return 'unknown' } }
    return 'empty'
}

function ConvertFrom-GptBytes([byte[]]$Header, [byte[]]$Entries) {
    # GPT 头（LBA 1）+ 分区项数组 → 分区列表。UEFI 规范：头签名 "EFI PART"，分区项起始 LBA @72、项数 @80、项大小 @84；
    # 每项：类型 GUID @0、分区 GUID @16、首 LBA @32、末 LBA @40、属性 @48、名字（UTF-16LE，36 字符）@56
    if ($Header.Length -lt 92 -or [Text.Encoding]::ASCII.GetString($Header, 0, 8) -cne 'EFI PART') { throw 'not a GPT header (signature)' }
    $count = [BitConverter]::ToUInt32($Header, 80)
    $size = [BitConverter]::ToUInt32($Header, 84)
    if ($size -lt 128) { throw "GPT entry size $size" }
    $out = New-Object System.Collections.ArrayList
    for ($i = 0; $i -lt $count; $i++) {
        $o = [long]$i * $size
        if ($o + 128 -gt $Entries.Length) { break }
        [byte[]]$tb = $Entries[$o..($o + 15)]
        $type = New-Object Guid (, $tb)
        if ($type -eq [Guid]::Empty) { continue }
        [byte[]]$ub = $Entries[($o + 16)..($o + 31)]
        $name = [Text.Encoding]::Unicode.GetString($Entries, [int]($o + 56), 72)
        $z = $name.IndexOf([char]0); if ($z -ge 0) { $name = $name.Substring(0, $z) }
        [void]$out.Add([pscustomobject]@{
            Index = $i + 1; Type = $type.ToString(); Guid = (New-Object Guid (, $ub)).ToString()
            FirstLba = [BitConverter]::ToUInt64($Entries, [int]($o + 32)); LastLba = [BitConverter]::ToUInt64($Entries, [int]($o + 40))
            Attributes = [BitConverter]::ToUInt64($Entries, [int]($o + 48)); Name = $name
        })
    }
    return $out.ToArray()
}

function ConvertTo-NormGuid($Guid) { return ([string]$Guid).Trim().Trim('{', '}').ToLowerInvariant() }

function Test-RemovalCandidates($Items) {
    # -RemoveAndroid 删之前的断言（§4.9.13 第 0 步）。$Items：系统盘上 GPT 名字在 Android 名单里的每个分区，
    #   @{ Name; Type; Guid; Offset; Size; Kind（Get-ContentKind）; OffsetOk（GPT 与 Windows 看到的偏移一致） }
    # 返回 @{ Errors; Notes } —— 有一条 Error 就整个不做（什么都还没改）
    $errors = New-Object System.Collections.ArrayList
    $notes = New-Object System.Collections.ArrayList
    $Items = @($Items)
    if ($Items.Count -eq 0) { [void]$errors.Add((T '系统盘上没找到 Android 的分区（misc / boot_a / super / userdata …）' 'no Android partitions found on the system disk')) }
    $want = @{ boot_a = 'android-boot'; boot_b = 'android-boot'; super = 'lp'; metadata = 'ext4'; userdata = 'ext4'; gk3rescue = 'ext4' }
    $confirmed = 0
    foreach ($g in ($Items | Group-Object -Property Name)) {
        if ($g.Count -gt 1) { [void]$errors.Add((T "分区名 $($g.Name) 出现了 $($g.Count) 次 —— 认不准是哪一个，不删" "partition name $($g.Name) appears $($g.Count) times - ambiguous, not removing")) }
    }
    foreach ($it in $Items) {
        if ((ConvertTo-NormGuid $it.Type) -ne $LinuxFsType) { [void]$errors.Add((T "$($it.Name) 的类型是 $($it.Type)，不是 Linux 文件系统（$LinuxFsType）—— 不像安装器建的" "$($it.Name) has type $($it.Type), not Linux filesystem - not created by the installer")) }
        if (-not $it.OffsetOk) { [void]$errors.Add((T "$($it.Name)：分区表里的位置与 Windows 看到的不一致" "$($it.Name): the GPT offset does not match what Windows reports")) }
        if (@('ntfs', 'bitlocker', 'fat', 'exfat') -contains $it.Kind) { [void]$errors.Add((T "$($it.Name) 里是 $($it.Kind) 文件系统 —— 那不是 Android 的分区，不删" "$($it.Name) contains $($it.Kind) - not an Android partition, not removing")) }
        if ($it.Name -eq 'misc' -and [long]$it.Size -gt [long]$MiscMaxMiB * 1MB) { [void]$errors.Add((T "misc 有 $([math]::Round($it.Size / 1MB, 1)) MiB，超过安装器建的 $MiscMaxMiB MiB" "misc is larger than the installer's $MiscMaxMiB MiB")) }
        if ($want.ContainsKey($it.Name)) {
            if ($it.Kind -eq $want[$it.Name]) { if (@('boot_a', 'boot_b', 'super') -contains $it.Name) { $confirmed++ } }
            elseif (@('ntfs', 'bitlocker', 'fat', 'exfat') -notcontains $it.Kind) { [void]$notes.Add((T "$($it.Name) 的内容没认出（$($it.Kind)），按名字与类型认" "$($it.Name): content not recognised ($($it.Kind)); matched by name and type")) }
        }
    }
    if ($Items.Count -gt 0 -and $confirmed -eq 0) { [void]$errors.Add((T 'boot_a / boot_b / super 里没有一个认得出是 Android（ANDROID! / LP 元数据）—— 不敢删' 'none of boot_a / boot_b / super looks like Android - refusing')) }
    return @{ Errors = $errors.ToArray(); Notes = $notes.ToArray() }
}

function Get-ExtendPlan($Partitions, [string]$TargetGuid, [string[]]$DeleteGuids) {
    # 删完 Android 之后能不能把 D: 扩回去：紧跟在 D: 后面的分区必须是要删的（脚本路径的布局：D: | GK3LIVE | Android…，
    # gaokun3-setup.ps1 的 New-LiveVolume）。不相邻就不扩、说明原因（§4.9.13 第 4 步；出厂布局 D: 后面紧接 WINPE，不去动它）
    $del = @($DeleteGuids | ForEach-Object { ConvertTo-NormGuid $_ })
    $sorted = @($Partitions | Sort-Object { [long]$_.Offset })
    $idx = -1
    for ($i = 0; $i -lt $sorted.Count; $i++) { if ((ConvertTo-NormGuid $sorted[$i].Guid) -eq (ConvertTo-NormGuid $TargetGuid)) { $idx = $i } }
    if ($idx -lt 0) { return @{ Adjacent = $false; Reason = 'target-missing'; Run = @(); Stranded = $del } }
    if ($idx -eq $sorted.Count - 1) { return @{ Adjacent = $false; Reason = 'nothing-after'; Run = @(); Stranded = $del } }
    if ($del -notcontains (ConvertTo-NormGuid $sorted[$idx + 1].Guid)) { return @{ Adjacent = $false; Reason = 'next-kept'; Next = $sorted[$idx + 1]; Run = @(); Stranded = $del } }
    $run = New-Object System.Collections.ArrayList
    for ($i = $idx + 1; $i -lt $sorted.Count; $i++) {
        $g = ConvertTo-NormGuid $sorted[$i].Guid
        if ($del -notcontains $g) { break }
        [void]$run.Add($g)
    }
    $stranded = @($del | Where-Object { $run -notcontains $_ })
    return @{ Adjacent = $true; Reason = ''; Run = $run.ToArray(); Stranded = $stranded }
}

function New-TaskXml {
    # 计划任务的 XML（Task Scheduler 1.2 架构，schtasks /Create /XML 用）。动作都是固定的命令行 —— 开始菜单项只能"触发"
    # 这几个任务，【传不进任何参数】，免得它成了提权通道（§4.9.15）。
    # ⚠️ 未验证：RegistrationInfo/SecurityDescriptor 让普通用户能 schtasks /Run（GRGX 给 Authenticated Users）在真 Windows 上
    #    是否生效；登录触发 + Users 组的任务能不能弹到用户桌面
    param([string]$Description, [ValidateSet('boot', 'logon', 'event1074', 'none')][string]$Trigger,
          [ValidateSet('system', 'users')][string]$RunAs, [string]$Command, [string]$Arguments, [switch]$UsersMayRun)
    $x = { param($s) [Security.SecurityElement]::Escape([string]$s) }
    $sd = ''
    if ($UsersMayRun) { $sd = '    <SecurityDescriptor>D:(A;;FA;;;SY)(A;;FA;;;BA)(A;;GRGX;;;AU)</SecurityDescriptor>' }
    $trig = ''
    switch ($Trigger) {
        'boot' { $trig = '<Triggers><BootTrigger><Enabled>true</Enabled><Delay>PT30S</Delay></BootTrigger></Triggers>' }
        'logon' { $trig = '<Triggers><LogonTrigger><Enabled>true</Enabled><Delay>PT20S</Delay></LogonTrigger></Triggers>' }
        'event1074' {
            $q = "<QueryList><Query Id=""0"" Path=""System""><Select Path=""System"">*[System[Provider[@Name='User32'] and EventID=1074]]</Select></Query></QueryList>"
            $trig = '<Triggers><EventTrigger><Enabled>true</Enabled><Subscription>' + (& $x $q) + '</Subscription></EventTrigger></Triggers>'
        }
    }
    if ($RunAs -eq 'system') { $pr = '<UserId>S-1-5-18</UserId><RunLevel>HighestAvailable</RunLevel>' }
    else { $pr = '<GroupId>S-1-5-32-545</GroupId><RunLevel>LeastPrivilege</RunLevel>' }
    $lines = @(
        '<?xml version="1.0" encoding="UTF-16"?>',
        '<Task version="1.2" xmlns="http://schemas.microsoft.com/windows/2004/02/mit/task">',
        '  <RegistrationInfo>',
        "    <Author>gaokun3</Author>",
        "    <Description>$(& $x $Description)</Description>"
    )
    if ($sd) { $lines += $sd }
    $lines += @(
        '  </RegistrationInfo>',
        "  $trig",
        "  <Principals><Principal id=""Author"">$pr</Principal></Principals>",
        '  <Settings>',
        '    <MultipleInstancesPolicy>IgnoreNew</MultipleInstancesPolicy>',
        '    <DisallowStartIfOnBatteries>false</DisallowStartIfOnBatteries>',
        '    <StopIfGoingOnBatteries>false</StopIfGoingOnBatteries>',
        '    <AllowHardTerminate>true</AllowHardTerminate>',
        '    <StartWhenAvailable>true</StartWhenAvailable>',
        '    <AllowStartOnDemand>true</AllowStartOnDemand>',
        '    <Enabled>true</Enabled>',
        '    <Hidden>false</Hidden>',
        '    <ExecutionTimeLimit>PT10M</ExecutionTimeLimit>',
        '  </Settings>',
        '  <Actions Context="Author"><Exec>',
        "    <Command>$(& $x $Command)</Command>",
        "    <Arguments>$(& $x $Arguments)</Arguments>",
        '  </Exec></Actions>',
        '</Task>'
    )
    return ($lines -join "`r`n") + "`r`n"
}

function Get-PendingNotices($Issues, [string[]]$Acked) {
    # 登录时要弹哪些：没确认过的（id|key）；BOOTAA64 被换掉这一条每次登录都弹，直到修好（开机自检默认开、不可关，U23）
    $out = New-Object System.Collections.ArrayList
    foreach ($i in @($Issues)) {
        if ($null -eq $i) { continue }
        $k = "$($i['id'])|$($i['key'])"
        if ($i['id'] -eq 'bootaa64-replaced' -or $Acked -notcontains $k) { [void]$out.Add($i) }
    }
    return $out.ToArray()
}

function Get-NoticeText($Issue) {
    # 返回 @(标题, 正文, 是否给"现在修复")
    $id = $Issue['id']; $d = [string]$Issue['detail']
    switch ($id) {
        'bootaa64-replaced' { return @((T 'gaokun3：Android 进不去了' 'gaokun3: Android cannot start'),
            (T "Windows 把开机用的启动器 \EFI\Boot\bootaa64.efi 换回了它自己的（Windows 启动管理器）。现在开机直接进 Windows，Android 进不去。`n`n现在修复吗？会先暂停 BitLocker 一次重启、再把 systemd-boot 拷回去（要管理员权限）。" "Windows replaced the boot loader \EFI\Boot\bootaa64.efi with its own Boot Manager, so the machine now boots straight into Windows and Android cannot start.`n`nRepair now? BitLocker is suspended for one restart first, then systemd-boot is copied back (needs administrator rights)."), $true) }
        'bootaa64-other' { return @((T 'gaokun3：启动器不认识' 'gaokun3: unknown boot loader'),
            (T "\EFI\Boot\bootaa64.efi 既不是 systemd-boot 也不是 Windows 启动管理器（也许是另一个 Linux 装的）。伴随工具不自动改它。开始菜单 gaokun3 → 检查启动状态 看详情。" "\EFI\Boot\bootaa64.efi is neither systemd-boot nor the Windows Boot Manager (another Linux?). The companion does not touch it. Start menu → gaokun3 → Check boot status."), $false) }
        'sdboot-missing' { return @((T 'gaokun3：ESP 上缺 systemd-boot' 'gaokun3: systemd-boot missing'),
            (T 'ESP 上没有 \EFI\systemd\systemd-bootaa64.efi，伴随工具修不了启动。用 U 盘 live 的"修复启动"。' 'The ESP has no \EFI\systemd\systemd-bootaa64.efi, so the companion cannot repair the boot. Use the USB live "repair boot".'), $false) }
        'default-reset' { return @((T 'gaokun3：开机默认项已重置' 'gaokun3: boot default was reset'),
            (T "开机默认项 LoaderEntryDefault 是 `"$d`"，不是 Windows 条目 —— 这样会把开机钉在一个条目上、绕过 Android 的回落（多半是在开机菜单里按了 d）。已清除 = 默认进 Android。想默认进 Windows：开始菜单 gaokun3 → 开机默认进 Windows。" "LoaderEntryDefault was `"$d`", which is not the Windows entry; it pins the boot to one entry and bypasses Android's fallback (probably 'd' pressed in the boot menu). Cleared = Android by default. For Windows by default: Start menu → gaokun3 → Boot Windows by default."), $false) }
        'bios-changed' { return @((T 'gaokun3：BIOS 版本变了' 'gaokun3: BIOS version changed'),
            (T "BIOS 从 $d。BIOS 更新可能把安全启动重新打开（那样两个系统都进不去 —— 进固件设置关掉它）、可能让 BitLocker 要恢复密钥（https://aka.ms/myrecoverykey）。平板上进固件设置的按键组合还没在本机确认（设计稿 E3）。" "BIOS changed: $d. A BIOS update may turn Secure Boot back on (then neither system starts - turn it off in firmware setup) and may make BitLocker ask for the recovery key (https://aka.ms/myrecoverykey). The key combination for firmware setup on the tablet is not confirmed yet (design E3)."), $false) }
        'secureboot-on' { return @((T 'gaokun3：安全启动开着' 'gaokun3: Secure Boot is on'),
            (T '安全启动开着：Android（没有签名）起不来。进固件设置把它关掉。' 'Secure Boot is on: Android (unsigned) cannot start. Turn it off in firmware setup.'), $false) }
        default { return @('gaokun3', (T "开机自检：$id $d" "boot check: $id $d"), $false) }
    }
}

# ── 与系统打交道 ──────────────────────────────────────────────────────────────

function Say([string]$Zh, [string]$En) { Write-Host (T $Zh $En) }
function Step([string]$Zh, [string]$En) { Write-Host ''; Write-Host ('== ' + (T $Zh $En)) -ForegroundColor Cyan }
function Warn([string]$Zh, [string]$En) { Write-Host ('!  ' + (T $Zh $En)) -ForegroundColor Yellow }
# Fail / 用户说"不"都抛出去，由最外层换成退出码（1 / 0）：这样 finally 照样会跑（ESP 不会一直挂在盘符上），
# 单元测试也能接住而不是整个进程退出
function Fail([string]$Zh, [string]$En) { $m = T $Zh $En; Write-Host ('!! ' + $m) -ForegroundColor Red; Write-Log "FAIL $m"; throw "GK3FAIL $m" }
function Quit { throw 'GK3QUIT' }
function Write-Log([string]$Line) {
    # 伴随工具的日志（计划任务以 SYSTEM 跑，没有窗口）：%ProgramData%\gaokun3\companion.log，超过 256 KiB 换一份
    try {
        if (-not (Test-Path -LiteralPath $DataDir)) { New-Item -ItemType Directory -Force -Path $DataDir | Out-Null }
        if ((Test-Path -LiteralPath $LogFile) -and (Get-Item -LiteralPath $LogFile).Length -gt 256KB) { Move-Item -Force -LiteralPath $LogFile -Destination "$LogFile.old" }
        Add-Content -LiteralPath $LogFile -Value ("{0} {1}" -f (Get-Date).ToString('yyyy-MM-dd HH:mm:ss'), $Line) -Encoding UTF8
    } catch { }
}
function Show-Preview {
    Warn '【预览】Windows 伴随工具还没在真 Windows 上跑过（只有容器里的单元测试；Parallels 的 D4 与真机的 D5/D6 待做）。' '[Preview] The Windows companion has not run on real Windows yet (unit tests in a container only; Parallels D4 and hardware D5/D6 pending).'
}
function Read-Answer([string]$Prompt) { return (Read-Host $Prompt) }

function Invoke-Bcd([string[]]$BcdArgs) {
    $out = & bcdedit.exe @BcdArgs 2>&1 | Out-String
    if ($LASTEXITCODE -ne 0) { throw "bcdedit $($BcdArgs -join ' ') -> $LASTEXITCODE`n$out" }
    return $out
}

function Get-FreeDriveLetter {
    $used = @((Get-PSDrive -PSProvider FileSystem).Name)
    foreach ($c in [char[]]'SRQPONMLKJIHGTUVWXYZ') { if ($used -notcontains [string]$c) { return [string]$c } }
    throw 'no free drive letter'
}

function Mount-Esp {
    $l = Get-FreeDriveLetter
    & mountvol.exe "$($l):" /S | Out-Null
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path "$($l):\EFI")) { Fail "挂不上 ESP（mountvol /S 失败）" 'cannot mount the ESP (mountvol /S failed)' }
    return "$($l):"
}
function Dismount-Esp([string]$Esp) { & mountvol.exe $Esp /D | Out-Null }

function Test-AndroidInstalled([string]$Esp) {
    return [bool](Get-ChildItem -Path "$Esp\loader\entries" -Filter '*-android-*.conf' -ErrorAction SilentlyContinue)
}

function Confirm-Yes([string]$Zh, [string]$En) {
    if ($Yes) { return }
    Write-Host ''
    $a = Read-Answer (T "$Zh`n输入 YES 继续，其他任何输入都会退出" "$En`nType YES to continue; anything else quits")
    if ($a -cne 'YES') { Say '已退出，什么都没改。' 'Quit. Nothing was changed.'; Quit }
}

function Confirm-YesNo([string]$Zh, [string]$En) {
    # 不破坏东西的 y/N（比如 U24 关休眠）。-Yes 时一律按"否"：要自动化就显式给参数（-Hibernate off）
    if ($Yes) { return $false }
    $a = Read-Answer (T "$Zh (y/N)" "$En (y/N)")
    return ($a -eq 'y' -or $a -eq 'Y')
}

function Test-Bundle([string]$Dir) {
    $sums = Join-Path $Dir 'SHA256SUMS'
    if (-not (Test-Path $sums)) { Fail "安装包不完整：缺 SHA256SUMS（$Dir）" "incomplete bundle: SHA256SUMS missing ($Dir)" }
    foreach ($line in Get-Content $sums) {
        if ($line -notmatch '^([0-9a-f]{64})\s+\*?(.+)$') { continue }
        $want = $Matches[1]; $rel = $Matches[2]
        $f = Join-Path $Dir ($rel -replace '/', '\')
        if (-not (Test-Path $f)) { Fail "安装包缺文件：$rel" "bundle is missing $rel" }
        $got = (Get-FileHash -Algorithm SHA256 -Path $f).Hash.ToLower()
        if ($got -ne $want) { Fail "安装包里的 $rel 损坏了（sha256 不对）—— 重新下载" "$rel is corrupt (sha256 mismatch) - download again" }
    }
}

function Save-State($State) {
    New-Item -ItemType Directory -Force -Path $DataDir | Out-Null
    $State | ConvertTo-Json | Set-Content -Path $StateFile -Encoding UTF8
}

function New-LiveVolume([int]$Disk, $Shrunk) {
    Step "3/5 新建 $LiveLabel" "3/5 Creating $LiveLabel"
    # GK3LIVE 放在缩出来那段的【开头】，Android 装在它后面 —— 与 fixture 场景 windows-live 的布局一致
    $off = [long]([math]::Ceiling(($Shrunk.Offset + $Shrunk.Size) / 1MB)) * 1MB
    $np = New-Partition -DiskNumber $Disk -Offset $off -Size ([long]$LiveMiB * 1MB) -GptType $BasicType -AssignDriveLetter
    Format-Volume -Partition $np -FileSystem FAT32 -NewFileSystemLabel $LiveLabel -Confirm:$false -Force | Out-Null
    return (Get-Volume -FileSystemLabel $LiveLabel)
}

# ── 与系统打交道：伴随工具（test-setup.ps1 把这些逐个换成桩）─────────────────────

function Test-Admin {
    try { return ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator) }
    catch { return $false }
}
function Assert-Admin { if (-not (Test-Admin)) { Fail '要以管理员身份运行（双击 gaokun3-setup.cmd 会自动请求；开始菜单 gaokun3 里的项也是）' 'run as Administrator (gaokun3-setup.cmd and the gaokun3 Start menu items ask for it)' } }
function Get-SystemDrive { if ($env:SystemDrive) { return $env:SystemDrive } return 'C:' }
function Get-PowerShellExe {
    # 计划任务与快捷方式里写全路径，不靠 PATH
    $root = $env:SystemRoot; if (-not $root) { $root = 'C:\Windows' }
    return "$root\System32\WindowsPowerShell\v1.0\powershell.exe"
}
function ConvertTo-NativePath([string]$Path) {
    # .NET 的 [IO.File] 不像 PowerShell 的 cmdlet 那样在 Linux 上把 \ 当分隔符（只影响容器里的单元测试）
    if ([IO.Path]::DirectorySeparatorChar -eq '\') { return $Path }
    return ($Path -replace '\\', '/')
}

function Invoke-Native([string]$Exe, [string[]]$ArgList) {
    # 外部程序（schtasks / powercfg / shutdown / manage-bde）都走这里：测试里换成桩、记下每次调用
    $out = & $Exe @ArgList 2>&1 | Out-String
    return [pscustomobject]@{ Code = $LASTEXITCODE; Out = $out }
}

function ConvertTo-Hash($o) {
    # Windows PowerShell 5.1 的 ConvertFrom-Json 没有 -AsHashtable：自己转（状态文件都是扁平的小对象）
    if ($null -eq $o) { return $null }
    if ($o -is [System.Management.Automation.PSCustomObject]) {
        $h = [ordered]@{}
        foreach ($p in $o.PSObject.Properties) { $h[$p.Name] = ConvertTo-Hash $p.Value }
        return $h
    }
    if ($o -is [System.Collections.IEnumerable] -and $o -isnot [string]) { return ,@($o | ForEach-Object { ConvertTo-Hash $_ }) }
    return $o
}
function Read-JsonFile([string]$Path) {
    if (-not (Test-Path -LiteralPath $Path)) { return $null }
    try { $o = Get-Content -LiteralPath $Path -Raw -Encoding UTF8 | ConvertFrom-Json } catch { return $null }
    return (ConvertTo-Hash $o)
}
function Write-JsonFile([string]$Path, $Object) {
    $dir = Split-Path -Parent $Path
    if (-not (Test-Path -LiteralPath $dir)) { New-Item -ItemType Directory -Force -Path $dir | Out-Null }
    $Object | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $Path -Encoding UTF8
}
function Read-Companion {
    $c = Read-JsonFile $CompanionFile
    if (-not $c) { $c = [ordered]@{} }
    foreach ($k in 'version', 'biosVersion', 'windowsPreset', 'fastStartupWas', 'hibernateWas') { if (-not $c.Contains($k)) { $c[$k] = $null } }
    if ($null -eq $c['windowsPreset']) { $c['windowsPreset'] = $false }
    return $c
}

function Get-FastStartup { try { return [int](Get-ItemProperty -Path $FastStartupKey -Name HiberbootEnabled -ErrorAction Stop).HiberbootEnabled } catch { return $null } }
function Set-FastStartup([int]$Value) { Set-ItemProperty -Path $FastStartupKey -Name HiberbootEnabled -Value $Value }
function Get-HibernateEnabled { try { return [int](Get-ItemProperty -Path $PowerKey -Name HibernateEnabled -ErrorAction Stop).HibernateEnabled } catch { return $null } }
function Set-Hibernate([bool]$On) {
    $v = 'off'; if ($On) { $v = 'on' }
    $r = Invoke-Native 'powercfg.exe' @('/hibernate', $v)
    if ($r.Code -ne 0) { throw "powercfg /hibernate $v -> $($r.Code) $($r.Out)" }
}
function Get-BiosVersion { try { return [string](Get-CimInstance -ClassName Win32_BIOS -ErrorAction Stop).SMBIOSBIOSVersion } catch { return $null } }
function Get-SecureBootOn { try { return [bool](Confirm-SecureBootUEFI -ErrorAction Stop) } catch { return $null } }

function Get-OsBitLockerState {
    # on / off / unknown（家庭版可能没有 BitLocker 模块；unknown 一律按"开着"小心处理）
    if (-not (Get-Command Get-BitLockerVolume -ErrorAction SilentlyContinue)) { return 'unknown' }
    try { $v = Get-BitLockerVolume -MountPoint (Get-SystemDrive) -ErrorAction Stop } catch { return 'unknown' }
    if ([string]$v.ProtectionStatus -eq 'On') { return 'on' }
    return 'off'
}
function Suspend-OsBitLocker([int]$Count) {
    # Suspend-BitLocker（与免 U 盘安装同一个 cmdlet）；模块不在或失败时退回 manage-bde -protectors -disable <盘> -RebootCount N
    $mp = Get-SystemDrive
    if (Get-Command Suspend-BitLocker -ErrorAction SilentlyContinue) {
        try { Suspend-BitLocker -MountPoint $mp -RebootCount $Count -ErrorAction Stop | Out-Null; return $true }
        catch { Warn "Suspend-BitLocker 失败（$($_.Exception.Message)），改用 manage-bde" "Suspend-BitLocker failed ($($_.Exception.Message)); trying manage-bde" }
    }
    $r = Invoke-Native 'manage-bde.exe' @('-protectors', '-disable', $mp, '-RebootCount', [string]$Count)
    return ($r.Code -eq 0)
}

function Initialize-Native { if (-not ('Gk3Native' -as [type])) { Add-Type -TypeDefinition $NativeSource -Language CSharp } }
function Enable-FwPrivilege {
    if ($script:FwPrivOk) { return }
    Initialize-Native
    $e = [Gk3Native]::EnablePrivilege('SeSystemEnvironmentPrivilege')
    if ($e -ne 0) { throw "AdjustTokenPrivileges(SeSystemEnvironmentPrivilege) -> Win32 $e" }
    $script:FwPrivOk = $true
}
function Get-FwVarRaw([string]$Name) {
    Enable-FwPrivilege
    $data = $null; [uint32]$attr = 0
    $code = [Gk3Native]::GetVar($Name, $LoaderGuid, [ref]$data, [ref]$attr)
    return @{ Code = $code; Data = $data; Attr = $attr }
}
function Set-FwVarRaw([string]$Name, [byte[]]$Data) {
    Enable-FwPrivilege
    return [Gk3Native]::SetVar($Name, $LoaderGuid, $Data, [uint32]$LoaderVarAttr)
}

function Read-LoaderVar([string]$Name) {
    $r = Get-FwVarRaw $Name
    if ($r.Code -eq 0) { return @{ State = 'present'; Value = (ConvertFrom-LoaderVarBytes ([byte[]]$r.Data)); Attr = $r.Attr } }
    if ($r.Code -eq 203) { return @{ State = 'absent'; Value = $null } }                 # ERROR_ENVVAR_NOT_FOUND
    if ($r.Code -eq 122) { return @{ State = 'present'; Value = '(too long)' } }         # ERROR_INSUFFICIENT_BUFFER：比任何条目 id 都长
    return @{ State = 'error'; Value = $null; Code = $r.Code }
}
function Write-LoaderVar([string]$Name, [string]$Value) {
    # 写、再读回逐字节核对（属性也核：gk3boot 的 loader_var_set 同样要求 0x07）。成功返回 $null，否则返回原因
    $b = ConvertTo-LoaderVarBytes $Value
    $c = Set-FwVarRaw $Name $b
    if ($c -ne 0) { return "SetFirmwareEnvironmentVariableEx($Name) -> Win32 $c" }
    $r = Get-FwVarRaw $Name
    if ($r.Code -ne 0) { return "read back $Name -> Win32 $($r.Code)" }
    if ([Convert]::ToBase64String([byte[]]$r.Data) -ne [Convert]::ToBase64String($b)) { return "read back ${Name}: content differs" }
    if ([int]$r.Attr -ne $LoaderVarAttr) { return ("read back ${Name}: attributes 0x{0:x} (want 0x7)" -f [int]$r.Attr) }
    return $null
}
function Remove-LoaderVar([string]$Name) {
    # 大小 0 = 删除（微软文档："setting this value to zero will result in the deletion of this variable"）；本来就没有也算成功
    $c = Set-FwVarRaw $Name $null
    if ($c -ne 0 -and $c -ne 203) { return "delete $Name -> Win32 $c" }
    $r = Get-FwVarRaw $Name
    if ($r.Code -eq 203) { return $null }
    if ($r.Code -eq 0) { return "delete ${Name}: still there" }
    return "read back $Name -> Win32 $($r.Code)"
}

function Read-DiskBytes([int]$Disk, [long]$Offset, [int]$Length) {
    Initialize-Native
    return ,([Gk3Native]::ReadDisk($Disk, $Offset, $Length))
}
function Get-DiskGpt([int]$Disk, [int]$SectorSize) {
    $hdr = Read-DiskBytes $Disk ([long]$SectorSize) $SectorSize
    if ([Text.Encoding]::ASCII.GetString($hdr, 0, 8) -cne 'EFI PART') { throw "disk ${Disk}: no GPT header at LBA 1" }
    $lba = [BitConverter]::ToUInt64($hdr, 72); $n = [BitConverter]::ToUInt32($hdr, 80); $sz = [BitConverter]::ToUInt32($hdr, 84)
    $len = [int]([math]::Ceiling([double]$n * $sz / $SectorSize) * $SectorSize)
    $ent = Read-DiskBytes $Disk ([long]$lba * $SectorSize) $len
    return (ConvertFrom-GptBytes $hdr $ent)
}

function New-Shortcut([string]$Path, [string]$Target, [string]$Arguments, [string]$Description) {
    $sh = New-Object -ComObject WScript.Shell
    $s = $sh.CreateShortcut($Path)
    $s.TargetPath = $Target; $s.Arguments = $Arguments; $s.Description = $Description; $s.WorkingDirectory = $InstallDir
    $s.Save()
}
function Register-Gk3Task([string]$Name, [string]$Xml) {
    New-Item -ItemType Directory -Force -Path $DataDir | Out-Null
    $f = Join-Path $DataDir "task-$Name.xml"
    # schtasks /XML 认 UTF-16（Task Scheduler 自己导出的就是 UTF-16 LE 带 BOM）
    [IO.File]::WriteAllText((ConvertTo-NativePath $f), $Xml, [Text.Encoding]::Unicode)
    try { $r = Invoke-Native 'schtasks.exe' @('/Create', '/TN', "$TaskFolder$Name", '/XML', $f, '/F') }
    finally { Remove-Item -Force -ErrorAction SilentlyContinue -LiteralPath $f }
    if ($r.Code -ne 0) { throw "schtasks /Create $TaskFolder$Name -> $($r.Code): $($r.Out)" }
}
function Unregister-Gk3Task([string]$Name) { $null = Invoke-Native 'schtasks.exe' @('/Delete', '/TN', "$TaskFolder$Name", '/F') }

function Show-Box([string]$Title, [string]$Text, [bool]$AskRepair) {
    Add-Type -AssemblyName System.Windows.Forms
    if ($AskRepair) {
        return ([System.Windows.Forms.MessageBox]::Show($Text, $Title, [System.Windows.Forms.MessageBoxButtons]::YesNo, [System.Windows.Forms.MessageBoxIcon]::Warning) -eq [System.Windows.Forms.DialogResult]::Yes)
    }
    [void][System.Windows.Forms.MessageBox]::Show($Text, $Title, [System.Windows.Forms.MessageBoxButtons]::OK, [System.Windows.Forms.MessageBoxIcon]::Information)
    return $false
}
function Get-LastShutdownType {
    # 最近一条 Event 1074（User32）的第 5 个参数 = 关机类型（本地化字串）；10 分钟之前的不算
    try {
        $e = Get-WinEvent -FilterHashtable @{ LogName = 'System'; ProviderName = 'User32'; Id = 1074 } -MaxEvents 1 -ErrorAction Stop
        if ($e.TimeCreated -lt (Get-Date).AddMinutes(-10)) { return $null }
        return [string]$e.Properties[4].Value
    } catch { return $null }
}

function Test-HasDriveLetter($Partition) { return ([string]$Partition.DriveLetter -match '^[A-Za-z]$') }
function Get-SystemDiskNumber { return (Get-Partition -DriveLetter (Get-SystemDrive).Substring(0, 1)).DiskNumber }
function Get-LivePartitions {
    # 系统盘上卷标 GK3LIVE 的分区。⚠️ U 盘介质的卷标也是 GK3LIVE（build-usb.sh 的 mformat -v），按盘号排除
    $sysDisk = Get-SystemDiskNumber
    $out = @()
    foreach ($v in @(Get-Volume -FileSystemLabel $LiveLabel -ErrorAction SilentlyContinue)) {
        if (-not $v) { continue }
        $p = $v | Get-Partition
        if ($p -and $p.DiskNumber -eq $sysDisk) { $out += $p }
    }
    return $out
}
function Test-LivePartitionContent($Partition) {
    # GK3LIVE 按卷标 + 内容认（gaokun3\live.squashfs，§4.9.13）；没盘符就临时给一个、看完收回
    $had = Test-HasDriveLetter $Partition
    $p = $Partition
    if (-not $had) {
        $p | Add-PartitionAccessPath -AssignDriveLetter
        $p = Get-Partition -DiskNumber $Partition.DiskNumber -PartitionNumber $Partition.PartitionNumber
    }
    try { return (Test-Path -LiteralPath "$($p.DriveLetter):\gaokun3\live.squashfs") }
    finally { if (-not $had -and (Test-HasDriveLetter $p)) { $p | Remove-PartitionAccessPath -AccessPath "$($p.DriveLetter):\" } }
}

# ── 伴随工具：ESP 体检 ─────────────────────────────────────────────────────────

function Get-EspReport([string]$Esp) {
    # 只读看 ESP（文件系统层面，不碰变量）。State：
    #   no-android          没有 *-android-*.conf（还没装 / 已卸载）
    #   missing-sdboot      没有 EFI\systemd\systemd-bootaa64.efi（修不了，用 U 盘）
    #   missing-bootaa64    回落路径上什么都没有（可以修：拷 systemd-boot 过去不覆盖任何人的文件）
    #   ok                  bootaa64.efi 与 systemd-bootaa64.efi 同字节（sha256）
    #   replaced-by-windows bootaa64.efi 与 EFI\Microsoft\Boot\bootmgfw.efi 同字节（Windows 换回去了，§4.9.6）
    #   other               都不是（另一个 Linux？）—— 只报告、不动
    $hash = { param($rel) $f = Join-Path $Esp $rel; if (Test-Path -LiteralPath $f -PathType Leaf) { (Get-FileHash -Algorithm SHA256 -LiteralPath $f).Hash.ToLowerInvariant() } else { $null } }
    $entDir = Join-Path $Esp 'loader\entries'
    $names = @()
    if (Test-Path -LiteralPath $entDir) { $names = @(Get-ChildItem -LiteralPath $entDir -File | ForEach-Object { $_.Name }) }
    $lc = Join-Path $Esp 'loader\loader.conf'
    $lcDefault = $null
    if (Test-Path -LiteralPath $lc) { $lcDefault = Get-LoaderConfDefault ([string](Get-Content -LiteralPath $lc -Raw)) }
    $wid = 'auto-windows'
    if (Test-Path -LiteralPath (Join-Path $Esp 'loader\entries\gk3-windows.conf')) { $wid = 'gk3-windows.conf' }   # 与 gk3boot.c 的 win_entry_id 同一条规则
    $r = [ordered]@{
        Android = (@($names | Where-Object { $_ -like '*-android-*.conf' }).Count -gt 0)
        Entries = $names
        Foreign = @($names | Where-Object { $_ -like '*.conf' -and -not (Test-OurEntryName $_) })
        BootAA64Hash = (& $hash 'EFI\Boot\bootaa64.efi')
        SdBootHash = (& $hash 'EFI\systemd\systemd-bootaa64.efi')
        BootmgfwHash = (& $hash 'EFI\Microsoft\Boot\bootmgfw.efi')
        Backup = (Test-Path -LiteralPath (Join-Path $Esp 'EFI\Boot\bootaa64.efi.before-gaokun3'))
        LoaderConf = (Test-Path -LiteralPath $lc)
        LoaderDefault = $lcDefault
        Gk3boot = (Test-Path -LiteralPath (Join-Path $Esp 'EFI\gk3boot'))
        Live = (Test-Path -LiteralPath (Join-Path $Esp 'EFI\gaokun3'))
        WindowsEntryId = $wid
        Bootmgfw = $false
        State = ''
    }
    $r.Bootmgfw = [bool]$r.BootmgfwHash
    if (-not $r.Android) { $r.State = 'no-android' }
    elseif (-not $r.SdBootHash) { $r.State = 'missing-sdboot' }
    elseif (-not $r.BootAA64Hash) { $r.State = 'missing-bootaa64' }
    elseif ($r.BootAA64Hash -eq $r.SdBootHash) { $r.State = 'ok' }
    elseif ($r.BootmgfwHash -and $r.BootAA64Hash -eq $r.BootmgfwHash) { $r.State = 'replaced-by-windows' }
    else { $r.State = 'other' }
    return $r
}

function Show-EspReport($R) {
    $st = @{
        'no-android' = (T '没装 Android（ESP 上没有 *-android-*.conf）' 'Android is not installed (no *-android-*.conf on the ESP)')
        'missing-sdboot' = (T '✗ ESP 上没有 \EFI\systemd\systemd-bootaa64.efi —— 修不了，用 U 盘 live 的"修复启动"' '✗ \EFI\systemd\systemd-bootaa64.efi is missing - cannot repair here; use the USB live "repair boot"')
        'missing-bootaa64' = (T '✗ \EFI\Boot\bootaa64.efi 不在 —— 可以修' '✗ \EFI\Boot\bootaa64.efi is missing - repairable')
        'ok' = (T '✓ 开机的启动器（\EFI\Boot\bootaa64.efi）就是 systemd-boot' '✓ the boot loader (\EFI\Boot\bootaa64.efi) is systemd-boot')
        'replaced-by-windows' = (T '✗ \EFI\Boot\bootaa64.efi 被换成了 Windows 启动管理器 —— Android 进不去；可以修' '✗ \EFI\Boot\bootaa64.efi was replaced by the Windows Boot Manager - Android cannot start; repairable')
        'other' = (T '? \EFI\Boot\bootaa64.efi 既不是 systemd-boot 也不是 Windows 启动管理器 —— 不动它' '? \EFI\Boot\bootaa64.efi is neither systemd-boot nor the Windows Boot Manager - left alone')
    }
    Write-Host $st[$R.State]
    $yn = { param($b) if ($b) { T '有' 'yes' } else { T '没有' 'no' } }
    Say "  loader.conf：$(& $yn $R.LoaderConf)，default = $($R.LoaderDefault)" "  loader.conf: $(& $yn $R.LoaderConf), default = $($R.LoaderDefault)"
    Say "  启动项 $(@($R.Entries).Count) 个；不是我们写的：$(@($R.Foreign) -join ', ')" "  $(@($R.Entries).Count) boot entries; not ours: $(@($R.Foreign) -join ', ')"
    Say "  统一启动入口 EFI\gk3boot：$(& $yn $R.Gk3boot)；安装器 EFI\gaokun3：$(& $yn $R.Live)；Windows 启动管理器：$(& $yn $R.Bootmgfw)（条目 id $($R.WindowsEntryId)）" `
        "  EFI\gk3boot: $(& $yn $R.Gk3boot); EFI\gaokun3: $(& $yn $R.Live); Windows Boot Manager: $(& $yn $R.Bootmgfw) (entry id $($R.WindowsEntryId))"
}

# ── 伴随工具：开机自检（SYSTEM 计划任务，默认开、不可关，U23）────────────────────

function Invoke-NormalizeDefault {
    # §4.9.3：LoaderEntryDefault 只允许"不存在"或 Windows 条目 id，别的一律删（= 默认 Android）。与 gk3boot 的 dual_defaults 同一条规则。
    # 读不出（不是"不存在"）就什么都不做 —— 不去删一个看不见的东西
    $v = Read-LoaderVar 'LoaderEntryDefault'
    if ($v.State -eq 'error') { return @{ Class = 'unreadable'; Value = $null; Reset = $false; Error = "Win32 $($v.Code)" } }
    $cls = 'absent'; if ($v.State -eq 'present') { $cls = Get-DefVarClass $v.Value }
    if ($cls -ne 'other') { return @{ Class = $cls; Value = $v.Value; Reset = $false; Error = $null } }
    $e = Remove-LoaderVar 'LoaderEntryDefault'
    return @{ Class = 'other'; Value = $v.Value; Reset = (-not $e); Error = $e }
}

function Disable-FastStartupDual($Companion) {
    # U18：双系统一律关快速启动（原先只在"要到安装器里缩 D:"那条路上关）。旧值只记第一次，卸载时恢复
    if ((Get-FastStartup) -eq 1) {
        Set-FastStartup 0
        if ($null -eq $Companion['fastStartupWas']) { $Companion['fastStartupWas'] = 1 }
        return $true
    }
    return $false
}

function Invoke-HibernateChoice($Companion) {
    # U24：Modern Standby 会在待机中【自动】转入休眠（§4.9.10），之后冷开机进 Android 就会在 Windows 休眠时改写共用的 ESP
    if ((Get-HibernateEnabled) -ne 1) { return }
    $off = $false
    if ($Hibernate -eq 'off') { $off = $true }
    elseif ($Hibernate -eq 'ask') {
        Warn 'Windows 的休眠开着。平板合盖待机久了 Windows 会【自己】转入休眠；那时开机进 Android，Android 写共用的 EFI 分区可能把它写坏。' "Hibernation is on. On this tablet Windows hibernates BY ITSELF after a while in standby; booting Android then writes the shared EFI partition while Windows is hibernated, which can corrupt it."
        Warn '建议关掉（powercfg /hibernate off）。代价：待机时电量耗尽会直接掉电，没保存的东西会丢。' 'Recommended: turn it off (powercfg /hibernate off). Cost: if the battery runs out in standby the machine just powers off and unsaved work is lost.'
        $off = Confirm-YesNo '关掉 Windows 的休眠吗？' 'Turn off hibernation in Windows?'
    }
    if ($off) {
        try {
            Set-Hibernate $false
            if ($null -eq $Companion['hibernateWas']) { $Companion['hibernateWas'] = 1 }
            Say '休眠已关（卸载 Android 时恢复）' 'hibernation is off (restored when Android is removed)'
        } catch { Warn "关休眠失败：$($_.Exception.Message)" "could not turn hibernation off: $($_.Exception.Message)" }
    } else {
        Say '休眠保持开着：Windows 里请用"关机"，别在 Windows 休眠时切到 Android。' 'Hibernation stays on: shut Windows down rather than hibernating it before switching to Android.'
    }
}

function Restore-PowerSettings {
    # 卸载时恢复：快速启动（伴随工具或免 U 盘安装关掉的）、休眠（U24 关掉的）
    $c = Read-Companion
    $s = Read-JsonFile $StateFile
    $fsWas = $c['fastStartupWas']
    if ($null -eq $fsWas -and $s) { $fsWas = $s['fastStartupWas'] }
    if ($fsWas -eq 1) { Set-FastStartup 1; Say '快速启动已恢复成开着' 'Fast Startup is back on' }
    if ($c['hibernateWas'] -eq 1) {
        try { Set-Hibernate $true; Say '休眠已恢复成开着' 'hibernation is back on' } catch { Warn "恢复休眠失败：$($_.Exception.Message)" "could not turn hibernation back on: $($_.Exception.Message)" }
    }
}

function Invoke-DualHousekeeping($Companion) {
    # Android 装上之后的一次性收尾（每次开机都看一眼，做过了就什么都不做）
    $notes = @()
    # a. 免 U 盘安装建的 bcdedit 对象（"gaokun3 installer"，$state.bcd）：H-A 下对应的固件项早被删了，但 Windows 的 BCD 里还留着
    #    {bootmgr} 的副本（§4.9.15）。EFI\gaokun3（live）留着：它是"重新安装"的安全网
    $st = Read-JsonFile $StateFile
    if ($st -and $st['bcd']) {
        try { Invoke-Bcd @('/delete', [string]$st['bcd']) | Out-Null; $notes += "bcd $($st['bcd']) deleted" }
        catch { $notes += "bcd $($st['bcd']) delete failed: $($_.Exception.Message)" }
        $st['bcd'] = $null
        Write-JsonFile $StateFile $st
    }
    # b. GK3LIVE 去盘符（U19）：只用 Remove-PartitionAccessPath，不设 GPT 属性位（§4.9.11）
    try {
        foreach ($p in @(Get-LivePartitions)) {
            if (Test-HasDriveLetter $p) { $l = [string]$p.DriveLetter; $p | Remove-PartitionAccessPath -AccessPath "$($l):\"; $notes += "GK3LIVE drive letter $l removed" }
        }
    } catch { $notes += "GK3LIVE: $($_.Exception.Message)" }
    # c. 快速启动（U18）：功能更新可能又把它打开
    if (Disable-FastStartupDual $Companion) { $notes += 'fast startup was on again: turned off' }
    return $notes
}

function Invoke-BootCheck {
    # ① BOOTAA64 自检 ② LoaderEntryDefault 规范化 ③ BIOS 版本变化；（可选）U25 的 Windows 侧预置。
    # 结果写 %ProgramData%\gaokun3\boot-check.json，用户登录时 -Notify 读它弹窗（SYSTEM 在开机时没有桌面可弹）
    $c = Read-Companion
    $old = Read-JsonFile $StatusFile
    $issues = New-Object System.Collections.ArrayList
    $notes = New-Object System.Collections.ArrayList
    $nowTicks = [DateTime]::UtcNow.Ticks
    $stamp = (Get-Date).ToString('yyyyMMddHHmmss')
    $rep = $null; $def = $null
    try {
        $esp = Mount-Esp
        try {
            $rep = Get-EspReport $esp
            if ($rep.Android) {
                switch ($rep.State) {
                    'replaced-by-windows' { [void]$issues.Add([ordered]@{ id = 'bootaa64-replaced'; key = [string]$rep.BootAA64Hash; detail = ''; ticks = $nowTicks }) }
                    'missing-bootaa64' { [void]$issues.Add([ordered]@{ id = 'bootaa64-replaced'; key = 'missing'; detail = 'missing'; ticks = $nowTicks }) }
                    'other' { [void]$issues.Add([ordered]@{ id = 'bootaa64-other'; key = [string]$rep.BootAA64Hash; detail = ''; ticks = $nowTicks }) }
                    'missing-sdboot' { [void]$issues.Add([ordered]@{ id = 'sdboot-missing'; key = 'missing'; detail = ''; ticks = $nowTicks }) }
                }
                try {
                    $def = Invoke-NormalizeDefault
                    if ($def.Reset) { [void]$issues.Add([ordered]@{ id = 'default-reset'; key = $stamp; detail = [string]$def.Value; ticks = $nowTicks }) }
                    elseif ($def.Error) { [void]$notes.Add("LoaderEntryDefault: $($def.Error)") }
                } catch { [void]$notes.Add("LoaderEntryDefault: $($_.Exception.Message)") }
                # U25（可选，默认关）：Windows 每次开机写 OneShot = Windows 条目 ⇒ Windows 里的任何重启都回 Windows；关机时由 -TaskAction WindowsShutdown 删
                if ($c['windowsPreset'] -and $rep.Bootmgfw -and $rep.State -eq 'ok') {
                    try {
                        $o = Read-LoaderVar 'LoaderEntryOneShot'
                        if ($o.State -eq 'present' -and (Get-DefVarClass $o.Value) -ne 'windows') { [void]$notes.Add("OneShot already '$($o.Value)': preset skipped") }
                        else {
                            $e = Write-LoaderVar 'LoaderEntryOneShot' $rep.WindowsEntryId
                            if ($e) { [void]$notes.Add("preset OneShot failed: $e") } else { [void]$notes.Add("preset OneShot = $($rep.WindowsEntryId)") }
                        }
                    } catch { [void]$notes.Add("preset OneShot: $($_.Exception.Message)") }
                }
            }
        } finally { Dismount-Esp $esp }
    } catch { [void]$issues.Add([ordered]@{ id = 'esp-unreadable'; key = $stamp; detail = [string]$_.Exception.Message; ticks = $nowTicks }) }
    if ($rep -and $rep.Android) { foreach ($n in @(Invoke-DualHousekeeping $c)) { [void]$notes.Add($n) } }
    # ③ BIOS 版本变化（第一次只记基线）
    $bios = Get-BiosVersion
    if ($bios) {
        if ($c['biosVersion'] -and [string]$c['biosVersion'] -ne $bios) { [void]$issues.Add([ordered]@{ id = 'bios-changed'; key = "$($c['biosVersion'])->$bios"; detail = "$($c['biosVersion']) -> $bios"; ticks = $nowTicks }) }
        $c['biosVersion'] = $bios
    }
    if ($rep -and $rep.Android -and (Get-SecureBootOn) -eq $true) { [void]$issues.Add([ordered]@{ id = 'secureboot-on'; key = 'on'; detail = ''; ticks = $nowTicks }) }
    # 事件类的提示（默认项被重置、BIOS 变了）留 7 天：这次开机与用户登录之间可能又重启过
    if ($old -and $old['issues']) {
        foreach ($i in @($old['issues'])) {
            if ($null -eq $i) { continue }
            if (@('default-reset', 'bios-changed') -notcontains $i['id']) { continue }
            if (([long]$nowTicks - [long]$i['ticks']) -gt [TimeSpan]::FromDays(7).Ticks) { continue }
            $dup = $false; foreach ($j in $issues) { if ($j['id'] -eq $i['id'] -and $j['key'] -eq $i['key']) { $dup = $true } }
            if (-not $dup) { [void]$issues.Add($i) }
        }
    }
    Write-JsonFile $CompanionFile $c
    $state = ''; $android = $false
    if ($rep) { $state = $rep.State; $android = $rep.Android }
    $defClass = $null; if ($def) { $defClass = $def.Class }
    $status = [ordered]@{ version = $ToolVersion; checkedAt = (Get-Date).ToString('o'); android = $android; bootaa64 = $state
                          loaderEntryDefault = $defClass; issues = $issues.ToArray(); notes = $notes.ToArray() }
    Write-JsonFile $StatusFile $status
    Write-Log ("boot-check: android=$android bootaa64=$state default=$defClass issues=" + (@($issues | ForEach-Object { $_['id'] }) -join ',') + ' notes=' + ($notes -join '; '))
    return $status
}

function Invoke-Notify {
    # 用户登录时（计划任务 \gaokun3\Notify，以登录用户身份）：把开机自检的结果弹出来。确认过的记在 %LOCALAPPDATA%\gaokun3\ack.json
    $st = Read-JsonFile $StatusFile
    if (-not $st) { return 0 }
    $ackFile = Join-Path (Join-Path (Get-SpecialDir 'LocalApplicationData' 'gk3-LocalAppData') 'gaokun3') 'ack.json'
    $ack = Read-JsonFile $ackFile
    $acked = @(); if ($ack -and $ack['acked']) { $acked = @($ack['acked']) }
    $show = @(Get-PendingNotices $st['issues'] $acked)
    foreach ($i in $show) {
        $t = Get-NoticeText $i
        $yes = Show-Box $t[0] $t[1] ([bool]$t[2])
        if ($t[2] -and $yes) { Start-Process -FilePath (Join-Path $InstallDir 'gaokun3-setup.cmd') -ArgumentList '-RepairBoot' }
        if ($i['id'] -ne 'bootaa64-replaced') { $acked += "$($i['id'])|$($i['key'])" }
    }
    if ($show.Count -gt 0) { Write-JsonFile $ackFile ([ordered]@{ acked = @($acked | Select-Object -Last 50) }) }
    return 0
}

# ── 伴随工具：动作 ──────────────────────────────────────────────────────────────

function Get-RebootToAndroidPlan {
    # §4.9.4：读 loader.conf 的 default（不用 *-android-* 宽通配：入口条目计数全用完时它会排到另一个槽的直连条目），
    # 写进 LoaderEntryOneShot、读回核对。⚠️ Windows 能不能写这个变量【未在真机验证】（D4 先在 Parallels 做原型，D6 真机）
    $esp = Mount-Esp
    try { $r = Get-EspReport $esp } finally { Dismount-Esp $esp }
    if (-not $r.Android) { return @{ Ok = $false; Message = (T '这台机器上没装 Android（ESP 上没有 Android 的启动项）。' 'Android is not installed (no Android boot entries on the ESP).') } }
    if ($r.State -ne 'ok') { return @{ Ok = $false; Message = (T '开机的启动器现在不是 systemd-boot（Windows 换掉了它？）—— 这样重启也进不了 Android。先运行开始菜单 gaokun3 → 修复 Android 启动。' 'The boot loader is not systemd-boot right now (replaced by Windows?), so a restart cannot reach Android. Run Start menu → gaokun3 → Repair Android boot first.') } }
    if (-not (Test-AndroidEntryPattern ([string]$r.LoaderDefault))) { return @{ Ok = $false; Message = (T "loader.conf 的 default（$($r.LoaderDefault)）不是 Android 条目 —— 不知道该进哪个槽。重启后在开机菜单里手选 Android。" "loader.conf's default ($($r.LoaderDefault)) is not an Android entry. Restart and pick Android in the boot menu.") } }
    $e = $null
    try { $e = Write-LoaderVar 'LoaderEntryOneShot' ([string]$r.LoaderDefault) } catch { $e = $_.Exception.Message }
    if ($e) { return @{ Ok = $false; Message = (T "写不进 LoaderEntryOneShot（$e）。Windows 能不能写 systemd-boot 的变量在这台机器上还没验证（设计稿 D4/D6）。请重启，在开机菜单里手选 Android。" "Could not write LoaderEntryOneShot ($e). Whether Windows can write systemd-boot's variables on this machine is not verified yet (design D4/D6). Restart and pick Android in the boot menu.") } }
    return @{ Ok = $true; Message = (T "下次开机进 Android（LoaderEntryOneShot = $($r.LoaderDefault)，已读回核对）。" "The next boot goes to Android (LoaderEntryOneShot = $($r.LoaderDefault), read back OK).") }
}

function Set-DefaultOs([string]$Os) {
    # §4.9.3：Windows 为默认 = LoaderEntryDefault 写 Windows 条目 id；Android 为默认 = 删掉它（什么都不设）。
    # Android 侧的默认系统缓存（GK3 记录）由 gk3boot 下次运行时自己更新
    $esp = Mount-Esp
    try { $r = Get-EspReport $esp } finally { Dismount-Esp $esp }
    if (-not $r.Android) { return @{ Ok = $false; Message = (T '这台机器上没装 Android。' 'Android is not installed.') } }
    $e = $null; $msg = ''
    if ($Os -eq 'windows') {
        if (-not $r.Bootmgfw) { return @{ Ok = $false; Message = (T 'ESP 上没有 \EFI\Microsoft\Boot\bootmgfw.efi —— 开机菜单里不会有 Windows，不能设成默认。' 'No \EFI\Microsoft\Boot\bootmgfw.efi on the ESP, so Windows is not in the boot menu.') } }
        try { $e = Write-LoaderVar 'LoaderEntryDefault' $r.WindowsEntryId } catch { $e = $_.Exception.Message }
        $msg = T "开机默认进 Windows（LoaderEntryDefault = $($r.WindowsEntryId)，下次开机生效）。" "Windows is now the default (LoaderEntryDefault = $($r.WindowsEntryId), from the next boot)."
        if (-not $r.Gk3boot) { $msg += T ' ⚠️ 这台机器上还没有统一启动入口（EFI\gk3boot）：在 Android 里重启也会落到 Windows（§4.9.3）。' ' Note: this machine has no gk3boot entry yet, so restarting from Android also lands in Windows.' }
    } else {
        try { $e = Remove-LoaderVar 'LoaderEntryDefault' } catch { $e = $_.Exception.Message }
        $msg = T '开机默认进 Android（LoaderEntryDefault 已删，下次开机生效）。' 'Android is now the default (LoaderEntryDefault removed, from the next boot).'
    }
    if ($e) { return @{ Ok = $false; Message = (T "改不了 LoaderEntryDefault（$e）。Windows 能不能写这个变量在这台机器上还没验证（D4/D6）。可以在开机菜单里高亮 Windows 按 d 设默认、对已是默认的那一项再按一次 d 撤销。" "Could not change LoaderEntryDefault ($e); not verified on this machine (D4/D6). In the boot menu, highlight Windows and press d to make it the default (press d again on the default entry to undo).") } }
    return @{ Ok = $true; Message = $msg }
}

function Write-ActionResult([string]$Action, $Res) {
    Write-JsonFile (Join-Path $DataDir "result-$Action.json") ([ordered]@{ action = $Action; ticks = [DateTime]::UtcNow.Ticks; ok = [bool]$Res.Ok; message = [string]$Res.Message })
}

function Invoke-WindowsShutdownHook {
    # U25 的另一半：Windows【关机】（不是重启）时，把开机写的 OneShot = Windows 条目删掉（只删还是那个值的，比较后删除），
    # 下次冷开机就回默认系统。分不清关机还是重启时按关机处理（安全的一侧）。⚠️ 判据未验证（D4 ⑨）
    $c = Read-Companion
    if (-not $c['windowsPreset']) { return @{ Ok = $true; Message = 'preset off: nothing to do' } }
    $kind = Get-ShutdownKind (Get-LastShutdownType)
    if ($kind -eq 'restart') { return @{ Ok = $true; Message = 'restart: OneShot kept (back to Windows)' } }
    $v = Read-LoaderVar 'LoaderEntryOneShot'
    if ($v.State -eq 'present' -and (Get-DefVarClass $v.Value) -eq 'windows') {
        $e = Remove-LoaderVar 'LoaderEntryOneShot'
        if ($e) { return @{ Ok = $false; Message = "$($kind): $e" } }
        return @{ Ok = $true; Message = "$($kind): OneShot removed" }
    }
    return @{ Ok = $true; Message = "$($kind): OneShot is $($v.State) '$($v.Value)'; left alone" }
}

function Invoke-TaskAction([string]$Action) {
    # SYSTEM 计划任务的固定动作。结果写 result-<动作>.json，开始菜单那边（-Trigger）等着读
    Write-Log "task $Action"
    $res = $null
    try {
        switch ($Action) {
            'RebootToAndroid' { $res = Get-RebootToAndroidPlan }
            'DefaultAndroid' { $res = Set-DefaultOs 'android' }
            'DefaultWindows' { $res = Set-DefaultOs 'windows' }
            'WindowsShutdown' { $res = Invoke-WindowsShutdownHook }
        }
    } catch { $res = @{ Ok = $false; Message = [string]$_.Exception.Message } }
    Write-ActionResult $Action $res
    Write-Log "task $Action -> ok=$($res.Ok) $($res.Message)"
    if ($Action -eq 'RebootToAndroid' -and $res.Ok) { $null = Invoke-Native 'shutdown.exe' @('/r', '/t', '3') }
    $code = 1; if ($res.Ok) { $code = 0 }
    return $code
}

function Invoke-Trigger([string]$Action) {
    # 开始菜单项（不提权）：schtasks /Run 触发固定的 SYSTEM 任务，再等它写的结果（最多 30 秒）
    Show-Preview
    $t0 = [DateTime]::UtcNow.Ticks
    $cli = @{ RebootToAndroid = '-RebootToAndroid'; DefaultAndroid = '-SetDefault android'; DefaultWindows = '-SetDefault windows' }[$Action]
    $r = Invoke-Native 'schtasks.exe' @('/Run', '/TN', "$TaskFolder$Action")
    if ($r.Code -ne 0) { Fail "触发计划任务 $TaskFolder$Action 失败（$($r.Code)）：伴随工具没装好，或者这个账户没有权限运行它（未验证）。可以改为以管理员身份运行 gaokun3-setup.cmd $cli" "could not start the task $TaskFolder$Action ($($r.Code)): the companion is not installed, or this account may not run it (unverified). Run gaokun3-setup.cmd $cli as administrator instead." }
    $f = Join-Path $DataDir "result-$Action.json"
    $deadline = (Get-Date).AddSeconds(30)
    $res = $null
    while ($true) {
        $x = Read-JsonFile $f
        if ($x -and [long]$x['ticks'] -ge $t0) { $res = $x; break }
        if ((Get-Date) -gt $deadline) { break }
        Start-Sleep -Milliseconds 300
    }
    if (-not $res) { Fail "30 秒内没等到 $Action 的结果（看 $LogFile）" "no result from $Action within 30 s (see $LogFile)" }
    if ($res['ok']) {
        Write-Host $res['message']
        if ($Action -eq 'RebootToAndroid') { Say '马上重启……' 'Restarting...' }
        else { Start-Sleep -Seconds 4 }   # 开始菜单开的窗口跑完就关：留几秒让人看见结果
        return 0
    }
    Write-Host ('!  ' + $res['message']) -ForegroundColor Yellow
    if (-not $Yes) { $null = Read-Answer (T '按回车关闭' 'Press Enter to close') }
    return 1
}

function Invoke-RebootToAndroidCli {
    Assert-Admin; Show-Preview
    $r = Get-RebootToAndroidPlan
    if (-not $r.Ok) { Fail $r.Message $r.Message }
    Write-Host $r.Message
    if ($NoReboot) { return 0 }
    $null = Invoke-Native 'shutdown.exe' @('/r', '/t', '0')
    return 0
}

function Invoke-SetDefaultCli([string]$Os) {
    Assert-Admin; Show-Preview
    $r = Set-DefaultOs $Os
    if (-not $r.Ok) { Fail $r.Message $r.Message }
    Write-Host $r.Message
    return 0
}

function Invoke-SuspendBitLockerOnly {
    # §4.9.15"仅暂停 BitLocker"：只执行暂停 2 次重启，不碰别的（U 盘路径"关安全启动之前"那一步，§4.9.7 规则 6）
    Assert-Admin; Show-Preview
    $bl = Get-OsBitLockerState
    $sd = Get-SystemDrive
    if ($bl -eq 'off') { Say "$sd 没开着 BitLocker 保护 —— 不用暂停。" "BitLocker protection is off on $sd - nothing to suspend."; return 0 }
    if ($bl -eq 'unknown') { Warn '读不到 BitLocker 状态（家庭版可能没有 BitLocker 模块），照样试一次。' 'BitLocker state unreadable (Home edition may lack the module); trying anyway.' }
    Warn '先确认你拿得到恢复密钥：https://aka.ms/myrecoverykey （设备加密的密钥多半存在你的 Microsoft 账户里）。' 'Make sure you have the recovery key first: https://aka.ms/myrecoverykey (device encryption usually stores it in your Microsoft account).'
    if (Suspend-OsBitLocker 2) { Say "已暂停 $sd 的 BitLocker 保护 2 次重启，之后 Windows 自己恢复。" "BitLocker on $sd is suspended for 2 restarts; Windows resumes it by itself."; return 0 }
    Fail "暂停 $sd 的 BitLocker 失败。" "could not suspend BitLocker on $sd."
}

function Repair-BootAA64([string]$Esp) {
    $fb = Join-Path $Esp 'EFI\Boot\bootaa64.efi'
    $sd = Join-Path $Esp 'EFI\systemd\systemd-bootaa64.efi'
    New-Item -ItemType Directory -Force -Path (Join-Path $Esp 'EFI\Boot') | Out-Null
    # 备份只留第一份（安装器装机时已经留过出厂的那份，§4.7）：不拿 Windows 换回来的拷贝把它盖掉
    if ((Test-Path -LiteralPath $fb) -and -not (Test-Path -LiteralPath "$fb.before-gaokun3")) { Copy-Item -LiteralPath $fb -Destination "$fb.before-gaokun3"; Say '原来的 bootaa64.efi 留成了 .before-gaokun3' 'kept the previous bootaa64.efi as .before-gaokun3' }
    Copy-Item -Force -LiteralPath $sd -Destination $fb
    if ((Get-FileHash -Algorithm SHA256 -LiteralPath $fb).Hash -ne (Get-FileHash -Algorithm SHA256 -LiteralPath $sd).Hash) { Fail '拷完核对 sha256 不一致 —— ESP 可能满了或出错了' 'sha256 mismatch after the copy - the ESP may be full or failing' }
}

function Invoke-RepairBoot([bool]$CheckOnly) {
    # §4.9.6：1. 只读体检（-Check 到此为止）；2. bootaa64.efi 被换成 bootmgfw（或不在）时：BitLocker 开着先暂停 1 次重启，
    # 备份（已有不覆盖），拷回 systemd-boot；是别的东西只报告；3. U15（自有启动项）1.0 不做；4. 幂等，每一步都打印
    Assert-Admin; Show-Preview
    Step '检查启动（只读）' 'Checking the boot (read-only)'
    $esp = Mount-Esp
    try { $r = Get-EspReport $esp } finally { Dismount-Esp $esp }
    Show-EspReport $r
    if ($r.Android) {
        try {
            $v = Read-LoaderVar 'LoaderEntryDefault'
            $cls = 'absent'; if ($v.State -eq 'present') { $cls = Get-DefVarClass $v.Value } elseif ($v.State -eq 'error') { $cls = "unreadable (Win32 $($v.Code))" }
            Say "  LoaderEntryDefault：$($v.Value)（$cls）" "  LoaderEntryDefault: $($v.Value) ($cls)"
        } catch { Say "  LoaderEntryDefault 读不出：$($_.Exception.Message)（未验证）" "  LoaderEntryDefault unreadable: $($_.Exception.Message) (unverified)" }
    }
    if ($CheckOnly) { if (@('ok', 'no-android') -contains $r.State) { return 0 } return 2 }
    switch ($r.State) {
        'ok' { Say '不用修。' 'Nothing to repair.'; return 0 }
        'no-android' { Say '没装 Android，没什么可修的。' 'Android is not installed; nothing to repair.'; return 0 }
        'missing-sdboot' { Fail 'ESP 上没有 \EFI\systemd\systemd-bootaa64.efi，这里修不了 —— 用 U 盘 live 的"修复启动"。' 'no \EFI\systemd\systemd-bootaa64.efi on the ESP - use the USB live "repair boot".' }
        'other' { Warn '\EFI\Boot\bootaa64.efi 是别的东西（另一个 Linux 的引导？）—— 只报告、不动。' '\EFI\Boot\bootaa64.efi is something else (another Linux loader?) - reported only, not touched.'; return 2 }
    }
    # replaced-by-windows / missing-bootaa64
    $sd = Get-SystemDrive
    $bl = Get-OsBitLockerState
    if ($bl -ne 'off') {
        Warn '拷回 systemd-boot 会改变 Windows 的启动链（PCR4），BitLocker 可能要恢复密钥。先确认你拿得到恢复密钥（https://aka.ms/myrecoverykey）。' 'Copying systemd-boot back changes the Windows boot chain (PCR4); BitLocker may ask for the recovery key. Make sure you have it (https://aka.ms/myrecoverykey).'
        if ($bl -eq 'unknown') { Warn '读不到 BitLocker 状态 —— 按"开着"处理。' 'BitLocker state unreadable - treating it as on.' }
        Confirm-Yes "我拿到了恢复密钥。接下来暂停 $sd 的 BitLocker 1 次重启，再把 systemd-boot 拷回 \EFI\Boot\bootaa64.efi。" "I have the recovery key. Next: suspend BitLocker on $sd for 1 restart, then copy systemd-boot back to \EFI\Boot\bootaa64.efi."
        if (Suspend-OsBitLocker 1) { Say "已暂停 $sd 的 BitLocker 1 次重启" "BitLocker on $sd suspended for 1 restart" }
        elseif ($bl -eq 'on') {
            Warn '暂停 BitLocker 失败：下次进 Windows 很可能要恢复密钥。' 'Could not suspend BitLocker: the next Windows boot will likely ask for the recovery key.'
            Confirm-Yes '仍然继续（手上一定要有恢复密钥）。' 'Continue anyway (keep the recovery key at hand).'
        }
    } else {
        Confirm-Yes '接下来把 systemd-boot 拷回 \EFI\Boot\bootaa64.efi（原来那份留成 .before-gaokun3，已有就不覆盖）。' 'Next: copy systemd-boot back to \EFI\Boot\bootaa64.efi (the previous file is kept as .before-gaokun3 unless one exists).'
    }
    $esp = Mount-Esp
    try { Repair-BootAA64 $esp } finally { Dismount-Esp $esp }
    Say '修好了：下次开机会出现 systemd-boot 菜单。' 'Repaired: the systemd-boot menu appears on the next boot.'
    Write-Log 'repair-boot: systemd-boot copied back'
    return 0
}

function Set-WindowsPresetOption([string]$OnOff) {
    # U25：做成选项、默认关；设计稿写的是"D4/D6 证实能写变量、D4 找到可靠的关机 / 重启判据之后默认开"
    Assert-Admin; Show-Preview
    $c = Read-Companion
    $on = ($OnOff -eq 'on')
    $c['windowsPreset'] = $on
    Write-JsonFile $CompanionFile $c
    if ($on) {
        Warn '【未验证】这一项靠两件还没证实的事：Windows 能写 LoaderEntryOneShot（D4/D6）、系统日志 Event 1074 能可靠分清"关机"和"重启"（D4 ⑨）。分不清时按关机处理（下次冷开机回默认系统）。' '[Unverified] This relies on two unproven things: Windows can write LoaderEntryOneShot (D4/D6), and Event 1074 reliably tells shutdown from restart (D4 #9). When unsure it is treated as a shutdown.'
        $defs = Get-CompanionTaskDefs $true
        Register-Gk3Task 'WindowsShutdown' $defs['WindowsShutdown']
        $esp = Mount-Esp
        try { $r = Get-EspReport $esp } finally { Dismount-Esp $esp }
        if ($r.Android -and $r.Bootmgfw -and $r.State -eq 'ok') {
            $e = Write-LoaderVar 'LoaderEntryOneShot' $r.WindowsEntryId
            if ($e) { Warn "现在写 OneShot 失败：$e" "writing the OneShot now failed: $e" } else { Say "LoaderEntryOneShot = $($r.WindowsEntryId)" "LoaderEntryOneShot = $($r.WindowsEntryId)" }
        }
        Say '已打开：Windows 里的重启（含更新的自动重启）回 Windows；关机后冷开机回默认系统。' 'On: restarts from Windows (including update restarts) come back to Windows; a cold boot after shutdown goes to the default system.'
    } else {
        Unregister-Gk3Task 'WindowsShutdown'
        try {
            $v = Read-LoaderVar 'LoaderEntryOneShot'
            if ($v.State -eq 'present' -and (Get-DefVarClass $v.Value) -eq 'windows') { $null = Remove-LoaderVar 'LoaderEntryOneShot' }
        } catch { }
        Say '已关闭（默认）：Windows 里的重启按开机默认系统走。' 'Off (default): restarts from Windows follow the default system.'
    }
    return 0
}

# ── 伴随工具：安装 / 卸载 ─────────────────────────────────────────────────────────

function Get-CompanionTaskDefs([bool]$Preset) {
    $ps = Get-PowerShellExe
    $ps1 = Join-Path $InstallDir 'gaokun3-setup.ps1'
    $base = "-NoProfile -NonInteractive -ExecutionPolicy Bypass -WindowStyle Hidden -File ""$ps1"""
    $d = [ordered]@{}
    $d['BootCheck'] = New-TaskXml -Description 'gaokun3 (preview): boot check - BOOTAA64, LoaderEntryDefault, BIOS version' -Trigger boot -RunAs system -Command $ps -Arguments "$base -BootCheck"
    $d['Notify'] = New-TaskXml -Description 'gaokun3 (preview): show boot-check results at logon' -Trigger logon -RunAs users -Command $ps -Arguments "$base -Notify"
    $d['RebootToAndroid'] = New-TaskXml -Description 'gaokun3 (preview): restart into Android' -Trigger none -RunAs system -UsersMayRun -Command $ps -Arguments "$base -TaskAction RebootToAndroid"
    $d['DefaultAndroid'] = New-TaskXml -Description 'gaokun3 (preview): boot Android by default' -Trigger none -RunAs system -UsersMayRun -Command $ps -Arguments "$base -TaskAction DefaultAndroid"
    $d['DefaultWindows'] = New-TaskXml -Description 'gaokun3 (preview): boot Windows by default' -Trigger none -RunAs system -UsersMayRun -Command $ps -Arguments "$base -TaskAction DefaultWindows"
    if ($Preset) { $d['WindowsShutdown'] = New-TaskXml -Description 'gaokun3 (preview, U25): on Windows shutdown, drop the Windows OneShot' -Trigger event1074 -RunAs system -Command $ps -Arguments "$base -TaskAction WindowsShutdown" }
    return $d
}

function Get-ShortcutDefs {
    # 开始菜单项（§4.9.15）：前三个不提权、只触发固定的计划任务；后四个经 gaokun3-setup.cmd 请求管理员（要确认、可能动 BitLocker）
    $ps = Get-PowerShellExe
    $ps1 = Join-Path $InstallDir 'gaokun3-setup.ps1'
    $cmd = Join-Path $InstallDir 'gaokun3-setup.cmd'
    $trig = "-NoProfile -ExecutionPolicy Bypass -File ""$ps1"" -Trigger"
    return @(
        @{ Name = (T '重启到 Android' 'Restart into Android'); Target = $ps; Arguments = "$trig RebootToAndroid"; Description = (T 'gaokun3（预览）：下一次开机进 Android' 'gaokun3 (preview): boot Android next') },
        @{ Name = (T '开机默认进 Android' 'Boot Android by default'); Target = $ps; Arguments = "$trig DefaultAndroid"; Description = (T 'gaokun3（预览）：开机默认进 Android' 'gaokun3 (preview): Android by default') },
        @{ Name = (T '开机默认进 Windows' 'Boot Windows by default'); Target = $ps; Arguments = "$trig DefaultWindows"; Description = (T 'gaokun3（预览）：开机默认进 Windows' 'gaokun3 (preview): Windows by default') },
        @{ Name = (T '修复 Android 启动' 'Repair Android boot'); Target = $cmd; Arguments = '-RepairBoot'; Description = (T 'gaokun3（预览）：Windows 换掉启动器之后用' 'gaokun3 (preview): after Windows replaced the boot loader') },
        @{ Name = (T '检查启动状态' 'Check boot status'); Target = $cmd; Arguments = '-RepairBoot -Check'; Description = (T 'gaokun3（预览）：只读体检' 'gaokun3 (preview): read-only check') },
        @{ Name = (T '仅暂停 BitLocker' 'Suspend BitLocker only'); Target = $cmd; Arguments = '-SuspendBitLocker'; Description = (T 'gaokun3（预览）：暂停 2 次重启' 'gaokun3 (preview): suspend for 2 restarts') },
        @{ Name = (T '卸载 Android' 'Remove Android'); Target = $cmd; Arguments = '-RemoveAndroid'; Description = (T 'gaokun3（预览）：卸载 Android，回到纯 Windows' 'gaokun3 (preview): remove Android, back to Windows only') }
    )
}

function Install-Companion {
    Assert-Admin
    Show-Preview
    Step '安装 Windows 伴随工具' 'Installing the Windows companion'
    if ($env:PROCESSOR_ARCHITECTURE -and $env:PROCESSOR_ARCHITECTURE -ne 'ARM64') { Fail "这不是 ARM64 的 Windows（$env:PROCESSOR_ARCHITECTURE）" "this is not Windows on ARM ($env:PROCESSOR_ARCHITECTURE)" }
    $src = Split-Path -Parent $SelfPath
    New-Item -ItemType Directory -Force -Path $InstallDir | Out-Null
    $same = $false
    try { $same = ((Resolve-Path -LiteralPath $src).Path.TrimEnd('\', '/') -eq (Resolve-Path -LiteralPath $InstallDir).Path.TrimEnd('\', '/')) } catch { }
    if (-not $same) {
        foreach ($f in 'gaokun3-setup.ps1', 'gaokun3-setup.cmd') {
            $p = Join-Path $src $f
            if (Test-Path -LiteralPath $p) { Copy-Item -Force -LiteralPath $p -Destination (Join-Path $InstallDir $f) }
        }
    }
    if (-not (Test-Path -LiteralPath (Join-Path $InstallDir 'gaokun3-setup.ps1'))) { Fail "$InstallDir 里没有 gaokun3-setup.ps1" "gaokun3-setup.ps1 is missing in $InstallDir" }
    Say "装到 $InstallDir（只有管理员能改，计划任务以 SYSTEM 跑的就是这一份）" "installed to $InstallDir (writable by administrators only; the SYSTEM tasks run this copy)"
    $c = Read-Companion
    $defs = Get-CompanionTaskDefs ([bool]$c['windowsPreset'])
    foreach ($n in @($defs.Keys)) { Register-Gk3Task $n $defs[$n] }
    if (-not $c['windowsPreset']) { Unregister-Gk3Task 'WindowsShutdown' }
    Say "计划任务 $($TaskFolder)：$(@($defs.Keys) -join ', ')" "scheduled tasks $($TaskFolder): $(@($defs.Keys) -join ', ')"
    New-Item -ItemType Directory -Force -Path $StartMenuDir | Out-Null
    Get-ChildItem -LiteralPath $StartMenuDir -Filter '*.lnk' -ErrorAction SilentlyContinue | Remove-Item -Force
    $sc = @(Get-ShortcutDefs)
    foreach ($s in $sc) { New-Shortcut (Join-Path $StartMenuDir ($s.Name + '.lnk')) $s.Target $s.Arguments $s.Description }
    Say "开始菜单 gaokun3：$(@($sc | ForEach-Object { $_.Name }) -join ' / ')" "Start menu gaokun3: $(@($sc | ForEach-Object { $_.Name }) -join ' / ')"
    # 一次性设置（U18 / U24）
    if (Disable-FastStartupDual $c) { Say '快速启动已关（双系统一律关，U18；卸载 Android 时恢复）' 'Fast Startup is off (always, with dual boot; restored when Android is removed)' }
    else { Say '快速启动本来就关着' 'Fast Startup was already off' }
    Invoke-HibernateChoice $c
    if (-not $c['biosVersion']) { $c['biosVersion'] = Get-BiosVersion }
    $c['version'] = $ToolVersion
    Write-JsonFile $CompanionFile $c
    Write-Log "installed $ToolVersion from $src"
    try { $null = Invoke-BootCheck; Say '开机自检跑了一次（以后每次开机由计划任务跑，默认开、不可关）' 'ran the boot check once (from now on the task runs it at every boot; always on)' }
    catch { Warn "开机自检第一次没跑成：$($_.Exception.Message)" "the first boot check failed: $($_.Exception.Message)" }
    Say 'Windows 侧预置（U25，Windows 里的重启回 Windows）默认关：要打开运行 gaokun3-setup.cmd -WindowsPreset on（未验证）' 'Windows-side preset (U25) is off by default: gaokun3-setup.cmd -WindowsPreset on to enable it (unverified)'
    return 0
}

function Uninstall-Companion {
    foreach ($n in $TaskNames) { Unregister-Gk3Task $n }
    Remove-Item -Recurse -Force -ErrorAction SilentlyContinue -LiteralPath $StartMenuDir
    foreach ($f in $CompanionFile, $StatusFile) { Remove-Item -Force -ErrorAction SilentlyContinue -LiteralPath $f }
    Get-ChildItem -LiteralPath $DataDir -Filter 'result-*.json' -ErrorAction SilentlyContinue | Remove-Item -Force
    if (Test-Path -LiteralPath $InstallDir) {
        $inside = $false
        try { $inside = ((Split-Path -Parent $SelfPath).TrimEnd('\', '/') -eq $InstallDir.TrimEnd('\', '/')) } catch { }
        # 正在从 %ProgramFiles%\gaokun3 里跑：等这个进程退出再删目录
        if ($inside) { Start-Process -WindowStyle Hidden -FilePath 'cmd.exe' -ArgumentList "/c ping -n 4 127.0.0.1 >nul & rmdir /s /q ""$InstallDir""" }
        else { Remove-Item -Recurse -Force -LiteralPath $InstallDir }
    }
    Say '伴随工具已卸载（计划任务、开始菜单、程序目录）' 'the companion is removed (tasks, Start menu, program folder)'
}

# ── 伴随工具：卸载 Android（U20，§4.9.13）──────────────────────────────────────────

function Restore-WindowsBoot([string]$Esp) {
    # 第 1 步：bootaa64.efi ← 【当前的】EFI\Microsoft\Boot\bootmgfw.efi；.before-gaokun3 只作最后的退路（它是装机那天的拷贝，
    # Windows 之后更新过的启动管理器只在 EFI\Microsoft\Boot 里）
    $fb = Join-Path $Esp 'EFI\Boot\bootaa64.efi'
    $mgr = Join-Path $Esp 'EFI\Microsoft\Boot\bootmgfw.efi'
    $bak = "$fb.before-gaokun3"
    New-Item -ItemType Directory -Force -Path (Join-Path $Esp 'EFI\Boot') | Out-Null
    $src = $null; $how = ''
    if (Test-Path -LiteralPath $mgr) { $src = $mgr; $how = 'bootmgfw' }
    elseif (Test-Path -LiteralPath $bak) { $src = $bak; $how = 'backup' }
    else { Fail 'ESP 上既没有 bootmgfw.efi 也没有 .before-gaokun3' 'neither bootmgfw.efi nor .before-gaokun3 on the ESP' }
    Copy-Item -Force -LiteralPath $src -Destination $fb
    if ((Get-FileHash -Algorithm SHA256 -LiteralPath $fb).Hash -ne (Get-FileHash -Algorithm SHA256 -LiteralPath $src).Hash) { Fail '还原 bootaa64.efi 后核对不一致' 'bootaa64.efi does not verify after the restore' }
    return $how
}

function Remove-OurEspFiles([string]$Esp, [bool]$Foreign) {
    # 第 2 步：只删我们的东西。绝不碰 EFI\Microsoft、EFI\UpdateCapsule、Persisted_Capsules.bin、OneKeyLog.txt（§4.9.8 / §4.9.11）。
    # ESP 上还有别的系统的启动项（共用 ESP 的另一个 Linux）时：只删我们的条目，保留 loader\ 与 EFI\systemd\
    $out = New-Object System.Collections.ArrayList
    $ent = Join-Path $Esp 'loader\entries'
    if (Test-Path -LiteralPath $ent) {
        foreach ($f in @(Get-ChildItem -LiteralPath $ent -File)) {
            if (Test-OurEntryName $f.Name) { Remove-Item -Force -LiteralPath $f.FullName; [void]$out.Add("loader\entries\$($f.Name)") }
        }
    }
    foreach ($d in @(Get-ChildItem -LiteralPath $Esp -Directory)) {
        if ($d.Name -match '^[0-9a-fA-F]{32}$' -and ((Test-Path -LiteralPath (Join-Path $d.FullName 'android')) -or (Test-Path -LiteralPath (Join-Path $d.FullName 'rescue')))) {
            Remove-Item -Recurse -Force -LiteralPath $d.FullName; [void]$out.Add("$($d.Name)\")
        }
    }
    $rels = @('EFI\gk3boot', 'EFI\gaokun3')
    if (-not $Foreign) { $rels += @('loader', 'EFI\systemd', 'EFI\Boot\bootaa64.efi.before-gaokun3') }
    foreach ($rel in $rels) {
        $p = Join-Path $Esp $rel
        if (Test-Path -LiteralPath $p) { Remove-Item -Recurse -Force -LiteralPath $p; [void]$out.Add($rel) }
    }
    if ($Foreign) {
        $lc = Join-Path $Esp 'loader\loader.conf'
        if (Test-Path -LiteralPath "$lc.before-gaokun3") { Copy-Item -Force -LiteralPath "$lc.before-gaokun3" -Destination $lc; Remove-Item -Force -LiteralPath "$lc.before-gaokun3"; [void]$out.Add('loader\loader.conf (restored from .before-gaokun3)') }
    }
    return $out.ToArray()
}

function Get-AndroidRemovalPlan {
    # 只读：系统盘的 GPT（裸读 —— Windows 的 Get-Partition 给不出 GPT 分区名）、Windows 看到的分区、每个候选分区开头 8 KiB、GK3LIVE、D:
    $sysDisk = Get-SystemDiskNumber
    $ss = [int](Get-Disk -Number $sysDisk).LogicalSectorSize
    if ($ss -le 0) { $ss = 512 }
    $gpt = @(Get-DiskGpt $sysDisk $ss)
    $parts = @(Get-Partition -DiskNumber $sysDisk)
    $byGuid = @{}
    foreach ($p in $parts) { $byGuid[(ConvertTo-NormGuid $p.Guid)] = $p }
    $items = @()
    foreach ($e in $gpt) {
        if ($AndroidPartNames -cnotcontains $e.Name) { continue }
        $p = $byGuid[(ConvertTo-NormGuid $e.Guid)]
        $off = [long]$e.FirstLba * $ss
        $kind = 'unreadable'
        try { $kind = Get-ContentKind (Read-DiskBytes $sysDisk $off 8192) } catch { }
        $items += [pscustomobject]@{ Name = $e.Name; Type = $e.Type; Guid = (ConvertTo-NormGuid $e.Guid); Offset = $off
                                     Size = ([long]$e.LastLba - [long]$e.FirstLba + 1) * $ss; Kind = $kind
                                     OffsetOk = ($null -ne $p -and [long]$p.Offset -eq $off) }
    }
    $check = Test-RemovalCandidates $items
    $errors = @($check.Errors); $notes = @($check.Notes)
    $live = $null
    foreach ($p in @(Get-LivePartitions)) {
        if (Test-LivePartitionContent $p) {
            if ($live) { $errors += (T '系统盘上有两个装着安装器的 GK3LIVE 分区 —— 认不准，不删' 'two GK3LIVE partitions with the installer on the system disk - ambiguous') }
            else { $live = $p }
        } else { $notes += (T '系统盘上有一个叫 GK3LIVE 的分区，但里面没有 gaokun3\live.squashfs —— 不删它' 'a GK3LIVE partition without gaokun3\live.squashfs - left alone') }
    }
    $st = Read-JsonFile $StateFile
    $drive = $ShrinkDrive
    if ($st -and $st['shrinkDrive']) { $drive = [string]$st['shrinkDrive'] }
    $target = $null
    try { $target = Get-Partition -DriveLetter $drive -ErrorAction Stop } catch { }
    $del = @($items | ForEach-Object { $_.Guid })
    if ($live) { $del += (ConvertTo-NormGuid $live.Guid) }
    if ($NoExtend) { $ext = @{ Adjacent = $false; Reason = 'no-extend' } }
    elseif (-not $target -or $target.DiskNumber -ne $sysDisk) { $ext = @{ Adjacent = $false; Reason = 'target-missing' } }
    else { $ext = Get-ExtendPlan $parts $target.Guid $del }
    return @{ Disk = $sysDisk; SectorSize = $ss; Items = $items; Errors = $errors; Notes = $notes; Live = $live
              Drive = $drive; Target = $target; Extend = $ext; DeleteGuids = $del }
}

function Invoke-RemoveAndroid {
    # 顺序是硬约束（§4.9.13）：0 预检 → 1 先还原引导 → 2 删 ESP 上我们的东西 → 3 删变量 → 4 删分区、扩 D:、还原快速启动 / 休眠 → 5 收尾。
    # 反过来先删分区的话，systemd-boot 默认仍进 Android、入口找不到分区、内核找不到 super、init 重启……一直循环到固件的"3 次关机"
    Assert-Admin; Show-Preview
    Step '0/5 预检（只读）' '0/5 Checks (read-only)'
    $plan = Get-AndroidRemovalPlan
    $esp = Mount-Esp
    try { $r = Get-EspReport $esp } finally { Dismount-Esp $esp }
    $foreign = (@($r.Foreign).Count -gt 0)
    $errs = @($plan.Errors)
    if (-not $foreign -and -not $r.Bootmgfw -and -not $r.Backup) { $errs += (T 'ESP 上找不到 Windows 启动管理器（\EFI\Microsoft\Boot\bootmgfw.efi），也没有 .before-gaokun3 备份 —— 删了 Android 就没有东西能启动了' 'no Windows Boot Manager (\EFI\Microsoft\Boot\bootmgfw.efi) and no .before-gaokun3 backup on the ESP - nothing would be left to boot') }
    if ($errs.Count -gt 0) {
        foreach ($e in $errs) { Write-Host ('!  ' + $e) -ForegroundColor Yellow }
        Fail '预检没过，什么都没改。' 'checks failed; nothing was changed.'
    }
    $names = @{}
    Say '将要删除的分区（都在系统盘上）：' 'partitions to delete (all on the system disk):'
    foreach ($it in $plan.Items) { $names[$it.Guid] = $it.Name; Write-Host ("  {0,-10} {1,8:N1} GiB  {2}" -f $it.Name, ($it.Size / 1GB), $it.Kind) }
    if ($plan.Live) { $g = ConvertTo-NormGuid $plan.Live.Guid; $names[$g] = $LiveLabel; Write-Host ("  {0,-10} {1,8:N1} GiB  installer" -f $LiveLabel, ($plan.Live.Size / 1GB)) }
    foreach ($n in $plan.Notes) { Write-Host "  (i) $n" }
    if ($foreign) { Warn "ESP 上还有别的系统的启动项（$(@($r.Foreign) -join ', ')）：只删我们的条目，保留 loader\ 与 EFI\systemd\，【不】还原回落路径 \EFI\Boot\bootaa64.efi —— 它放谁你自己决定" "the ESP has other systems' boot entries ($(@($r.Foreign) -join ', ')): only our entries are removed; loader\ and EFI\systemd\ stay and the fallback \EFI\Boot\bootaa64.efi is NOT restored - your call" }
    else { Say '启动：\EFI\Boot\bootaa64.efi 换回 Windows 启动管理器；删 ESP 上的 loader\、EFI\systemd\、EFI\gk3boot\、EFI\gaokun3\、Android 的目录；删 LoaderEntryDefault / LoaderEntryOneShot' 'boot: \EFI\Boot\bootaa64.efi back to the Windows Boot Manager; remove loader\, EFI\systemd\, EFI\gk3boot\, EFI\gaokun3\, the Android folder and LoaderEntryDefault / LoaderEntryOneShot' }
    $ext = $plan.Extend
    $drive = $plan.Drive
    if ($ext.Adjacent) {
        Say "删完把 $($drive): 扩回去（并进紧挨着它的 $(@($ext.Run).Count) 个分区的空间）" "afterwards $($drive): is grown into the space of the $(@($ext.Run).Count) partitions right after it"
        if (@($ext.Stranded).Count -gt 0) { Warn "另有 $(@($ext.Stranded).Count) 个分区不紧挨着 $($drive):，删了之后那段空间不会并回去（留成未分配）" "$(@($ext.Stranded).Count) partition(s) are not next to $($drive):; their space stays unallocated" }
    } else {
        $why = @{
            'no-extend' = (T '你给了 -NoExtend' 'you passed -NoExtend')
            'target-missing' = (T "系统盘上找不到 $($drive):" "no $($drive): on the system disk")
            'nothing-after' = (T "$($drive): 后面没有分区" "nothing after $($drive):")
            'next-kept' = (T "紧挨着 $($drive): 的不是要删的分区（出厂布局里那是 WINPE / 恢复分区 —— 不去动它）" "the partition right after $($drive): is not one being deleted (on the factory layout that is WINPE / recovery - left alone)")
        }[$ext.Reason]
        Warn "删完【不】扩 $($drive):：$why。删出来的空间会留成未分配。" "$($drive): will NOT be grown: $why. The freed space stays unallocated."
    }
    $bl = Get-OsBitLockerState
    if ($bl -ne 'off' -and -not $foreign) { Warn '还原 bootaa64.efi 会改变 Windows 的启动链，BitLocker 可能要恢复密钥。先确认你拿得到恢复密钥（https://aka.ms/myrecoverykey）；接下来会暂停 BitLocker 1 次重启。' 'Restoring bootaa64.efi changes the Windows boot chain; BitLocker may ask for the recovery key. Make sure you have it (https://aka.ms/myrecoverykey); BitLocker is suspended for 1 restart next.' }
    Confirm-Yes '上面列出的分区会被删除，里面的 Android 数据全部丢失（要留的先备份）。' 'The partitions above will be deleted and all Android data on them is lost (back up first).'
    if ($bl -ne 'off' -and -not $foreign) {
        if (Suspend-OsBitLocker 1) { Say 'BitLocker 已暂停 1 次重启' 'BitLocker suspended for 1 restart' }
        elseif ($bl -eq 'on') { Warn '暂停 BitLocker 失败：下次进 Windows 很可能要恢复密钥。' 'Could not suspend BitLocker: the next Windows boot will likely ask for the recovery key.'; Confirm-Yes '仍然继续（手上一定要有恢复密钥）。' 'Continue anyway (keep the recovery key at hand).' }
    }

    $esp = Mount-Esp
    try {
        Step '1/5 还原引导' '1/5 Restoring the Windows boot'
        if (-not $foreign) {
            $how = Restore-WindowsBoot $esp
            if ($how -eq 'backup') { Warn '用的是装机那天留的 .before-gaokun3（ESP 上找不到当前的 bootmgfw.efi）' 'used the .before-gaokun3 copy from install day (no current bootmgfw.efi)' }
            Say '\EFI\Boot\bootaa64.efi = Windows 启动管理器' '\EFI\Boot\bootaa64.efi = Windows Boot Manager'
        } else { Say '跳过（ESP 上还有别的系统）' 'skipped (other systems on the ESP)' }
        Step '2/5 删 ESP 上我们的东西' '2/5 Removing our files from the ESP'
        foreach ($x in @(Remove-OurEspFiles $esp $foreign)) { Write-Host "  - $x" }
    } finally { Dismount-Esp $esp }

    Step '3/5 删 Loader 变量' '3/5 Removing the Loader variables'
    foreach ($n in 'LoaderEntryDefault', 'LoaderEntryOneShot') {
        try {
            $v = Read-LoaderVar $n
            if ($v.State -ne 'present') { continue }
            if ($foreign -and -not (Test-OurVarValue $v.Value)) { Say "  $n = $($v.Value) 不是我们写的，留着" "  $n = $($v.Value) is not ours; kept"; continue }
            $e = Remove-LoaderVar $n
            if ($e) { Warn "  $n 没删掉（$e；未验证，留着无害）" "  $n not removed ($e; unverified, harmless)" } else { Say "  删了 $n" "  removed $n" }
        } catch { Warn "  $($n)：$($_.Exception.Message)（未验证，无害）" "  $($n): $($_.Exception.Message) (unverified, harmless)" }
    }

    Step '4/5 删分区' '4/5 Deleting the partitions'
    foreach ($g in $plan.DeleteGuids) {
        $p = @(Get-Partition -DiskNumber $plan.Disk | Where-Object { (ConvertTo-NormGuid $_.Guid) -eq $g })
        if ($p.Count -eq 0) { Warn "  $($names[$g]) 已经不在了" "  $($names[$g]) is already gone"; continue }
        Remove-Partition -DiskNumber $plan.Disk -PartitionNumber $p[0].PartitionNumber -Confirm:$false
        Say "  删了 $($names[$g])" "  deleted $($names[$g])"
    }
    if ($ext.Adjacent) {
        try { Update-Disk -Number $plan.Disk } catch { }
        $max = (Get-PartitionSupportedSize -DriveLetter $drive).SizeMax
        $cur = (Get-Partition -DriveLetter $drive).Size
        if ($max -gt $cur) { Resize-Partition -DriveLetter $drive -Size $max; Say "$($drive): 扩到了 $([math]::Round($max / 1GB, 1)) GiB" "$($drive): grown to $([math]::Round($max / 1GB, 1)) GiB" }
        else { Say "$($drive): 没有可并的空间" "no space to grow $($drive): into" }
    }
    Restore-PowerSettings
    $st = Read-JsonFile $StateFile
    if ($st -and $st['bcd']) { try { Invoke-Bcd @('/delete', [string]$st['bcd']) | Out-Null } catch { } }
    Remove-Item -Force -ErrorAction SilentlyContinue -LiteralPath $StateFile

    Step '5/5 收尾' '5/5 Finishing'
    Uninstall-Companion
    Write-Log 'remove-android: done'
    Say '卸载完成。重启后应当直接进 Windows（核对一下）。' 'Done. After a restart the machine should go straight into Windows (check it).'
    if (-not $NoReboot -and -not $Yes) {
        $a = Read-Answer (T '现在重启吗？(y/N)' 'Restart now? (y/N)')
        if ($a -eq 'y' -or $a -eq 'Y') { $null = Invoke-Native 'shutdown.exe' @('/r', '/t', '0') }
    }
    return 0
}

# ── 撤销 ─────────────────────────────────────────────────────────────────────

function Invoke-Uninstall {
    Step '撤销免 U 盘安装' 'Undoing the USB-free install'
    $state = $null
    if (Test-Path $StateFile) { $state = Get-Content $StateFile -Raw | ConvertFrom-Json }
    $esp = Mount-Esp
    try {
        $android = Test-AndroidInstalled $esp
        if ($state -and $state.bcd) {
            try { Invoke-Bcd @('/delete', $state.bcd) | Out-Null; Say "删了启动项 $($state.bcd)" "removed boot entry $($state.bcd)" }
            catch { Warn "删启动项失败（可能已经不在了）：$_" "could not remove the boot entry (maybe already gone): $_" }
        }
        Remove-Item -Recurse -Force -ErrorAction SilentlyContinue "$esp\EFI\gaokun3", "$esp\loader\entries\gaokun3-live.conf"
        if (-not $android) {
            if ($state -and $state.loaderConfCreated) {
                Remove-Item -Force -ErrorAction SilentlyContinue "$esp\loader\loader.conf"
                # loader\ 是我们建的：空了就一起删（2026-09-25 虚拟机实测，原先会留下空的 loader\entries）
                foreach ($d in "$esp\loader\entries", "$esp\loader") {
                    if ((Test-Path $d) -and -not (Get-ChildItem -Force $d)) { Remove-Item -Force $d }
                }
            }
            $bak = "$esp\EFI\Boot\bootaa64.efi.before-gaokun3"
            if ($state -and $state.fallback -and (Test-Path $bak)) {
                Copy-Item -Force $bak "$esp\EFI\Boot\bootaa64.efi"; Remove-Item -Force $bak
                Say '回落路径的 bootaa64.efi 已还原成原件' 'restored the original fallback bootaa64.efi'
            }
        }
        Say 'ESP 上的安装器文件已删' 'installer files removed from the ESP'
    } finally { Dismount-Esp $esp }
    $drive = $ShrinkDrive
    if ($state -and $state.shrinkDrive) { $drive = $state.shrinkDrive }
    if ($android) {
        # ★ Android 装上之后：EFI\gk3boot、EFI\systemd、Android 的启动项、loader.conf 一概不碰（上面只删了 EFI\gaokun3 与
        #   gaokun3-live.conf）；快速启动【不】恢复 —— 双系统时它必须关着（U18）；伴随工具留着（它负责开机自检与修复）
        Warn 'Android 已经装上了：GK3LIVE 分区与压缩出去的空间都留着（Android 在用那段空间）；快速启动保持关闭、伴随工具保留。要整个卸掉 Android 用 -RemoveAndroid。' 'Android is installed: the GK3LIVE partition and the shrunk space are left alone; Fast Startup stays off and the companion stays. To remove Android entirely use -RemoveAndroid.'
        return
    }
    Restore-PowerSettings
    # 只认系统盘上的 GK3LIVE：U 盘介质的卷标也是 GK3LIVE
    $p = @(Get-LivePartitions) | Select-Object -First 1
    if ($p) {
        Confirm-Yes "要删掉分区 $LiveLabel（磁盘 $($p.DiskNumber) 分区 $($p.PartitionNumber)），并把 $($drive): 扩回原来的大小。" "About to delete partition $LiveLabel and grow $($drive): back."
        Remove-Partition -DiskNumber $p.DiskNumber -PartitionNumber $p.PartitionNumber -Confirm:$false
    }
    if ($p -or ($state -and $state.shrunkBytes -gt 0)) {
        $max = (Get-PartitionSupportedSize -DriveLetter $drive).SizeMax
        Resize-Partition -DriveLetter $drive -Size $max
        Say "$($drive): 已扩回 $([math]::Round($max / 1GB, 1)) GiB" "$($drive): grown back to $([math]::Round($max / 1GB, 1)) GiB"
    }
    Uninstall-Companion
    Remove-Item -Force -ErrorAction SilentlyContinue $StateFile
    Say '撤销完成。' 'Done.'
}

# ── 主流程 ───────────────────────────────────────────────────────────────────

function Invoke-Setup {
    $here = Split-Path -Parent $PSCommandPath

    Step '1/5 预检' '1/5 Checks'
    Assert-Admin
    if ($env:PROCESSOR_ARCHITECTURE -ne 'ARM64') { Fail "这不是 ARM64 的 Windows（$env:PROCESSOR_ARCHITECTURE）" "this is not Windows on ARM ($env:PROCESSOR_ARCHITECTURE)" }
    $model = (Get-CimInstance Win32_ComputerSystem).Model
    if ($model -ne 'GK-W7X' -and -not $SkipModelCheck) { Fail "型号是 $model，这个安装器只给 MateBook E Go 2022（GK-W7X）" "model is $model; this installer is for the MateBook E Go 2022 (GK-W7X) only" }
    Say "型号 $model" "model $model"
    # ★ BitLocker 排在安全启动【之前】查（v1.0 计划 GUI-7 / INST-11）：关安全启动本身就会让绑定 PCR7 的
    #   BitLocker / 设备加密在下次开机要恢复密钥。原先这一条排在"安全启动开着就 Fail"之后 —— 用户照提示关掉
    #   安全启动、回到 Windows（可能当场就被要恢复密钥），重跑脚本才第一次看到"先确认拿得到恢复密钥"，已经晚了。
    #   现在：先确认恢复密钥，再暂停保护 2 次重启（关安全启动回来算一次、装完第一次经 systemd-boot 进 Windows 算一次；
    #   之后 Windows 自己恢复保护、按当时的启动路径重新封存 —— 按 Windows 的行为推断，没在本机上实测过），然后才让用户去关安全启动。
    # 先读安全启动状态（读不到 = 不是 UEFI，直接 Fail）、但【开着】的判定放到 BitLocker 之后：
    #   不是 UEFI 的机器不该先被暂停 BitLocker 再被拒（GUI 审查 2026-10-05）
    try { $sb = Confirm-SecureBootUEFI } catch { Fail '不是 UEFI 启动（或读不到安全启动状态）' 'not booted in UEFI mode (or Secure Boot state unreadable)' }
    $bl = $null
    $blSuspended = $false
    try { $bl = Get-BitLockerVolume -MountPoint $env:SystemDrive -ErrorAction Stop } catch { }
    if ($bl -and $bl.ProtectionStatus -eq 'On') {
        Warn "$env:SystemDrive 开着 BitLocker（或设备加密）。关安全启动、改了启动方式之后，Windows 下次开机可能要你输入恢复密钥。" "BitLocker (or device encryption) is on for $env:SystemDrive. After Secure Boot is turned off or the boot path changes, Windows may ask for the recovery key."
        Warn '先确认你拿得到恢复密钥（Microsoft 账户：https://aka.ms/myrecoverykey），再继续。（这一条没在本机上实测过）' 'Make sure you have the recovery key (https://aka.ms/myrecoverykey) before going on. (Not verified on this machine.)'
        Confirm-Yes "我已经拿到了恢复密钥。接下来暂停 $env:SystemDrive 的 BitLocker 保护 2 次重启。" "I have the recovery key. Next: suspend BitLocker on $env:SystemDrive for 2 restarts."
        try {
            Suspend-BitLocker -MountPoint $env:SystemDrive -RebootCount 2 -ErrorAction Stop | Out-Null
            $blSuspended = $true
            Say "已暂停 $env:SystemDrive 的 BitLocker 保护：接下来 2 次重启不会要恢复密钥，之后 Windows 自动恢复保护" "BitLocker on $env:SystemDrive is suspended for the next 2 restarts; Windows resumes it by itself afterwards"
        } catch {
            Warn "暂停 BitLocker 失败（$($_.Exception.Message)）：照样可以继续，但手上一定要有恢复密钥" "Could not suspend BitLocker ($($_.Exception.Message)): you can go on, but keep the recovery key at hand"
        }
    }
    if ($sb) {
        if ($blSuspended) { Fail '安全启动开着：内核没有签名，开着就起不来。BitLocker 已经暂停了 —— 现在重启进固件设置关掉安全启动，回到 Windows 后再运行本脚本' 'Secure Boot is on: the kernel is unsigned. BitLocker is suspended now - restart into firmware setup, turn Secure Boot off, then run this again from Windows' }
        Fail '安全启动开着：内核没有签名，开着就起不来。进固件设置关掉安全启动后再运行' 'Secure Boot is on: the kernel is unsigned. Turn Secure Boot off in firmware setup, then run this again'
    }
    Say '安全启动已关' 'Secure Boot is off'
    Test-Bundle $here
    Say '安装包校验通过（sha256）' 'bundle verified (sha256)'

    $sys = Get-Partition -DriveLetter $env:SystemDrive.Substring(0, 1)
    $disk = $sys.DiskNumber
    $shr = Get-Partition -DriveLetter $ShrinkDrive -ErrorAction SilentlyContinue
    if (-not $shr) { Fail "没有 $($ShrinkDrive): 盘（用 -ShrinkDrive 指定要压缩的卷，比如 C）" "there is no $($ShrinkDrive): (pick the volume to shrink with -ShrinkDrive, e.g. C)" }
    if ($shr.DiskNumber -ne $disk) { Fail "$($ShrinkDrive): 不在系统盘上" "$($ShrinkDrive): is not on the system disk" }
    if ((Get-Disk -Number $disk).PartitionStyle -ne 'GPT') { Fail '系统盘不是 GPT' 'the system disk is not GPT' }

    # 安装器分区只要装得下自己：按安装包里的实际内容算
    if ($LiveMiB -eq 0) {
        $content = [long]0
        foreach ($d in (Join-Path $here 'live'), (Join-Path $here 'payload')) {
            if (Test-Path $d) { $content += [long](Get-ChildItem -Recurse -File $d | Measure-Object -Property Length -Sum).Sum }
        }
        $script:LiveMiB = Get-LiveMiB $content
    }
    # 要缩的卷加了密：安装器缩不了它（只有 Windows 能）—— 没给 -AndroidGiB 的话问一次要不要现在就缩
    $tbl = $null
    try { $tbl = Get-BitLockerVolume -MountPoint "$($ShrinkDrive):" -ErrorAction Stop } catch { }
    if (Test-VolumeEncrypted $tbl) {
        Warn "$($ShrinkDrive): 加了密（BitLocker / 设备加密，状态 $($tbl.VolumeStatus)）—— 安装器缩不了加密的卷，只有 Windows 能。" "$($ShrinkDrive): is encrypted (BitLocker / device encryption, status $($tbl.VolumeStatus)) - the installer cannot shrink an encrypted volume; only Windows can."
        if (-not $script:AndroidGiBGiven -and -not $Yes) {
            do {
                $n = ConvertFrom-AndroidGiBAnswer (Read-Host (T "现在就让 Windows 从 $($ShrinkDrive): 缩出多少 GiB 给 Android？直接回车 = 64；输入 0 = 现在不缩（安装器里会告诉你怎么回来处理）" "How many GiB should Windows free on $($ShrinkDrive): for Android now? Enter = 64; 0 = not now (the installer will tell you what to do)")) 64
                if ($null -eq $n) { Warn '要 0，或者 24–2048 之间的整数' 'enter 0 or a whole number from 24 to 2048' }
            } while ($null -eq $n)
            $script:AndroidGiB = $n
        }
    }

    # ⚠️ 不在 try 里 exit：finally 不保证跑，ESP 会一直挂在那个盘符上 —— 先卸载、再判
    $esp = Mount-Esp
    try {
        $free = [System.IO.DriveInfo]::new("$esp\").AvailableFreeSpace
        $hasAndroid = Test-AndroidInstalled $esp
        $hasOurs = Test-Path "$esp\EFI\gaokun3"
    } finally { Dismount-Esp $esp }
    $freeMiB = [math]::Floor($free / 1MB)
    # 重跑时我们自己的文件已经占了一份，按"还要多少"算
    if ($freeMiB -lt $EspNeedMiB -and -not $hasOurs) {
        Fail "ESP 只剩 $freeMiB MiB，要 $EspNeedMiB MiB（Android 150 + 安装器 20）" "the ESP has only $freeMiB MiB free; $EspNeedMiB MiB is needed"
    }
    if ($hasAndroid) { Fail 'ESP 上已经有 Android 的启动项 —— 已经装过了' 'there are already Android boot entries on the ESP - already installed' }
    Say "ESP 空闲 $freeMiB MiB（要 $EspNeedMiB）" "ESP free $freeMiB MiB (need $EspNeedMiB)"

    $live = Get-Volume -FileSystemLabel $LiveLabel -ErrorAction SilentlyContinue
    $state = [ordered]@{ version = 1; shrinkDrive = $ShrinkDrive; shrunkBytes = 0; bcd = $null; fallback = [bool]$UseFallbackPath; loaderConfCreated = $false; fastStartupWas = $null }
    if (Test-Path $StateFile) {
        $old = Get-Content $StateFile -Raw | ConvertFrom-Json
        foreach ($k in 'shrunkBytes', 'bcd', 'loaderConfCreated', 'fastStartupWas') { if ($old.PSObject.Properties[$k]) { $state[$k] = $old.$k } }
    }

    # 快速启动【一律】关（U18，2026-10-05 起；原先只在"要到安装器里缩 D:"那条路上关）：它关机时让分区停在休眠状态 ——
    # 安装器为了不损坏 Windows 的数据会拒绝缩（installer-lib.sh 的 gk3__ntfs_trial_mount / why=ntfs-hibernated）；
    # 而装成双系统之后，Android 会写共用的 ESP，Windows 从快速启动恢复时可能把它写坏（§4.9.10）
    if ((Get-FastStartup) -eq 1) {
        Warn 'Windows 的“快速启动”开着：它关机时让 Windows 的分区停在休眠状态 —— 那样的分区安装器会拒绝缩；装成双系统之后它还可能写坏两个系统共用的 EFI 分区。' "Windows' Fast Startup is on: shutting down leaves Windows' partitions hibernated - the installer refuses to shrink those, and with dual boot it can corrupt the EFI partition both systems share."
        Confirm-Yes '关掉快速启动（休眠本身不受影响；装上 Android 之前 -Uninstall 会恢复，装上之后一直关着）。' 'Turn off Fast Startup (hibernation itself is unaffected; -Uninstall restores it before Android is installed; afterwards it stays off).'
        Set-FastStartup 0
        if ($null -eq $state.fastStartupWas) { $state.fastStartupWas = 1 }
        Save-State $state
        Say '快速启动已关' 'Fast Startup is off'
    }

    if ($live) {
        Say "已经有 $LiveLabel 分区（上次运行留下的）—— 不再压缩，只刷新里面的文件" "a $LiveLabel partition already exists (from an earlier run) - not shrinking again, just refreshing its files"
    } elseif ($state.shrunkBytes -gt 0) {
        # 上次缩成了、分区没建成（断电 / 报错）：【不能再缩一次】，直接在缩出来的地方建
        Say "上次已经缩过 $($ShrinkDrive):（$([math]::Round($state.shrunkBytes / 1GB, 1)) GiB）—— 不再压缩" "$($ShrinkDrive): was already shrunk last time - not shrinking again"
        $live = New-LiveVolume $disk (Get-Partition -DriveLetter $ShrinkDrive)
    } else {
        Step "2/5 压缩 $($ShrinkDrive):" "2/5 Shrinking $($ShrinkDrive):"
        $sup = Get-PartitionSupportedSize -DriveLetter $ShrinkDrive
        $want = [long]$AndroidGiB * 1GB + [long]$LiveMiB * 1MB
        $target = Get-ShrinkTarget $shr.Size $sup.SizeMin $want ([long]$WindowsKeepGiB * 1GB)
        if (-not $target) {
            $can = [math]::Floor(($shr.Size - $sup.SizeMin - [long]$WindowsKeepGiB * 1GB) / 1GB)
            Fail "$($ShrinkDrive): 缩不出 $AndroidGiB GiB + $LiveMiB MiB（最多约 $can GiB，还要给 Windows 留 $WindowsKeepGiB GiB）。可以用 -AndroidGiB 调小，或先清理 $($ShrinkDrive):" "cannot shrink $($ShrinkDrive): by that much (about $can GiB at most, keeping $WindowsKeepGiB GiB for Windows). Use a smaller -AndroidGiB"
        }
        if ($AndroidGiB -gt 0) {
            Say ("{0}: {1:N1} GiB -> {2:N1} GiB；缩出 {3} GiB 给 Android + {4} MiB 给安装器" -f $ShrinkDrive, ($shr.Size / 1GB), ($target / 1GB), $AndroidGiB, $LiveMiB) `
                ("{0}: {1:N1} GiB -> {2:N1} GiB; {3} GiB for Android + {4} MiB for the installer" -f $ShrinkDrive, ($shr.Size / 1GB), ($target / 1GB), $AndroidGiB, $LiveMiB)
        } else {
            Say ("{0}: {1:N1} GiB -> {2:N1} GiB；只缩出 {3} MiB 给安装器 —— 给 Android 的空间到安装器里再分" -f $ShrinkDrive, ($shr.Size / 1GB), ($target / 1GB), $LiveMiB) `
                ("{0}: {1:N1} GiB -> {2:N1} GiB; only {3} MiB for the installer - space for Android is decided in the installer" -f $ShrinkDrive, ($shr.Size / 1GB), ($target / 1GB), $LiveMiB)
        }
        Confirm-Yes "即将压缩 $($ShrinkDrive):（Windows 自己的“压缩卷”，不删文件），并新建分区 $LiveLabel。建议先备份重要数据。" "About to shrink $($ShrinkDrive): (Windows' own Shrink Volume; no files are deleted) and create partition $LiveLabel. Back up important data first."
        Resize-Partition -DriveLetter $ShrinkDrive -Size $target
        $state.shrunkBytes = $shr.Size - $target
        Save-State $state
        $live = New-LiveVolume $disk (Get-Partition -DriveLetter $ShrinkDrive)
    }
    $lp = $live | Get-Partition
    if (-not $lp.DriveLetter -or $lp.DriveLetter -eq [char]0) { $lp | Add-PartitionAccessPath -AssignDriveLetter; $lp = $live | Get-Partition }
    $lroot = "$($lp.DriveLetter):"

    Say "拷贝 live 系统到 $lroot" "copying the live system to $lroot"
    Copy-Item -Recurse -Force -Path (Join-Path $here 'live\*') -Destination "$lroot\"
    $payload = Join-Path $here 'payload'
    if (Test-Path (Join-Path $payload 'super.img.zst')) {
        Say '带上安装载荷（离线安装，不用联网）' 'including the install payload (offline install)'
        New-Item -ItemType Directory -Force -Path "$lroot\gaokun3\payload" | Out-Null
        Copy-Item -Force -Path (Join-Path $payload '*') -Destination "$lroot\gaokun3\payload\"
    } else {
        Say '没有安装载荷 —— 安装器会从网络下载（要先连上 WiFi）' 'no install payload - the installer will download it (needs WiFi)'
    }
    foreach ($rel in 'gaokun3\live.squashfs', 'gaokun3\initramfs.img') {
        $a = (Get-FileHash -Algorithm SHA256 (Join-Path $here "live\$rel")).Hash
        $b = (Get-FileHash -Algorithm SHA256 "$lroot\$rel").Hash
        if ($a -ne $b) { Fail "拷到 $LiveLabel 的 $rel 校验不对" "$rel on $LiveLabel does not verify" }
    }

    if ($Wifi -ne 'none') {
        # ⚠️ 不放 %TEMP%：用户名带空格时 folder="…" 会被拆开；这个目录里是【明文】密码，finally 里删
        $tmp = Join-Path $DataDir ('wlan-' + [guid]::NewGuid())
        New-Item -ItemType Directory -Force -Path $tmp | Out-Null
        try {
            & netsh.exe wlan export profile key=clear folder="$tmp" | Out-Null
            $current = @()
            if ($Wifi -eq 'current') { $current = @(Get-NetConnectionProfile -ErrorAction SilentlyContinue | ForEach-Object { $_.Name }) }
            $blocks = @()
            foreach ($f in Get-ChildItem -Path $tmp -Filter '*.xml') {
                $x = New-Object xml; $x.Load($f.FullName)   # 按文件自己声明的编码读
                $nm = $x.WLANProfile.name
                if ($Wifi -eq 'current' -and $current -notcontains $nm) { continue }
                $b = ConvertTo-WpaNetwork $x
                if ($b) { $blocks += $b } else { Warn "WiFi「$nm」的加密方式不支持（企业网 / WEP），跳过" "WiFi '$nm' uses an unsupported security type, skipped" }
            }
            if ($blocks.Count -gt 0) {
                $conf = ($blocks -join "`n`n") + "`n"
                [IO.File]::WriteAllText("$lroot\gaokun3\wpa_supplicant.conf", $conf, (New-Object Text.UTF8Encoding($false)))
                Say "带上了 $($blocks.Count) 个 WiFi（WPA2 写的是推导出的 PSK，不是明文密码）" "included $($blocks.Count) WiFi network(s) (WPA2 as a derived PSK, not the password)"
            } else {
                Say '没有可带的 WiFi —— 到安装器里再连' 'no WiFi to include - connect in the installer'
            }
        } finally { Remove-Item -Recurse -Force -ErrorAction SilentlyContinue $tmp }
    }
    # 装完就收回盘符：别让它出现在资源管理器里被误删
    $lp | Remove-PartitionAccessPath -AccessPath "$lroot\"

    Step '4/5 ESP：systemd-boot 与启动项' '4/5 ESP: systemd-boot and the boot entry'
    $esp = Mount-Esp
    try {
        New-Item -ItemType Directory -Force -Path "$esp\EFI\gaokun3", "$esp\loader\entries" | Out-Null
        Copy-Item -Force -Path (Join-Path $here 'esp\EFI\gaokun3\*') -Destination "$esp\EFI\gaokun3\"
        Copy-Item -Force -Path (Join-Path $here 'esp\loader\entries\gaokun3-live.conf') -Destination "$esp\loader\entries\"
        if (-not (Test-Path "$esp\loader\loader.conf")) {
            [IO.File]::WriteAllText("$esp\loader\loader.conf", (Format-LoaderConf), (New-Object Text.UTF8Encoding($false)))
            $state.loaderConfCreated = $true
        }
        if ($UseFallbackPath) {
            $fb = "$esp\EFI\Boot\bootaa64.efi"
            New-Item -ItemType Directory -Force -Path "$esp\EFI\Boot" | Out-Null
            if ((Test-Path $fb) -and -not (Test-Path "$fb.before-gaokun3")) { Copy-Item $fb "$fb.before-gaokun3" }
            Copy-Item -Force "$esp\EFI\gaokun3\systemd-bootaa64.efi" $fb
            Say '回落路径 \EFI\Boot\bootaa64.efi 已换成 systemd-boot（原件留 .before-gaokun3）' 'the fallback \EFI\Boot\bootaa64.efi is now systemd-boot (original kept as .before-gaokun3)'
        }
    } finally { Dismount-Esp $esp }
    Save-State $state

    Step '5/5 下一次开机进安装器' '5/5 Boot the installer next time'
    if ($UseFallbackPath) {
        Say '开机会出现 systemd-boot 菜单：默认进安装器，Windows Boot Manager 也在菜单里。' 'The systemd-boot menu will appear: the installer is the default; Windows Boot Manager is listed too.'
    } else {
        if (-not $state.bcd) {
            $out = Invoke-Bcd @('/copy', '{bootmgr}', '/d', $EntryTitle)
            $state.bcd = Get-BcdGuid $out
            if (-not $state.bcd) { Fail "bcdedit /copy 的输出里找不到 GUID：$out" "no GUID in the bcdedit /copy output: $out" }
            Save-State $state
        }
        Invoke-Bcd @('/set', $state.bcd, 'path', '\EFI\gaokun3\systemd-bootaa64.efi') | Out-Null
        Invoke-Bcd @('/set', '{fwbootmgr}', 'displayorder', $state.bcd, '/addlast') | Out-Null
        Invoke-Bcd @('/set', '{fwbootmgr}', 'bootsequence', $state.bcd) | Out-Null
        Say "只有下一次开机进安装器（固件启动项 $($state.bcd)）；默认启动项没动。" "Only the next boot goes to the installer (firmware entry $($state.bcd)); the default is unchanged."
        Warn 'bcdedit 的"只下一次"在这台机器的固件上还没验证过。重启后如果直接进了 Windows，改用 -UseFallbackPath 再运行一次。' "bcdedit's one-time boot has not been verified on this machine's firmware. If the next boot goes straight to Windows, run again with -UseFallbackPath."
    }
    if ($AndroidGiB -gt 0) {
        Say '在安装器里选"保留现有系统"。' 'In the installer choose "Keep the current system".'
    } else {
        Say "在安装器里先选“缩小现有分区腾出空间”缩 $($ShrinkDrive):，再选“保留现有系统”。" "In the installer choose ""Shrink an existing partition to make room"" for $($ShrinkDrive):, then ""Keep the current system""."
    }
    Say '不想装了：直接重启回 Windows，再运行本脚本加 -Uninstall。' 'Changed your mind: reboot into Windows and run this with -Uninstall.'
    # 伴随工具（U23）：装 Android 之后 Windows 侧的修复 / 互相重启 / 默认系统都靠它；装不上不挡安装器
    try { $null = Install-Companion }
    catch {
        if ([string]$_.Exception.Message -like 'GK3QUIT*') { throw }
        Warn "伴随工具没装上（$($_.Exception.Message)）：之后可以运行 gaokun3-setup.cmd -InstallCompanion" "the companion was not installed ($($_.Exception.Message)): run gaokun3-setup.cmd -InstallCompanion later"
    }
    if (-not $NoReboot) {
        $a = Read-Answer (T '现在重启吗？(y/N)' 'Restart now? (y/N)')
        if ($a -eq 'y' -or $a -eq 'Y') { Restart-Computer }
    }
}

# ── 分派 ─────────────────────────────────────────────────────────────────────

function Get-DefaultAction {
    # 不带参数时：旁边是免 U 盘安装包 → 安装；就在 %ProgramFiles%\gaokun3 里 → 只读体检；
    # 否则（U 盘介质上的 gaokun3-windows\，只有这两个脚本）→ 安装伴随工具
    $dir = Split-Path -Parent $SelfPath
    if ((Test-Path -LiteralPath (Join-Path $dir 'live')) -or (Test-Path -LiteralPath (Join-Path $dir 'SHA256SUMS'))) { return 'setup' }
    $same = $false
    try { $same = ((Resolve-Path -LiteralPath $dir).Path.TrimEnd('\', '/') -eq (Resolve-Path -LiteralPath $InstallDir).Path.TrimEnd('\', '/')) } catch { }
    if ($same) { return 'status' }
    return 'companion'
}

function Invoke-Main {
    $acts = @()
    if ($Uninstall) { $acts += 'uninstall' }
    if ($InstallCompanion) { $acts += 'companion' }
    if ($RepairBoot) { $acts += 'repair' }
    if ($RebootToAndroid) { $acts += 'reboot-android' }
    if ($SetDefault) { $acts += 'set-default' }
    if ($SuspendBitLocker) { $acts += 'suspend-bitlocker' }
    if ($RemoveAndroid) { $acts += 'remove-android' }
    if ($WindowsPreset) { $acts += 'windows-preset' }
    if ($BootCheck) { $acts += 'boot-check' }
    if ($Notify) { $acts += 'notify' }
    if ($Trigger) { $acts += 'trigger' }
    if ($TaskAction) { $acts += 'task' }
    if ($acts.Count -gt 1) { Fail "一次只能做一件事：$($acts -join ', ')" "one action at a time: $($acts -join ', ')" }
    if ($Check -and -not $RepairBoot -and $acts.Count -gt 0) { Fail '-Check 只能与 -RepairBoot 连用' '-Check goes with -RepairBoot only' }
    $a = 'default'; if ($acts.Count -eq 1) { $a = $acts[0] }
    if ($a -eq 'default') { $a = Get-DefaultAction }
    switch ($a) {
        'setup' { Invoke-Setup | Out-Host; return 0 }
        'uninstall' { Invoke-Uninstall | Out-Host; return 0 }
        'companion' { return (Install-Companion) }
        'status' { return (Invoke-RepairBoot $true) }
        'repair' { return (Invoke-RepairBoot ([bool]$Check)) }
        'reboot-android' { return (Invoke-RebootToAndroidCli) }
        'set-default' { return (Invoke-SetDefaultCli $SetDefault) }
        'suspend-bitlocker' { return (Invoke-SuspendBitLockerOnly) }
        'remove-android' { return (Invoke-RemoveAndroid) }
        'windows-preset' { return (Set-WindowsPresetOption $WindowsPreset) }
        'boot-check' { $null = Invoke-BootCheck; return 0 }
        'notify' { return (Invoke-Notify) }
        'trigger' { return (Invoke-Trigger $Trigger) }
        'task' { return (Invoke-TaskAction $TaskAction) }
    }
    return 1
}

# 被 dot-source 时（test-setup.ps1）只加载函数，不跑主流程
if ($MyInvocation.InvocationName -ne '.') {
    $code = 1
    try { $code = [int](@(Invoke-Main) | Select-Object -Last 1) }
    catch {
        $m = [string]$_.Exception.Message
        if ($m -like 'GK3QUIT*') { $code = 0 }
        elseif ($m -like 'GK3FAIL*') { $code = 1 }
        else { Write-Host ($_ | Out-String) -ForegroundColor Red; Write-Log "ERROR $m"; $code = 1 }
    }
    exit $code
}
