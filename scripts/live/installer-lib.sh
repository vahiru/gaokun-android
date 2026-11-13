# gaokun3 安装器后端。被两个前端共用：
#   * scripts/install-gaokun3.sh   命令行
#   * installer/live/      图形安装器（LiveCD）
#
# ★ 一套实现两个前端，是因为本仓反复吃过"两份拷贝各自漂移"的亏。
#   ⚠️ 这句话写下之后很长时间里并不成立：install-gaokun3.sh 根本没 source 这个
#      文件，两份实现各自漂了 —— 救援分区 24 GiB vs 1 GiB、loader.conf 的 default
#      一个指救援一个指 Android、cmdline 一份从 boot.img 取一份手抄且已过时。
#      2026-09-24 收拢：命令行版现在只是这个库外面的一层薄壳。
#
# 全部输出都是【面向机器的行记录】：`键=值` 一行一条，前缀标明类型。
# 人看的信息一律走 stderr；stderr 上另有两种给界面的行：
#   PROGRESS <百分比> <代码> [k=v …]   进度。代码是 [a-z][a-z0-9-]*，前端按它查 l10n；值同样百分号编码
#   ERR code=<代码> [k=v …]            失败的原因（紧接着还有一行给人看、给日志的 `!! <说明>`）
# ★ 2026-10-05 起（v1.0 计划 INST-10）后端【不再】给界面送中文：英文界面里原先夹着这里写死的中文进度与报错。
#   `!! …` 与普通日志仍是中文 —— 它们给日志、给命令行版看；界面只把它们放进"详情"。
#   进度代码与 ERR 代码的全集 = installer/live/lib/ui/messages.dart 里的两个 switch（新加一个要两边一起加）。
#   ERR 为什么也走 stderr：`x=$(gk3__…)` 会把 stdout 吞掉，报错跟着丢；stderr 不会。
# stdout 上另有一种记录：`NOTE code=<代码> [k=v …]` —— gk3_apply 成功了，但有件事要在完成页告诉用户
#   （2026-10-05 S10：loadervar-stuck name= value= = EFI 变量删不掉）。前端 session.notes 收集、DonePage 按代码说。
#

# 值里不会有空格：自由文本字段（分区名、卷标、磁盘型号、SSID）一律经
#   gk3__enc 做百分号编码（% → %25，空格 → %20，制表符 → %09）。
#   原因：Windows 建的分区 PARTLABEL 全是 "Basic data partition"
#   （见 docs/dev/hardware.md），而 PART 记录里 name= 和 fslabel=
#   两个自由文本字段都夹在中间 —— "最后一个键吃到行尾"救不了两个。
#   ⇒ 解析规则就一条：按空格切，每个值再做 %-解码。
#
# ⚠️ 需要 bash（PIPESTATUS、数组）。前端一律 `bash -c '. installer-lib.sh && …'`。
#
#   . installer-lib.sh
#   gk3_preflight                  # 这台机器能不能装（型号 / 电源）
#   gk3_probe                      # 列出磁盘 / 分区 / 空闲区
#   gk3_esp_info <分区>            # 现有 ESP 的剩余空间、上面有没有 Windows、BitLocker / 休眠、统一启动入口（双系统之前问）
#   gk3_release_info [目录]        # 发布目录 / 安装 U 盘里带了什么（镜像、救援系统、版本）
#   gk3_plan  <参数…>              # 算出分区方案（纯计算，不碰磁盘）
#   gk3_apply <参数…>              # 执行（唯一会写盘的函数）
#   gk3_job_run <函数> <参数…>     # 同上，但放进独立的 systemd 单元跑：界面崩了它照样写完（GUI-11，见文件末尾）
#   gk3_save_logs [分区|目录]      # 把日志另存一份到用户拿得到的地方（GUI-12，见文件末尾）

if [ -z "${BASH_VERSION:-}" ]; then
    echo "!! installer-lib.sh 需要 bash" >&2
    return 1 2>/dev/null || exit 1
fi

# ── 布局常量 ────────────────────────────────────────────────────────────────
# 说明见 install-gaokun3.sh 顶部那段（为什么 esp 必须叫 esp、
# 为什么 misc 挂载点必须是 /misc 等等）。
GK3_ESP_MIB=300
GK3_MISC_MIB=4
GK3_METADATA_MIB=32
GK3_SUPER_MIB=12288
GK3_BOOT_MIB=64          # 每个槽位；与 BoardConfig 的 BOARD_BOOTIMAGE_PARTITION_SIZE 对齐
# ★ 救援系统从 24 GiB 降到 1 GiB —— Stage 7 之后它是 55 MiB 的 squashfs，
#   不再是一整套装在盘上的 Ubuntu。1 GiB 给日后换更大的镜像留足余量。
GK3_RESCUE_MIB=1024
GK3_USERDATA_MIN_MIB=8192

# 双系统安装时，ESP 里至少要能放下我们的两个槽位（内核+ramdisk+dtb ×2）
GK3_ESP_NEED_MIB=150
# 重新安装时 ESP 上我们的文件是【覆盖】不是新增：只给内核 / ramdisk 变大留余量
GK3_ESP_REINSTALL_NEED_MIB=16
# OTA postinstall 的门槛：ESP 空闲 + 目标槽目录里将被覆盖的旧文件 > 56 MiB
# （device/huawei/gaokun3/bin/gaokun3-ota-postinstall.sh:93-96）。装完之后要还能 OTA
GK3_ESP_OTA_NEED_KIB=57344
# 双系统（ESP 上有 EFI/Microsoft/Boot/bootmgfw.efi）共用 ESP 的余量（见 tools/gk3boot/README.md）：
#   写完、并扣掉下一次 OTA 的暂时空间（一个槽的 Image + ramdisk + dtb，再加一版统一启动入口）之后，
#   还要给 Windows 和固件留 ≥ 32 MiB —— 功能更新要系统分区空闲 15 MB、质量更新 13 MB（微软 KB 3086249），
#   固件胶囊也暂存在 ESP 的 EFI/UpdateCapsule。postinstall / gk3-esp-sync 要用同一个数。
GK3_ESP_WIN_RESERVE_KIB=32768
# 一版统一启动入口（EFI/gk3boot/<ver>/ 下 gk3boot.efi + fastboot.img）的估计
GK3_ESP_GK3BOOT_KIB=5120
# 比这小的 ESP（用户自己重装 Windows 得到的 100 MiB）在 1.0 里明确不装。
# 200：出厂 300 MiB 与 Windows 在 4Kn 盘上建的 260 MiB 都过，100 MiB 那一档不过
GK3_ESP_MIN_SIZE_MIB=200
# loader.conf 的菜单等待（秒）。纯 Android 与双系统统一显示菜单 5 秒，不写 menu-hidden。
#   OTA postinstall 只把【早先版本写的 15】改成 5，用户改过的不动
GK3_LOADER_TIMEOUT=5
GK3_LOADER_TIMEOUT_DUAL=5
# systemd-boot 的厂商 GUID（refs/systemd-v257/src/boot/efivars.h:12，与 scripts/boot-oneshot.sh、
# overlay-common/usr/bin/gk3-boot-android 相同）。LoaderEntryDefault / LoaderEntryOneShot 都在它下面
GK3_LOADER_GUID=4a67b082-0a4c-41cf-b6c7-440b29bb8c4f

# 这个库所在的目录。gk3-unsparse.py / gk3-bootimg.py 跟它放在一起
# （仓库里是 scripts/live/，live 镜像里是 /usr/share/gaokun3/）。
GK3_LIBDIR=${GK3_LIBDIR:-$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)}
# 运行时的临时目录（写盘任务的状态与输出等）；测试换成临时目录
GK3_RUNDIR=${GK3_RUNDIR:-/run/gaokun3}

gk3_log()  { echo "$*" >&2; }
gk3_die()  { echo "!! $*" >&2; return 1; }

# 进度：gk3_prog <百分比> <代码> [k=v …]（值在这里编码）
gk3_prog() {
    local out="PROGRESS $1 $2" kv; shift 2
    for kv in "$@"; do out="$out ${kv%%=*}=$(gk3__enc "${kv#*=}")"; done
    echo "$out" >&2
}

# 失败：gk3_fail <代码> [k=v …] -- <给人看的说明…>
#   stderr 上先出 `ERR code=<代码> k=v…`（给界面），再出 `!! <说明>`（给日志与命令行版）。返回 1。
# ★ 在 gk3_apply 里面（看调用栈，不看变量 —— 同一个 shell 里先后调过它，变量会留着）
#   自动带上 touched=yes|no：目标盘动过没有（GK3__TOUCHED，由 gk3_apply 维护）。
#   界面据此决定失败页说"盘没动过，可以返回 / 重试"还是"盘可能写了一半"；命令行版在失败时照着说一句。
#   调用方自己给了 touched=（例如 gk3__table_guard 在任何入口里都是"没动过"）就不再追加。
gk3_fail() {
    local out="ERR code=$1" kv; shift
    while [ $# -gt 0 ] && [ "$1" != -- ]; do
        kv=$1; shift
        out="$out ${kv%%=*}=$(gk3__enc "${kv#*=}")"
    done
    [ "${1:-}" = -- ] && shift
    case "$out " in
        *" touched="*) ;;
        *) case " ${FUNCNAME[*]} " in *" gk3_apply "*) out="$out touched=${GK3__TOUCHED:-no}" ;; esac ;;
    esac
    echo "$out" >&2
    gk3_die "$@"
}

# 协议的百分号编码（见文件头）。
gk3__enc() { printf '%s' "$1" | sed -e 's/%/%25/g' -e 's/ /%20/g' -e "s/$(printf '\t')/%09/g"; }

# 安装介质（/media/gk3，见 initramfs-init:119-120）所在的整块盘；不是从介质启动就是空串。
gk3__medium_disk() {
    local dev pk
    dev=$(findmnt -no SOURCE /media/gk3 2>/dev/null || echo "")
    [ -n "$dev" ] || return 0
    pk=$(lsblk -no PKNAME "$dev" 2>/dev/null | head -1)
    [ -n "$pk" ] && echo "/dev/$pk"
    return 0
}

# ── 探测 ────────────────────────────────────────────────────────────────────
# 输出：
#   DISK path=/dev/nvme0n1 size_mib=488386 model=... removable=0 tran=nvme|usb medium=no|yes table=gpt|mbr|unreadable|unknown
#   （table=：gk3__read_table 的结论。mbr ⇒ 界面只给整盘清空；unreadable ⇒ 这块盘不给选，后面不跟 PART / FREE，
#     stderr 上另有 `ERR code=disk-unreadable disk=… touched=no` + `!!`，gk3_probe 最后返回 1 —— 别的盘照常列出）
#   PART path=/dev/nvme0n1p1 num=1 start=2048 end=616447 size_mib=300 \
#        type=<GUID> name=esp fs=vfat fslabel=... os=windows|linux|android| medium=no|yes
#   （PART 的 medium=yes：安装器就是从这个分区跑起来的 —— 挂在 /media/gk3 的那个）
#   FREE disk=/dev/nvme0n1 start=616448 end=… size_mib=…
#   （model / name / fslabel 已百分号编码）
gk3_probe() {
    local d rc=0
    for d in /sys/block/*; do
        local name; name=$(basename "$d")
        # ⚠️ 正常情况下跳过 loop —— 没人往 loop 设备装系统。但测试需要它，
        #    所以给一个显式开关：GK3_ALLOW_LOOP=1。
        #    （否则测试没法用 gk3_probe 验"空间释放出来了没有"，
        #     只能另写一套算法，而那就等于测了两份不同的逻辑。）
        case "$name" in
            loop*)  [ "${GK3_ALLOW_LOOP:-0}" = 1 ] || continue ;;
            ram*|zram*|dm-*|sr*|md*) continue ;;
        esac
        [ -e "/dev/$name" ] || continue
        local sectors; sectors=$(cat "$d/size" 2>/dev/null || echo 0)
        [ "$sectors" -gt 0 ] || continue
        # 512 字节扇区 → MiB
        local size_mib=$(( sectors / 2048 ))
        [ "$size_mib" -ge 1024 ] || continue      # 小于 1 GiB 的不当安装目标
        local model removable tran medium=no
        # 型号去掉首尾空白（NVMe 的 model 属性右侧补空格补到 40 字节），中间的空格编码保留。
        # 原先 tr -d ' ' 会把 "SAMSUNG MZ9L…" 挤成 "SAMSUNGMZ9L…"。
        model=$(head -1 "$d/device/model" 2>/dev/null | sed -e 's/^[[:space:]]*//' -e 's/[[:space:]]*$//')
        removable=$(cat "$d/removable" 2>/dev/null || echo 0)
        # tran=usb：界面据此标"外接盘"—— USB 硬盘盒的 removable 常常是 0，靠它认不出来
        tran=$(lsblk -dno TRAN "/dev/$name" 2>/dev/null | head -1 | tr -d ' ')
        # medium=yes：安装器就是从这块盘启动的。gk3_apply 的安全闸 1 会拒绝整盘清空它；
        # 界面应当【一开始】就把它标出来，而不是等用户选完、确认完再报错。
        [ "/dev/$name" = "$(gk3__medium_disk)" ] && medium=yes
        gk3__read_table "/dev/$name"
        echo "DISK path=/dev/$name size_mib=$size_mib model=$(gk3__enc "${model:-?}") removable=$removable tran=${tran:-?} medium=$medium table=$GK3__PT"
        gk3__probe_parts "/dev/$name" "$sectors" have-table || rc=1
    done
    return $rc
}

# ── 分区表是什么、读不读得出来（见 docs/dev/installer-rust-design.md）─────────
# gk3__read_table <盘>：跑一次 sgdisk -p，结论放进全局变量（不用 $(…)：调用方还要用 sgdisk 的输出）——
#   GK3__PT     = gpt | mbr | unreadable | unknown（没有 sgdisk）
#   GK3__PT_P   = sgdisk -p 的 stdout
#   GK3__PT_WHY = unreadable 的原因（给人看的，只进 `!!`）
# 判据（gdisk 1.0.10 / util-linux 2.41.5，在 test-env 容器里对各种盘实录）：
#   unreadable：sgdisk 退出码非零（打不开：rc=2 "Problem opening … for reading!"）；或者 stderr 上有读错误
#     （"Warning! Read error 5; strange behavior now likely!" / "Error 5 reading partition table for CRC check!"）；
#     或者 stdout 里没有 "First usable sector is N, last usable sector is M"。
#     整块盘读都 EIO 时（dm-error 实测）sgdisk 的退出码是【0】，stdout 是一张 "Creating new GPT entries in memory."
#       的空表 —— 只看退出码 / 只看 stdout 都会把它当成一块空盘。读错误只在 stderr 上。
#     只在 stderr 里找读错误：stdout 里有分区名，名字叫 "read error" 的分区不该把整块盘判成读不出。
#   ★ mbr：blkid -p 说 PTTYPE=dos，或者 sgdisk 在行首说 "Found invalid GPT and valid MBR; converting MBR to GPT format"
#     （这句在 stdout 上）。原来的判据找 "MBR only" —— gdisk 1.0.10 从来不这么说，于是 MBR 检查从不触发（S2）。
#     两样都认：blkid 是独立于 sgdisk 的一份判断；sgdisk 那句说的是它此刻手里拿的已经不是盘上那张表了。
#     混合 MBR（sgdisk -h）与坏了主 GPT 头的盘：blkid 说 gpt、sgdisk 照 GPT 读 ⇒ 不算 MBR。只剩保护性 MBR 的盘：
#     blkid 说 PMBR、sgdisk 当空盘 ⇒ 也不算（没有可丢的布局）。
#   unreadable 优先于 mbr：读不出来的盘上，"它是 MBR"这个结论也不可信。
gk3__read_table() {
    local disk=$1 ef err rc
    GK3__PT_P=""; GK3__PT_WHY=""
    command -v sgdisk >/dev/null || { GK3__PT=unknown; GK3__PT_WHY="缺 sgdisk"; return 0; }
    ef=$(mktemp)
    GK3__PT_P=$(sgdisk -p "$disk" 2>"$ef" </dev/null); rc=$?
    err=$(cat "$ef"); rm -f "$ef"
    if [ "$rc" != 0 ]; then
        GK3__PT=unreadable; GK3__PT_WHY="sgdisk -p 退出码 ${rc}：$(printf '%s\n' "$err" | head -1)"
    elif printf '%s\n' "$err" | grep -Eqi 'read error|error [0-9]+ reading'; then
        GK3__PT=unreadable; GK3__PT_WHY="读盘出错：$(printf '%s\n' "$err" | grep -Ei -m1 'read error|error [0-9]+ reading')"
    elif ! printf '%s\n' "$GK3__PT_P" | grep -Eq '^First usable sector is [0-9]+, last usable sector is [0-9]+'; then
        GK3__PT=unreadable; GK3__PT_WHY="sgdisk -p 的输出里没有可用扇区的范围"
    elif [ "$(blkid -p -o value -s PTTYPE "$disk" 2>/dev/null </dev/null)" = dos ] \
         || printf '%s\n%s\n' "$GK3__PT_P" "$err" | grep -q '^Found invalid GPT and valid MBR; converting MBR to GPT format'; then
        GK3__PT=mbr
    else
        GK3__PT=gpt
    fi
    return 0
}

# gk3__table_guard <盘> [mbr-ok]：写盘入口在动盘之前调。读不出 ⇒ ERR disk-unreadable；MBR ⇒ ERR mbr-disk
#   （整盘清空本来就要抹掉分区表，传 mbr-ok）。两种都带 touched=no：这一步之前什么都没写。
#   ★ MBR 盘上为什么必须在【入口】拦（2026-10-06 容器实录，gdisk 1.0.10）：sgdisk 读 MBR 盘时在内存里转成 GPT，
#     写的时候不带 -g 就拒绝保存 —— `sgdisk -d / -n / -t / -c` 一律 rc=3 "Non-GPT disk; not saving changes. Use -g to override."，
#     盘上的 MBR 不变（带 -g 才真的转成 GPT）。所以不拦的话不是"静默转表"，而是写表那一步【必然失败】，而失败之前的步骤已经做了：
#     apply 已经标了 touched=yes —— 报出来的是 gpt-rewrite / cmd-failed
#     （"盘可能写了一半"），不是"这是 MBR 盘"。整盘清空的 `sgdisk --zap-all` 在 MBR 盘上照常成功（rc 0，之后 PTTYPE 为空）。
#   没有 sgdisk（unknown）不在这里拦：各入口自己的工具检查会报 tool-missing。
gk3__table_guard() {
    gk3__read_table "$1"
    case "$GK3__PT" in
        unreadable)
            gk3_fail disk-unreadable "disk=$1" touched=no -- "读不出 $1 的分区表（${GK3__PT_WHY}）—— 不动它（盘没动过）"; return 1 ;;
        mbr)
            [ "${2:-}" = mbr-ok ] && return 0
            gk3_fail mbr-disk "disk=$1" touched=no -- "$1 是 MBR 分区表：我们只改 GPT 分区表（转成 GPT 会丢掉现有布局，sgdisk 不带 -g 也拒绝写）—— 不动它。要用这块盘只能整盘清空（盘没动过）"; return 1 ;;
    esac
    return 0
}

# ⚠️ 用 sgdisk 而不是 lsblk：我们要的是【分区表层面】的起止扇区和类型 GUID，
#    而且要能算出空闲区间 —— lsblk 不报空闲区间。
#   gk3__probe_parts <盘> <总扇区> [have-table]   have-table：调用方刚对这块盘跑过 gk3__read_table，不再读一遍
# ⚠️★ S1（2026-10-06 修）：原来 sgdisk -p 读失败时可用区间落到默认值（2048 / 总扇区−2048）、表行为空，
#   于是【整块盘报成一段空闲】—— 界面会把一块有数据、只是读不出的盘当空盘给人选。现在读不出就明确报错、不报空闲。
#   （总扇区这个参数因此不再用到，留着是为了调用方不用改）
gk3__probe_parts() {
    local disk=$1
    [ "${3:-}" = have-table ] || gk3__read_table "$disk"
    case "$GK3__PT" in
        unknown) gk3_log "缺 sgdisk，跳过 $disk 的分区探测"; return 0 ;;
        unreadable) gk3_fail disk-unreadable "disk=$disk" touched=no -- "读不出 $disk 的分区表（${GK3__PT_WHY}）—— 不列它的分区与空闲区"; return 1 ;;
    esac

    local first_usable last_usable
    first_usable=$(printf '%s\n' "$GK3__PT_P" | sed -n 's/^First usable sector is \([0-9]*\).*/\1/p')
    last_usable=$(printf '%s\n' "$GK3__PT_P" | sed -n 's/.*last usable sector is \([0-9]*\).*/\1/p')

    # 收集分区，按起始扇区排序
    local tmp; tmp=$(mktemp)
    printf '%s\n' "$GK3__PT_P" | awk '/^ *[0-9]+ /{print $1" "$2" "$3}' | sort -k2 -n > "$tmp"

    local cursor=$first_usable num start end medium_part
    medium_part=$(findmnt -no SOURCE /media/gk3 2>/dev/null || echo "")
    while read -r num start end; do
        [ -n "$num" ] || continue
        if [ "$start" -gt "$cursor" ]; then
            gk3__emit_free "$disk" "$cursor" $(( start - 1 ))
        fi
        local part; part=$(gk3_partpath "$disk" "$num")
        local ptype pname fstype fslabel
        ptype=$(sgdisk -i "$num" "$disk" 2>/dev/null | sed -n 's/^Partition GUID code: \([0-9A-Fa-f-]*\).*/\1/p')
        pname=$(sgdisk -i "$num" "$disk" 2>/dev/null | grep "^Partition name:" | cut -d"'" -f2)
        fstype=$(blkid -o value -s TYPE "$part" 2>/dev/null || echo "")
        fslabel=$(blkid -o value -s LABEL "$part" 2>/dev/null || echo "")
        # 也报 KiB：本机 misc 只有 1007 KiB（GPT 头之后那段闲置空间），
        # 只报 MiB 会显示成 "0 MiB"，界面上看着像个空分区。
        # 实测发现的 —— 合成数据里没有这种小分区。
        echo "PART path=$part num=$num start=$start end=$end" \
             "size_mib=$(( (end - start + 1) / 2048 )) size_kib=$(( (end - start + 1) / 2 ))" \
             "type=${ptype:-?} name=$(gk3__enc "$pname") fs=${fstype:-} fslabel=$(gk3__enc "$fslabel")" \
             "os=$(gk3__guess_os "$part" "$ptype" "$pname" "$fstype")" \
             "medium=$([ -n "$medium_part" ] && [ "$(readlink -f "$medium_part")" = "$(readlink -f "$part")" ] && echo yes || echo no)"
        cursor=$(( end + 1 ))
    done < "$tmp"
    rm -f "$tmp"

    if [ "$cursor" -lt "$last_usable" ]; then
        gk3__emit_free "$disk" "$cursor" "$last_usable"
    fi
}

gk3__emit_free() {
    local disk=$1 start=$2 end=$3
    local mib=$(( (end - start + 1) / 2048 ))
    # 小于 16 MiB 的碎片没有意义，不报（GPT 对齐留下的缝隙）
    [ "$mib" -ge 16 ] || return 0
    echo "FREE disk=$disk start=$start end=$end size_mib=$mib"
}

# 认出分区上大概是什么系统 —— 只用于界面提示，不参与任何判断逻辑
gk3__guess_os() {
    local part=$1 ptype=$2 pname=$3 fstype=$4
    case "$pname" in
        esp) echo "esp"; return ;;
        super|userdata|metadata|misc|boot_a|boot_b) echo "android"; return ;;
    esac
    case "$ptype" in
        C12A7328-F81F-11D2-BA4B-00A0C93EC93B) echo "esp"; return ;;
        DE94BBA4-06D1-4D40-A16A-BFD50179D6AC) echo "winre"; return ;;
        E3C9E316-0B5C-4DB8-817D-F92DF00215AE) echo "msr"; return ;;
    esac
    case "$fstype" in
        ntfs) echo "windows" ;;
        ext4|ext3|btrfs|xfs) echo "linux" ;;
        f2fs) echo "android" ;;
        vfat) echo "fat" ;;
        crypto_LUKS) echo "luks" ;;
        *) echo "" ;;
    esac
}

# nvme 是 p1，sd 是 1
gk3_partpath() {
    case "$1" in
        *[0-9]) echo "$1p$2" ;;
        *)      echo "$1$2" ;;
    esac
}

# ── 预检 ────────────────────────────────────────────────────────────────────
# 这台机器能不能装。一项一行：
#   CHECK id=model ok=yes|no|unknown value=GK-W7X
#   CHECK id=power ok=yes|no|unknown value=<电量%> ac=yes|no min=<门槛>    （见下面 gk3__power_check）
# ok=no 的项前端必须拦住；unknown 只警告 —— 读不到 ≠ 不合格（例如内核没开 DMIID）。
# root 权限、UEFI 引导、Secure Boot、工具齐全不再检查：能启动到 LiveCD 并运行安装器，这几项就必然满足；
# 缺工具时各写盘入口自己会报 tool-missing。
#
# 型号的读取点：/sys/class/dmi/id/product_name（drivers/firmware/dmi-id.c:42-47，要 CONFIG_DMIID，Kconfig 默认 y），
#   读不到时退回内核启动日志里那行 "Hardware name: HUAWEI GK-W7X/GK-W7X-PCB, BIOS 2.16 …"
#   （本机实测原文见 docs/dev/hardware.md）。GK3_SKIP_MODEL_CHECK=1 放行（gaokun2 是另一台机器、另一套 EC 协议，别装）。
gk3_preflight() {
    local v
    v=$(cat /sys/class/dmi/id/product_name 2>/dev/null)
    [ -n "$v" ] || v=$(dmesg 2>/dev/null | grep -m1 'Hardware name:' | sed -n 's/.*Hardware name: [^ ]* \([^/,]*\).*/\1/p')
    if [ "${GK3_SKIP_MODEL_CHECK:-0}" = 1 ]; then
        echo "CHECK id=model ok=unknown value=$(gk3__enc "${v:-?}") skipped=yes"
    else case "$v" in
        GK-W7X) echo "CHECK id=model ok=yes value=$v" ;;
        "")     echo "CHECK id=model ok=unknown value=" ;;
        *)      echo "CHECK id=model ok=no value=$(gk3__enc "$v")" ;;
    esac; fi
    gk3__power_check
    return 0
}

# ★ 电量（v1.0 计划 GUI-8）：缩 NTFS、写 12 GiB 的 super 要几分钟到十几分钟，半路没电就是一块半装的盘。
#   电量低于 GK3_POWER_MIN_PCT（默认 15）而且没接电源 ⇒ ok=no，拒绝开始。
#   CHECK id=power ok=yes|no|unknown value=<电量%> ac=yes|no min=<门槛>
#   名字取自本机的 EC 驱动：电池 gaokun-ec-battery、适配器 gaokun-ec-adapter（POWER_SUPPLY_TYPE_USB，属性 online）——
#   refs/gaokun-buildbot/drivers/gaokun-ec/huawei-gaokun-battery.c:435、:174-175、:155-156；Android 下的实机路径
#   见 docs/dev/hardware.md、scripts/perf/standby-sampler.sh:30-31。没有这两个名字就按 type 找（驱动改名 / 别的机器）。
#   找不到电池 ⇒ ok=unknown（不拦）。live 镜像的内核里这个驱动在不在、名字是不是这两个，尚未在 live 里实测。
gk3__power_check() {
    local ps=${GK3_POWER_SUPPLY_DIR:-/sys/class/power_supply} min=${GK3_POWER_MIN_PCT:-15}
    local bat="" ac=no cap="" d t
    if [ -r "$ps/gaokun-ec-battery/capacity" ]; then bat=$ps/gaokun-ec-battery
    else
        for d in "$ps"/*; do
            [ "$(cat "$d/type" 2>/dev/null)" = Battery ] && [ -r "$d/capacity" ] && { bat=$d; break; }
        done
    fi
    # 接着电源：任何一个不是电池的 power_supply 报 online=1（适配器、UCSI 的 source-psy 都算），或者电池自己说在充电
    for d in "$ps"/*; do
        t=$(cat "$d/type" 2>/dev/null)
        [ -n "$t" ] && [ "$t" != Battery ] && [ "$(cat "$d/online" 2>/dev/null)" = 1 ] && { ac=yes; break; }
    done
    [ -n "$bat" ] && [ "$(cat "$bat/status" 2>/dev/null)" = Charging ] && ac=yes
    [ -n "$bat" ] && cap=$(cat "$bat/capacity" 2>/dev/null)
    case "$cap" in
        ''|*[!0-9]*) echo "CHECK id=power ok=unknown value= ac=$ac min=$min" ;;
        *) if [ "$cap" -ge "$min" ] || [ "$ac" = yes ]; then echo "CHECK id=power ok=yes value=$cap ac=$ac min=$min"
           else echo "CHECK id=power ok=no value=$cap ac=$ac min=$min"; fi ;;
    esac
}

# ── 方案计算 ────────────────────────────────────────────────────────────────
# 纯计算，不碰磁盘 —— 所以可以在任何机器上跑、可以单元测。
#
#   gk3_plan --disk /dev/nvme0n1 --mode wipe|alongside|reinstall --rescue yes|no \
#            [--region-start S --region-end E] [--esp PATH] [--userdata-mib N] [--keep-data yes|no]
#
# 输出（顺序即执行顺序）：
#   PLAN op=wipe    disk=...
#   PLAN op=mkpart  num=0 name=super start=... end=... type=... size_mib=...
#   PLAN op=useesp  path=/dev/nvme0n1p1
#   PLAN op=reuse   name=super path=/dev/nvme0n1p6 num=6 start=... end=... size_mib=... action=write|format|keep
#                   （只有 reinstall：不改分区表，逐个复用盘上现有的分区）
#   PLANSUM total_mib=... userdata_mib=... rescue=yes|no mode=... [keep_data=yes|no]
# 失败：
#   PLANERR msg=...
#
# ⚠️ 所有分区起始都对齐到 1 MiB（2048 扇区）。不对齐会让 NVMe 的写放大变差，
#    而且 sgdisk 会自己挪，挪完之后我们算出来的 end 就对不上了。
GK3_CUR=0
GK3_TYPE_ESP=ef00
GK3_TYPE_DATA=8300

gk3_plan() {
    local disk="" mode="wipe" rescue="no" rstart="" rend="" esp="" ud_mib="" keep="no"
    while [ $# -gt 0 ]; do
        case "$1" in
            --disk) disk=$2; shift 2 ;;
            --keep-data) keep=$2; shift 2 ;;
            --mode) mode=$2; shift 2 ;;
            --rescue) rescue=$2; shift 2 ;;
            --region-start) rstart=$2; shift 2 ;;
            --region-end) rend=$2; shift 2 ;;
            --esp) esp=$2; shift 2 ;;
            --userdata-mib) ud_mib=$2; shift 2 ;;
            --disk-size-mib) GK3_FAKE_DISK_MIB=$2; shift 2 ;;   # 只给自测用
            *) echo "PLANERR msg=unknown-arg:$1"; return 1 ;;
        esac
    done
    [ -n "$disk" ] || { echo "PLANERR msg=no-disk"; return 1; }

    # ⚠️★ ⑤ MBR 盘。我们只会写 GPT；MBR 盘上的写表一定失败、而且失败在别的步骤已经动了盘之后（gk3__table_guard 的注释；
    #   这里原来写的"sgdisk 会把 MBR 盘静默转成 GPT"不对：不带 -g 它拒绝写，2026-10-06 实录）。
    #   老 Windows 装机大多是 MBR，所以这不是理论风险。整盘清空不拦（本来就要抹掉分区表 —— 这是 MBR 盘唯一能走的路）。
    #   ⚠️ 2026-10-06 之前这里找的是 "MBR only"，gdisk 1.0.10 从来不输出它 ⇒ 这道检查从没触发过（设计稿 S2），
    #   MBR 盘照样算出双系统方案；判据见 gk3__read_table。也挪到了重新安装的分支之前（原来重新安装绕过它）。
    #   读不出的盘（unreadable）这里不拦：方案是纯计算（自测拿不存在的 /dev/nvme0n1 算），读不出由 gk3_probe 报、
    #   由 gk3_apply 在动盘前拦（gk3__table_guard）。
    if [ "$mode" != wipe ]; then
        gk3__read_table "$disk"
        if [ "$GK3__PT" = mbr ]; then
            echo "PLANERR msg=mbr-disk"
            return 1
        fi
    fi

    if [ "$mode" = reinstall ]; then
        gk3__plan_reinstall "$disk" "$rescue" "$keep" "$esp"; return $?
    fi

    # ⚠️★ ① 分区名查重。Android 的 first-stage mount 走 by-name/super，那是
    #   ueventd 按 PARTLABEL 建的符号链接 —— 重名时哪个赢【不确定】。在一块
    #   已经装过我们系统的盘上跑 alongside，就会建出第二个 super、第二个
    #   userdata，然后开机随机挂错分区。整盘模式不用查（分区表会被清空）。
    #   ⚠️ 取名字用 cut 不用 sed 反捕获 —— 第一版用了 sed 的反向引用，那个
    #      反斜杠在写文件的路上被吃成 0x01 控制字符，于是守卫看着在那儿
    #      却从不触发。实机验出来的。
    if [ "$mode" != wipe ] && command -v sgdisk >/dev/null; then
        local dup="" nm n
        for n in $(sgdisk -p "$disk" 2>/dev/null | awk '/^ *[0-9]+ /{print $1}'); do
            nm=$(sgdisk -i "$n" "$disk" 2>/dev/null | grep "^Partition name:" | cut -d"'" -f2)
            [ "$nm" = esp ] && continue
            case " misc metadata boot_a boot_b super userdata gk3rescue " in
                *" $nm "*) dup="$dup $nm" ;;
            esac
        done
        if [ -n "$dup" ]; then
            echo "PLANERR msg=partlabel-conflict names=$(echo $dup | tr " " ",")"
            return 1
        fi
    fi

    local cur last
    if [ "$mode" = wipe ]; then
        local total_mib=${GK3_FAKE_DISK_MIB:-}
        if [ -z "$total_mib" ]; then
            local sectors; sectors=$(cat "/sys/block/$(basename "$disk")/size" 2>/dev/null || echo 0)
            total_mib=$(( sectors / 2048 ))
        fi
        [ "$total_mib" -gt 0 ] || { echo "PLANERR msg=cannot-size-disk"; return 1; }
        cur=2048
        last=$(( total_mib * 2048 - 2048 ))       # 尾部给备份 GPT 留 1 MiB
        echo "PLAN op=wipe disk=$disk"
    else
        [ -n "$rstart" ] && [ -n "$rend" ] || { echo "PLANERR msg=alongside-needs-region"; return 1; }
        # 起点向上对齐到 1 MiB
        cur=$(( (rstart + 2047) / 2048 * 2048 ))
        last=$rend
        if [ -z "$esp" ]; then
            echo "PLANERR msg=alongside-needs-existing-esp"; return 1
        fi
        echo "PLAN op=useesp path=$esp need_mib=$GK3_ESP_NEED_MIB"
    fi

    local avail_mib=$(( (last - cur + 1) / 2048 ))

    # 固定开销
    local fixed=$(( GK3_MISC_MIB + GK3_METADATA_MIB + GK3_BOOT_MIB * 2 + GK3_SUPER_MIB ))
    [ "$mode" = wipe ] && fixed=$(( fixed + GK3_ESP_MIB ))
    [ "$rescue" = yes ] && fixed=$(( fixed + GK3_RESCUE_MIB ))

    local need=$(( fixed + GK3_USERDATA_MIN_MIB ))
    if [ "$avail_mib" -lt "$need" ]; then
        # fixed_mib：除 /data 之外的固定开销，界面需要时直接用，不在 Dart 里再算一遍
        echo "PLANERR msg=not-enough-space avail_mib=$avail_mib need_mib=$need fixed_mib=$fixed"
        return 1
    fi

    # userdata 吃掉剩下的（除非调用方指定）
    local userdata_mib=$(( avail_mib - fixed ))
    if [ -n "$ud_mib" ]; then
        [ "$ud_mib" -ge "$GK3_USERDATA_MIN_MIB" ] || { echo "PLANERR msg=userdata-too-small min_mib=$GK3_USERDATA_MIN_MIB"; return 1; }
        [ "$ud_mib" -le "$userdata_mib" ] || { echo "PLANERR msg=userdata-too-big max_mib=$userdata_mib"; return 1; }
        userdata_mib=$ud_mib
    fi

    # ⚠️ 顺序即磁盘顺序，userdata 必须最后 —— 这样以后扩容不用挪任何东西。
    #    （本仓 M6 扩 /data 时正是靠这一点。）
    GK3_CUR=$cur
    if [ "$mode" = wipe ]; then
        gk3__emit_part "$disk" esp      "$GK3_ESP_MIB"      "$GK3_TYPE_ESP"
    fi
    gk3__emit_part "$disk" misc     "$GK3_MISC_MIB"     "$GK3_TYPE_DATA"
    gk3__emit_part "$disk" metadata "$GK3_METADATA_MIB" "$GK3_TYPE_DATA"
    gk3__emit_part "$disk" boot_a   "$GK3_BOOT_MIB"     "$GK3_TYPE_DATA"
    gk3__emit_part "$disk" boot_b   "$GK3_BOOT_MIB"     "$GK3_TYPE_DATA"
    gk3__emit_part "$disk" super    "$GK3_SUPER_MIB"    "$GK3_TYPE_DATA"
    if [ "$rescue" = yes ]; then
        gk3__emit_part "$disk" gk3rescue "$GK3_RESCUE_MIB" "$GK3_TYPE_DATA"
    fi
    gk3__emit_part "$disk" userdata "$userdata_mib"     "$GK3_TYPE_DATA"

    # ★ 最后一条不能越过可用区尾部。fixed+userdata 的算术上面已经保证了，
    #   但这里【再独立验一次】—— 算错的代价是写到别人的分区上。
    if [ "$(( GK3_CUR - 1 ))" -gt "$last" ]; then
        echo "PLANERR msg=plan-overruns-region end=$(( GK3_CUR - 1 )) limit=$last"
        return 1
    fi

    echo "PLANSUM mode=$mode rescue=$rescue avail_mib=$avail_mib fixed_mib=$fixed userdata_mib=$userdata_mib"
}

# 重新安装（用户 2026-09-25）：盘上已经有我们的 Android 时，另外两种方式都走不通 ——
# 整盘清空会锯掉安装器自己（免 U 盘时它就在这块盘上），双系统会建出第二套同名分区。
# 这里不改分区表，按 PARTLABEL 认出现有的每个分区，逐个决定：写新系统 / 格式化 / 保留。
# ★ 每个名字必须【恰好一个】：零个 = 不是一套完整的安装；两个 = 不知道写哪个
#   （by-name 链接重名时哪个赢不确定，见上面分区名查重的注释）。
# ⚠️ 纯计算，不碰盘；目标分区挂没挂着、是不是安装器所在的分区，由 gk3_apply 在动盘之前查。
gk3__plan_reinstall() {
    local disk=$1 rescue=$2 keep=$3 esp=$4
    command -v sgdisk >/dev/null || { echo "PLANERR msg=no-sgdisk"; return 1; }
    [ -n "$esp" ] || { echo "PLANERR msg=reinstall-needs-esp"; return 1; }
    local table="" n nm
    for n in $(sgdisk -p "$disk" 2>/dev/null | awk '/^ *[0-9]+ /{print $1}'); do
        nm=$(sgdisk -i "$n" "$disk" 2>/dev/null | grep "^Partition name:" | cut -d"'" -f2)
        table="$table$nm $n
"
    done
    local want="misc metadata boot_a boot_b super userdata" miss="" dup="" c
    [ "$rescue" = yes ] && want="$want gk3rescue"
    for nm in $want; do
        c=$(printf '%s' "$table" | awk -v x="$nm" '$1 == x' | wc -l | tr -d ' ')
        [ "$c" = 0 ] && miss="$miss $nm"
        [ "$c" -gt 1 ] && dup="$dup $nm"
    done
    [ -z "$dup" ] || { echo "PLANERR msg=reinstall-duplicate names=$(echo $dup | tr ' ' ',')"; return 1; }
    [ -z "$miss" ] || { echo "PLANERR msg=reinstall-missing names=$(echo $miss | tr ' ' ',')"; return 1; }

    echo "PLAN op=useesp path=$esp need_mib=$GK3_ESP_REINSTALL_NEED_MIB"
    local st en mib kib act min fixed=0 ud=0
    for nm in $want; do
        n=$(printf '%s' "$table" | awk -v x="$nm" '$1 == x {print $2}')
        st=$(sgdisk -i "$n" "$disk" 2>/dev/null | awk '/^First sector:/{print $3}')
        en=$(sgdisk -i "$n" "$disk" 2>/dev/null | awk '/^Last sector:/{print $3}')
        kib=$(( (en - st + 1) / 2 )); mib=$(( kib / 1024 ))
        # ⚠️ 下限按 KiB 比。2026-09-25 真机：本机的 misc 只有 1007 KiB（放在 GPT 头之后那段空隙里，
        #    Android 天天在用），按 MiB 取整是 0 ——"misc 太小"把重新安装整个拦住了。
        #    misc 的下限取实测在用的那个量级（1000 KiB）：更小的没验证过，不放行。
        case "$nm" in
            super) min=$(( GK3_SUPER_MIB * 1024 )); act=write ;;
            boot_a|boot_b) min=$(( GK3_BOOT_MIB * 1024 )); act=write ;;
            misc) min=1000; act=write ;;
            gk3rescue) min=$(( GK3_RESCUE_MIB * 1024 )); act=write ;;
            metadata) min=$(( GK3_METADATA_MIB * 1024 )); [ "$keep" = yes ] && act=keep || act=format ;;
            userdata) min=$(( GK3_USERDATA_MIN_MIB * 1024 )); [ "$keep" = yes ] && act=keep || act=format ;;
        esac
        # 新系统要装得下：super / boot 比这一版要的小，写到一半才会发现
        [ "$kib" -ge "$min" ] || { echo "PLANERR msg=reinstall-part-small name=$nm have_kib=$kib need_kib=$min"; return 1; }
        echo "PLAN op=reuse name=$nm path=$(gk3_partpath "$disk" "$n") num=$n start=$st end=$en size_mib=$mib size_kib=$kib action=$act"
        if [ "$nm" = userdata ]; then ud=$mib; else fixed=$(( fixed + mib )); fi
    done
    echo "PLANSUM mode=reinstall rescue=$rescue keep_data=$keep avail_mib=$(( fixed + ud )) fixed_mib=$fixed userdata_mib=$ud"
}

# 打印一条 mkpart 记录，并把游标 GK3_CUR 推到下一个 1 MiB 边界。
#
# ⚠️★ 第一版是"回显新游标"，调用方写 `cur=$(gk3__emit_part …)` ——
#   于是 **PLAN 行也被 $() 吞进变量里**，一条都没到 stdout，
#   而 cur 变成了带换行的字符串，下一次算术当场 unbound variable。
#   自测（test-plan.sh）一跑就抓到了。函数既要输出数据又要回传值时，
#   **走全局变量，别混用 stdout**。
gk3__emit_part() {
    local disk=$1 name=$2 mib=$3 type=$4
    local start=$GK3_CUR
    local end=$(( start + mib * 2048 - 1 ))
    echo "PLAN op=mkpart disk=$disk num=0 name=$name start=$start end=$end size_mib=$mib type=$type"
    GK3_CUR=$(( end + 1 ))
}

# ── 执行 ────────────────────────────────────────────────────────────────────
# 这是【唯一】会写盘的函数。
#
#   gk3_apply --disk X --mode wipe|alongside|reinstall --rescue yes|no --release DIR \
#             [--region-start S --region-end E --esp PATH] [--userdata-mib N] [--keep-data yes|no] \
#             [--default-os android|windows] [--bitlocker-key yes]
#   reinstall：不改分区表，复用盘上现有的那套分区（gk3__plan_reinstall）；--keep-data yes 不格式化
#   userdata / metadata（默认格式化 —— 跨版本降级时保留的数据可能起不来）
#   --default-os（双系统，U12）：冷开机默认进哪个系统，默认 android。windows 要 ESP 上有 Windows 的启动管理器。
#     ★ 不在这里写 LoaderEntryDefault：写成 GK3 记录里的 set_default 请求（gk3-misc init --default），由统一启动入口
#       第一次在动作模式下运行时按 core/src/dual.c 的规则写 —— 与 Android 里 Parts 改默认系统同一条通路。理由见
#       下面"misc"一节的注释（直接写的话，装完第一次重启就进了 Windows，而且 Android 里的重启也会落进 Windows）
#   --bitlocker-key yes（U16）：Windows 卷是 BitLocker、而这次要改 EFI/BOOT/BOOTAA64.EFI 时必须给 —— 用户在界面上确认过
#     "已拿到恢复密钥"。没给就在动盘之前拒绝（ERR bitlocker-key-unconfirmed）
#
# 发布目录 = GitHub Release / R2 的 install/<VER>/ 下载下来的那一份：
#   boot.img                         必需
#   super.img.zst 或 super.img       必需（前者是发版产物，后者是构建机 out/ 里的）
#   recovery-ramdisk.img             可选（发版不带，见 docs/INSTALL.md）
#   systemd-bootaa64.efi             可选（live 镜像自带 /usr/share/gaokun3/ 那份）
#   rescue.squashfs + initramfs.img  --rescue yes 时必需；发布目录里没有就用
#                                    启动介质上的（/media/gk3/gaokun3/，build-usb.sh 放的；
#                                    Windows 安装包那条路上是 live.squashfs，gk3__find_rescue_squashfs）
#   wpa_supplicant.conf              可选，装进救援分区
#
# 进度打在 stderr：`PROGRESS <百分比> <说明>`，其余是日志。
#
# ⚠️★ GK3_DRYRUN=1 时【只打印不执行】。写这个开关不是为了方便 ——
#   是因为这段代码一旦错了就是别人的一整块盘，而它没法在 CI 里跑。
#   任何改动都应当先用 dry-run 看一遍要执行的命令序列。
#
# ⚠️★ 所有输入【在第一次写盘之前】验完。原先 systemd-boot 和散装内核文件的
#   检查排在写完 super 之后 —— 缺一个就是分区表已改、super 已写、然后死在
#   引导链那一步，留下一块半装的盘（本函数下面 ESP 那段的注释警告过同一件事）。
# ESP 上 <machine-id>/ 用哪个目录。$1=挂载点 $2=没有现成目录时用的名字
# ★ 必须与 OTA postinstall 找目录的规则【一致】：第一个 32 位十六进制的目录 ——
#   不一致的话 OTA 写进一个目录、启动项指着另一个。
#   ⓘ 2026-10-05（v1.0 OTA-9）：postinstall 改成从 *-android-<槽>.conf 的 linux 行反推目录、条目不是恰好一个就让
#     OTA 失败（gaokun3-ota-postinstall.sh 里 OTA-9 那段）。本安装器写的条目就是 linux /$mid/android/slot_$slot/Image，
#     所以装完两边仍选同一个目录；⬜ 重新安装时若 ESP 上已有我们的条目，更稳的是也按它的目录选（本轮没改这里）。
# ⚠️★ 2026-09-26 M4b 实测：这里原先直接用【正在跑的系统】的 /etc/machine-id。live 的 machine-id
#   是 systemd 每次开机现生成的，于是重新安装在 ESP 上另开了一个目录、又写了一整套内核
#   （46 MB），ESP 写满，slot_b 的 ramdisk 截断在 2.8 MB、它的启动项是空文件 —— 而安装报告成功；
#   default 的通配 *-android-b.conf 从此同时匹配新旧两个条目，下一次 OTA 就是抛硬币。
gk3__esp_pick_mid() {
    local d; d=$(LC_ALL=C ls "$1" 2>/dev/null | grep -E '^[0-9a-f]{32}$' | head -1)
    echo "${d:-$2}"
}

# 往 ESP 上写一组文件还要多少 KiB。$1=挂载点，其余是 <ESP 上的相对路径>=<源文件>。
# 同一路径上已有的文件会被覆盖，只算变大的那部分；每个文件按 4 KiB 簇向上取整
gk3__esp_delta_kib() {
    local m=$1 t src new old kib=0; shift
    for t in "$@"; do
        src=${t#*=}; t=${t%%=*}
        new=$(wc -c < "$src"); old=0
        [ -f "$m/$t" ] && old=$(wc -c < "$m/$t")
        [ "$new" -gt "$old" ] && kib=$(( kib + (new - old + 4095) / 4096 * 4 ))
    done
    echo "$kib"
}

# systemd-boot 从哪来（gk3_apply 往 ESP 上写的那一份；gk3_esp_info 拿它比 BOOTAA64）。$1=发布目录（可空）
gk3__sdboot() {
    if [ -n "${GK3_SDBOOT:-}" ]; then [ -f "$GK3_SDBOOT" ] && echo "$GK3_SDBOOT"; return; fi
    gk3__find_file systemd-bootaa64.efi /usr/share/gaokun3 "${1:-}" /usr/lib/systemd/boot/efi
}

# gk3-misc（tools/gk3boot/misc/gk3-misc.c）：安装器用它的 init 子命令初始化 misc（见 tools/gk3boot/README.md）。
# 找的顺序：GK3_MISC_TOOL → 与本库同目录（live 镜像的 /usr/share/gaokun3/gk3-misc，build-rootfs.sh 静态编进去的）→ PATH →
#   仓库 checkout（命令行版从 scripts/live/ source 本库）里就地用 cc 编一份到 $GK3_RUNDIR/bin/（源文件比它新就重编）。
# ⚠️ 不用 Python 另写一份：BCAB 与 GK3 记录的布局只在 libgk3core 里有一份（入口、HAL、执行端、这里都用它）
gk3__misc_tool() {
    local t src out
    for t in "${GK3_MISC_TOOL:-}" "$GK3_LIBDIR/gk3-misc"; do
        [ -n "$t" ] && [ -x "$t" ] && { echo "$t"; return 0; }
    done
    t=$(command -v gk3-misc 2>/dev/null) && { echo "$t"; return 0; }
    src=$GK3_LIBDIR/../../tools/gk3boot
    [ -f "$src/misc/gk3-misc.c" ] && command -v cc >/dev/null 2>&1 || return 1
    out=$GK3_RUNDIR/bin/gk3-misc
    if [ ! -x "$out" ] || [ -n "$(find "$src/misc" "$src/core" -name '*.[ch]' -newer "$out" 2>/dev/null | head -1)" ]; then
        mkdir -p "$GK3_RUNDIR/bin" || return 1
        # 编法同 tools/gk3boot/Makefile 的 $(B)/gk3-misc
        cc -std=c11 -O2 -I"$src/core/include" -o "$out.new" "$src/misc/gk3-misc.c" "$src"/core/src/*.c >&2 \
            && mv -f "$out.new" "$out" || { rm -f "$out.new"; return 1; }
        echo "gk3-misc：从仓库源码编了一份 → $out" >&2
    fi
    echo "$out"
}

# 同一块盘上的 Windows 卷是什么状态（双系统写 ESP 之前看，见 tools/gk3boot/README.md）。
#   $1=ESP 分区节点 → 一行 `bitlocker=yes|no hibernated=yes|no|unknown hib_part=<分区>`
# hibernated：任一 NTFS 卷的 hiberfil.sys 以 hibr/HIBR 开头（gk3__ntfs_hibernated，ntfscat 只读）⇒ yes；
#   没有 ntfscat、或者盘上有 BitLocker 卷（里面的 NTFS 这边读不到）而别的 NTFS 都没在休眠 ⇒ unknown；否则 no。
# ⚠️ 只看 ESP 所在的那块盘：Windows 的 C: 在别的盘上时这里看不到（本机只有一块 NVMe）
gk3__win_state() {
    local disk p fs bl=no hib=no hp="" unk=no
    disk=$(gk3__disk_of "$1") || { echo "bitlocker=no hibernated=unknown hib_part="; return 0; }
    for p in $(lsblk -nrpo NAME "$disk" 2>/dev/null); do
        [ "$p" != "$disk" ] && [ -b "$p" ] || continue
        fs=$(blkid -p -o value -s TYPE "$p" 2>/dev/null)
        case "$fs" in
            BitLocker) bl=yes ;;
            ntfs)
                if ! command -v ntfscat >/dev/null 2>&1; then unk=yes
                elif [ "$hib" = no ] && gk3__ntfs_hibernated "$p"; then hib=yes; hp=$p; fi ;;
        esac
    done
    [ "$hib" = no ] && { [ "$bl" = yes ] || [ "$unk" = yes ]; } && hib=unknown
    echo "bitlocker=$bl hibernated=$hib hib_part=$hp"
}

# ── systemd-boot 的 EFI 变量（LoaderEntryDefault / LoaderEntryOneShot）─────────────────────────
# 文件格式同 overlay-common/usr/bin/gk3-boot-android：4 字节属性 + UTF-16LE + 双字节 NUL；覆盖 / 删之前先 chattr -i。
# GK3_EFIVARS=<目录>：测试时当作 efivarfs（与 gk3-boot-android 同一个开关）。
# live 的内核 cmdline 带 efi=noruntime，但变量照样能读写：uefisecapp（见 tools/gk3boot/README.md）；
#   在真机的 live 里删变量尚未实测（gk3-boot-android 写 OneShot 同样待核）
gk3__efivars() {   # efivarfs 的目录（没挂就挂一次）；不可用 ⇒ 返回 1
    local d=${GK3_EFIVARS:-/sys/firmware/efi/efivars}
    [ -n "${GK3_EFIVARS:-}" ] && { [ -d "$d" ] && echo "$d"; return; }
    [ -n "$(ls -A "$d" 2>/dev/null)" ] || mount -t efivarfs efivarfs "$d" 2>/dev/null
    [ -n "$(ls -A "$d" 2>/dev/null)" ] && echo "$d"
}
gk3__loader_var() {   # $1=名字 → 打印值；不存在 ⇒ 返回 1；efivarfs 不可用 ⇒ 返回 2
    local d; d=$(gk3__efivars) || return 2
    [ -f "$d/$1-$GK3_LOADER_GUID" ] || return 1
    tail -c +5 "$d/$1-$GK3_LOADER_GUID" 2>/dev/null | tr -d '\000'
}
gk3__loader_var_del() {   # $1=名字；本来就没有也算成功
    local d f; d=$(gk3__efivars) || return 1
    f=$d/$1-$GK3_LOADER_GUID
    [ -e "$f" ] || return 0
    chattr -i "$f" 2>/dev/null; rm -f "$f" 2>/dev/null
    [ ! -e "$f" ]
}

# 写完之后从【介质】读回来核对：卸下、丢掉这块设备的缓存（blockdev --flushbufs）、只读挂回去逐个 cmp。
# 挂着直接 cmp 读的是页缓存 —— 介质在回写时才报的错那样看不出来（2026-09-27 审查）。
#   gk3__verify_on <分区> <文件系统类型> <分区上的相对路径>=<源文件> …
gk3__verify_on() {
    local dev=$1 fst=$2 m w bad=""; shift 2
    blockdev --flushbufs "$dev" 2>/dev/null
    m=$(mktemp -d)
    mount -o ro -t "$fst" "$dev" "$m" 2>/dev/null || { rmdir "$m"; gk3_fail verify-remount "dev=$dev" -- "写完之后 $dev 挂不回来"; return 1; }
    for w in "$@"; do
        cmp -s "$m/${w%%=*}" "${w#*=}" || { bad=${w%%=*}; break; }
    done
    umount "$m"; rmdir "$m" 2>/dev/null
    [ -z "$bad" ] || { gk3_fail verify-mismatch "dev=$dev" -- "$dev 上的 $bad 与源文件不一致（读回来核对没过）"; return 1; }
    echo "${dev}：$# 个文件从介质读回核对一致" >&2
}

gk3_apply() {
    local disk="" mode=wipe rescue=no rel="" rstart="" rend="" esp="" ud_mib="" keep=no defos=android blkey=no
    # 目标盘动过没有（gk3_fail 把它带进 ERR 的 touched=）。【不是】local：命令行版在失败之后要读它
    GK3__TOUCHED=no
    while [ $# -gt 0 ]; do
        case "$1" in
            --disk) disk=$2; shift 2 ;;
            --keep-data) keep=$2; shift 2 ;;
            --mode) mode=$2; shift 2 ;;
            --rescue) rescue=$2; shift 2 ;;
            --release) rel=$2; shift 2 ;;
            --region-start) rstart=$2; shift 2 ;;
            --region-end) rend=$2; shift 2 ;;
            --esp) esp=$2; shift 2 ;;
            --userdata-mib) ud_mib=$2; shift 2 ;;
            --default-os) defos=$2; shift 2 ;;
            --bitlocker-key) blkey=$2; shift 2 ;;
            *) gk3_fail usage -- "apply: 不认识的参数 $1"; return 1 ;;
        esac
    done
    [ -n "$disk" ] || { gk3_fail usage -- "apply 要 --disk"; return 1; }
    [ -n "$rel" ] && [ -d "$rel" ] || { gk3_fail usage -- "apply 要 --release <目录>"; return 1; }
    case "$defos" in android|windows) ;; *) gk3_fail usage -- "--default-os 只能是 android 或 windows（给的是 '$defos'）"; return 1 ;; esac
    [ "$defos" = windows ] && [ "$mode" = wipe ] && { gk3_fail default-os-no-windows -- "整盘清空之后盘上没有 Windows，不能把它设成默认系统"; return 1; }

    local DRY=${GK3_DRYRUN:-0}
    # ⚠️ 回显走 stderr：stdout 只留给行记录（文件头的协议）。原先 "+ 命令" 与
    #    各种说明都 echo 到 stdout —— 前端靠"长得不像记录"才没被它们骗过去。
    gk3__run() {
        if [ "$DRY" = 1 ]; then echo "DRY: $*" >&2; else
            echo "+ $*" >&2
            "$@" >&2 || { gk3_fail cmd-failed "cmd=$1" -- "失败：$*"; return 1; }
        fi
    }

    # ── 输入：全部在动盘之前验完 ─────────────────────────────────────────
    gk3_prog 1 check
    [ -f "$rel/boot.img" ] || { gk3_fail release-no-boot -- "发布目录里没有 boot.img"; return 1; }
    local super_src
    if   [ -f "$rel/super.img.zst" ]; then super_src=$rel/super.img.zst
    elif [ -f "$rel/super.img" ];     then super_src=$rel/super.img
    else gk3_fail release-no-super -- "发布目录里既没有 super.img.zst 也没有 super.img"; return 1; fi

    local sdboot
    if [ -n "${GK3_SDBOOT:-}" ]; then
        # 显式指定的就用它，叫什么名字都行（原先按文件名找，于是这个开关只在文件
        # 恰好叫 systemd-bootaa64.efi 时才生效 —— 录 fixture 时才发现）
        [ -f "$GK3_SDBOOT" ] || { gk3_fail sdboot-missing -- "GK3_SDBOOT 指的文件不存在：$GK3_SDBOOT"; return 1; }
        sdboot=$GK3_SDBOOT
    else
        sdboot=$(gk3__find_file systemd-bootaa64.efi /usr/share/gaokun3 "$rel" /usr/lib/systemd/boot/efi) \
            || { gk3_fail sdboot-missing -- "找不到 systemd-bootaa64.efi（Debian：apt install systemd-boot-efi）"; return 1; }
    fi

    local r_squash="" r_initrd=""
    if [ "$rescue" = yes ]; then
        # 找救援镜像的顺序见 gk3__find_rescue_squashfs（发布目录 → U 盘上专门放的 → 正在跑的这个）
        r_squash=$(gk3__find_rescue_squashfs "$rel") \
            || { gk3_die "选了装救援系统，但发布目录和启动介质上都没有 rescue.squashfs"; return 1; }
        r_initrd=$(gk3__find_file initramfs.img "$rel" /media/gk3/gaokun3) \
            || { gk3_die "选了装救援系统，但发布目录和启动介质上都没有 initramfs.img"; return 1; }
    fi

    local t
    for t in sgdisk partprobe blkid lsblk mkfs.vfat mkfs.ext4 python3; do
        command -v "$t" >/dev/null || { gk3_fail tool-missing "tool=$t" -- "缺工具：$t"; return 1; }
    done
    case "$super_src" in *.zst) command -v zstd >/dev/null || { gk3_fail tool-missing tool=zstd -- "缺工具：zstd"; return 1; } ;; esac
    local misc_tool
    misc_tool=$(gk3__misc_tool) || { gk3_fail tool-missing tool=gk3-misc -- "缺工具：gk3-misc（live 镜像自带；从仓库跑命令行版时要能用 cc 现编 tools/gk3boot/misc/gk3-misc.c）"; return 1; }

    # ★ 目标盘的分区表：读不出 ⇒ disk-unreadable；MBR ⇒ mbr-disk（整盘清空除外）。都在动盘之前、在 ESP 检查之前 ——
    #   2026-10-06 之前 MBR 盘上的双系统是被 ESP 检查【碰巧】挡住的（MBR 分区的类型不是 EFI System ⇒ esp-not-esp-type），
    #   用户看到的是"ESP 类型不对"而不是"这是 MBR 盘"；哪天 ESP 检查放宽，就会真的把 MBR 盘写成 GPT（设计稿 S2）
    gk3__table_guard "$disk" "$([ "$mode" = wipe ] && echo mbr-ok)" || return 1

    # ★ 完整性也在动盘之前验。下载断在一半的 super.img.zst 能通过上面所有检查，
    #   要到流式写盘写到一半才暴露 —— 那时分区表已经改了。有发版的校验清单
    #   （install-artifacts.sha256，scripts/release.sh 生成）就逐个核；没有清单时
    #   .zst 至少完整试解一遍（zstd -t，几秒钟）。
    if [ -f "$rel/install-artifacts.sha256" ]; then
        gk3_prog 1 verify-sha
        local want f got
        while read -r want f; do
            f=${f#\*}
            [ -f "$rel/$f" ] || continue          # 清单里的 OTA zip 装机用不到，没下就算了
            case "$f" in boot.img|super.img.zst|super.img) ;; *) continue ;; esac
            got=$(sha256sum "$rel/$f" 2>/dev/null | cut -d' ' -f1)
            [ -n "$got" ] || got=$(shasum -a 256 "$rel/$f" | cut -d' ' -f1)
            [ "$got" = "$want" ] || { gk3_fail sha256-mismatch "file=$f" -- "$f 的 sha256 与发版清单不符 —— 下载不完整或被改过（盘还没动过）"; return 1; }
            echo "${f}：sha256 与发版清单一致" >&2
        done < "$rel/install-artifacts.sha256"
    elif [ "${super_src%.zst}" != "$super_src" ]; then
        gk3_prog 1 test-zst
        zstd -tq --long=31 "$super_src" \
            || { gk3_fail zst-corrupt -- "super.img.zst 不完整（zstd -t 没通过）—— 重新下载（盘还没动过）"; return 1; }
    fi

    # boot.img 当场拆开：拆不开说明镜像有问题 —— 而此刻盘还一个字节都没动。
    # （拆包逻辑与设备侧 bootimg_extract.cpp 同源，见 gk3-bootimg.py 的文件头）
    local parts; parts=$(mktemp -d)
    python3 "$GK3_LIBDIR/gk3-bootimg.py" "$rel/boot.img" "$parts" \
        || { rm -rf "$parts"; gk3_fail bootimg-unpack -- "boot.img 拆不开 —— 盘还没动过"; return 1; }
    # ★ cmdline 取自 boot.img（BOARD_KERNEL_CMDLINE），不在这里另抄一份：
    #   这里原先手抄的那份缺 androidboot.boot_devices（首阶段 by-name 解析靠它）、
    #   init=/init、himax disable_pressure=0 等（TODO B15）。
    local cmdline; cmdline=$(tr -d '\r\n' < "$parts/cmdline.txt")

    # ── 双系统：别人的 ESP 在动盘之前验完 ────────────────────────────────
    # ⚠️★ 2026-09-24 loop 端到端测试抓到：这里原先在【写完分区表之后】才按
    #   PARTLABEL 找名叫 "esp" 的分区 —— 而 Windows 的 ESP 叫 "EFI system partition"，
    #   于是双系统模式在任何一台 Windows 机器上都必然失败，并留下一串建了一半的
    #   分区。空间检查也排在分区表之后（下面格式化那段的注释说过为什么那样不行）。
    #   双系统的 apply 此前从未真跑过：真盘上验过的只是 gk3_plan 的方案计算。
    # ESP 上 <machine-id>/ 的名字：双系统 / 重新安装在动盘前按现有 ESP 选（gk3__esp_pick_mid），
    # 整盘清空时 ESP 是新格式化的，在写引导链时再选（那时没有现成目录 → 用这个）
    local esp_mid="" esp_win=no mid_fb=${GK3_MACHINE_ID:-$(cat /etc/machine-id 2>/dev/null || echo 8a29534fa802480d9fbb71aa18c01d7b)}
    # 要往 ESP 上写的文件：<相对路径>=<源>。算空间与写完之后逐个核对用的是同一份清单
    gk3__esp_files() {
        local sl f
        echo "EFI/BOOT/BOOTAA64.EFI=$sdboot"
        echo "EFI/systemd/systemd-bootaa64.efi=$sdboot"
        for sl in a b; do
            for f in Image gaokun3.dtb ramdisk.img; do echo "$1/android/slot_$sl/$f=$parts/$f"; done
            [ ! -f "$rel/recovery-ramdisk.img" ] || echo "$1/android/slot_$sl/recovery-ramdisk.img=$rel/recovery-ramdisk.img"
        done
        [ "$rescue" != yes ] || echo "$1/rescue/initramfs.img=$r_initrd"
    }
    if [ "$mode" != wipe ]; then
        [ -n "$esp" ] && [ -b "$esp" ] || { rm -rf "$parts"; gk3_fail esp-missing "esp=$esp" -- "双系统模式要 --esp <现有 ESP 的分区节点>，给的是 '${esp}'"; return 1; }
        if [ "$DRY" != 1 ]; then
            local etype efs
            efs=$(blkid -o value -s TYPE "$esp" 2>/dev/null)
            # ⚠️ 不用 lsblk -o PARTTYPE：它要 udev 数据库，没有 udev 时是空串（容器里实测）。
            #    blkid -p 是 libblkid 直接读分区表，救援系统里也成立。
            etype=$(blkid -p -o value -s PART_ENTRY_TYPE "$esp" 2>/dev/null | tr 'a-f' 'A-F')
            [ "$efs" = vfat ] || { rm -rf "$parts"; gk3_fail esp-not-fat "esp=$esp" "fs=$efs" -- "$esp 不是 FAT 文件系统（是 '${efs}'）—— 不像一个 ESP"; return 1; }
            [ "$etype" = C12A7328-F81F-11D2-BA4B-00A0C93EC93B ] \
                || { rm -rf "$parts"; gk3_fail esp-not-esp-type "esp=$esp" "type=$etype" -- "$esp 的分区类型不是 EFI System（是 '${etype}'）"; return 1; }
            # ⚠️ 2026-09-29（SELinux 第七轮）：Android 侧按 by-name 名字给 ESP 打专用标签，只认
            #   `esp` 与 Windows 默认的 "EFI system partition"（sepolicy/file_contexts）。别的名字
            #   （空、大小写不同、本地化）在 enforcing 下 bootctl 与 OTA 都打不开它。不替别人改名，只警告。
            local ename; ename=$(blkid -p -o value -s PART_ENTRY_NAME "$esp" 2>/dev/null)
            case "$ename" in
                esp|"EFI system partition") ;;
                *) echo "⚠️ ESP 的分区名是 '${ename}'，不是 esp / EFI system partition —— 以后的 enforcing 版本上切槽与 OTA 会失败。可以用 sgdisk -c <号>:esp 改名（UEFI 只认类型 GUID，改名无害）" >&2 ;;
            esac
            # ⚠️★ ② 真的去量它有多少空闲。原先只在 plan 里打一行 need_mib=150
            #   却从不验证 —— 不够的话分区表已经改完、super 已经写完，然后死在
            #   装引导链那一步，留下一块半装的盘。
            local em fm; em=$(mktemp -d)
            if mount -o ro -t vfat "$esp" "$em" 2>/dev/null; then
                fm=$(df -m "$em" | awk 'NR==2{print $4}')
                # ★ 按【真要写的文件】算（2026-09-26 M4b 之后）：新文件减去同一路径上会被覆盖的旧文件
                esp_mid=$(gk3__esp_pick_mid "$em" "$mid_fb")
                local -a efl; mapfile -t efl < <(gk3__esp_files "$esp_mid")
                local need_kib free_kib slot_kib
                need_kib=$(( $(gk3__esp_delta_kib "$em" "${efl[@]}") + 256 ))    # 256：启动项、loader.conf
                if [ -f "$em/EFI/BOOT/BOOTAA64.EFI" ] && [ ! -e "$em/EFI/BOOT/BOOTAA64.EFI.before-gaokun3" ] \
                   && ! cmp -s "$em/EFI/BOOT/BOOTAA64.EFI" "$sdboot"; then        # 原件要留一份（见写引导链那一段）
                    need_kib=$(( need_kib + $(wc -c < "$em/EFI/BOOT/BOOTAA64.EFI") / 1024 + 4 ))
                fi
                free_kib=$(df -k "$em" | awk 'NR==2{print $4}')
                slot_kib=$(( $(cat "$parts/Image" "$parts/gaokun3.dtb" "$parts/ramdisk.img" | wc -c) / 1024 ))
                # 双系统（共用 Windows 的 ESP）：vfat 挂载不分大小写（Windows 写的是 EFI/Microsoft/Boot/bootmgfw.efi）
                [ -f "$em/EFI/Microsoft/Boot/bootmgfw.efi" ] && esp_win=yes
                # 这次会不会改 BOOTAA64 的字节（U16：只在字节不同时写；会改 + Windows 卷是 BitLocker ⇒ 要用户确认拿到了恢复密钥）
                local aa_change=no
                if [ ! -f "$em/EFI/BOOT/BOOTAA64.EFI" ] || ! cmp -s "$em/EFI/BOOT/BOOTAA64.EFI" "$sdboot"; then aa_change=yes; fi
                umount "$em"; rmdir "$em" 2>/dev/null
                echo "ESP 上用目录 ${esp_mid}；要写 $(( need_kib / 1024 )) MiB（已扣掉会被覆盖的同名文件），空闲 $(( free_kib / 1024 )) MiB；Windows 的启动管理器：${esp_win}；BOOTAA64 要变：${aa_change}" >&2
                if [ "$esp_win" = yes ]; then
                    # ★ U18：Windows 休眠（快速启动的"关机"也是休眠，默认开着）时它缓存着 ESP 的 FAT —— 这边写了，Windows 醒来
                    #   再写就可能把 FAT 写坏（设计稿 §4.9.10，D4 定级）。判据见 gk3__ntfs_hibernated（只读）
                    local ws; ws=$(gk3__win_state "$esp")
                    echo "Windows 卷：$ws" >&2
                    if [ "$(gk3__f "$ws" hibernated)" = yes ]; then
                        rm -rf "$parts"
                        gk3_fail esp-windows-hibernated "part=$(gk3__f "$ws" hib_part)" -- "Windows 处于休眠或“快速启动”状态（$(gk3__f "$ws" hib_part)）—— 这时往共用的 EFI 分区里写东西，Windows 醒来时可能把它写坏。回 Windows 关掉快速启动、用“关机”退出，再试（盘还没动过）"
                        return 1
                    fi
                    [ "$(gk3__f "$ws" hibernated)" = unknown ] && echo "⚠️ 看不出 Windows 是否在休眠（BitLocker 卷读不到里面，或缺 ntfscat）—— 请确认是在 Windows 里用“关机”（不是休眠）退出的、快速启动已关" >&2
                    # ★ U16 规则 3：BitLocker 的卷 + 这次要换掉 BOOTAA64（固件每次开机进的就是它，PCR4 会变）⇒ 下一次进 Windows
                    #   多半要恢复密钥。Linux 这边暂停不了 BitLocker，只能要用户确认手里有密钥（界面上那个勾选框）
                    if [ "$(gk3__f "$ws" bitlocker)" = yes ] && [ "$aa_change" = yes ] && [ "$blkey" != yes ]; then
                        rm -rf "$parts"
                        gk3_fail bitlocker-key-unconfirmed -- "Windows 的卷是 BitLocker 加密的，这次安装要换掉 EFI/BOOT/BOOTAA64.EFI —— 下次进 Windows 可能要 48 位恢复密钥。确认拿到了恢复密钥（https://aka.ms/myrecoverykey）再装（盘还没动过）"
                        return 1
                    fi
                elif [ "$defos" = windows ]; then
                    rm -rf "$parts"
                    gk3_fail default-os-no-windows "esp=$esp" -- "$esp 上没有 Windows 的启动管理器（EFI/Microsoft/Boot/bootmgfw.efi），不能把 Windows 设成默认系统（盘还没动过）"
                    return 1
                fi
                # ★ U17：用户自己重装 Windows 得到的 100 MiB ESP，装完是负数 —— 明确拒绝，不让它去撞下面的空间检查
                #   （那样只会说"空间不够、请清理"，而清理也清不出来）。重新安装不查：我们已经在这个 ESP 上了
                local esp_size_mib=$(( $(blockdev --getsize64 "$esp" 2>/dev/null || echo 0) / 1048576 ))
                if [ "$mode" = alongside ] && [ "$esp_size_mib" -gt 0 ] && [ "$esp_size_mib" -lt "$GK3_ESP_MIN_SIZE_MIB" ]; then
                    rm -rf "$parts"
                    gk3_fail esp-too-small "size_mib=$esp_size_mib" "min_mib=$GK3_ESP_MIN_SIZE_MIB" -- "这个 EFI 分区只有 ${esp_size_mib} MiB（重装 Windows 建的那种），装不下 Android 的启动文件、也没法给 Windows 更新留余量。1.0 不支持这种布局（以后的版本会改用单独的 XBOOTLDR 分区）（盘还没动过）"
                    return 1
                fi
                if [ "$free_kib" -lt "$need_kib" ]; then
                    rm -rf "$parts"
                    gk3_fail esp-full "need_mib=$(( need_kib / 1024 ))" "free_mib=$(( free_kib / 1024 ))" -- "ESP 空间不够：要写 $(( need_kib / 1024 )) MiB，只有 $(( free_kib / 1024 )) MiB。请先清理 EFI 分区（盘还没动过）"
                    return 1
                fi
                if [ $(( free_kib - need_kib + slot_kib )) -le "$GK3_ESP_OTA_NEED_KIB" ]; then
                    rm -rf "$parts"
                    gk3_fail esp-ota-room "left_mib=$(( (free_kib - need_kib) / 1024 ))" -- "ESP 装得下，但装完只剩 $(( (free_kib - need_kib) / 1024 )) MiB —— 以后的系统更新（OTA）会因为 ESP 空间不够失败。请先清理 EFI 分区（盘还没动过）"
                    return 1
                fi
                # ★ U17：双系统再加一道 —— 扣掉下一次 OTA 的暂时空间（一个槽 + 一版入口）之后，还要给 Windows 与固件留 32 MiB。
                #   ⚠️ 清理时绝不删 Persisted_Capsules.bin、EFI/Microsoft、EFI/UpdateCapsule（固件与 Windows 的）
                if [ "$esp_win" = yes ]; then
                    local win_left=$(( free_kib - need_kib - slot_kib - GK3_ESP_GK3BOOT_KIB ))
                    if [ "$win_left" -lt "$GK3_ESP_WIN_RESERVE_KIB" ]; then
                        rm -rf "$parts"
                        gk3_fail esp-win-reserve "left_mib=$(( win_left / 1024 ))" "need_mib=$(( GK3_ESP_WIN_RESERVE_KIB / 1024 ))" -- "装完、再扣掉以后系统更新要的暂时空间，EFI 分区只剩 $(( win_left / 1024 )) MiB，不够给 Windows 更新和固件留的 $(( GK3_ESP_WIN_RESERVE_KIB / 1024 )) MiB。请先在 Windows 里清理 EFI 分区（不要删 Persisted_Capsules.bin、EFI/Microsoft、EFI/UpdateCapsule）（盘还没动过）"
                        return 1
                    fi
                    echo "U17：装完、扣掉一次 OTA 的暂时空间后还剩 $(( win_left / 1024 )) MiB 给 Windows 与固件（要 ≥ $(( GK3_ESP_WIN_RESERVE_KIB / 1024 ))）" >&2
                fi
                # 重新安装是【覆盖】ESP 上我们自己的文件，不是新增 —— 按 150 MiB 要求的话，一台已经装过的
                # 机器（我们的文件占了一百多 MiB）会被误判成"空间不够"
                local eneed=$GK3_ESP_NEED_MIB; [ "$mode" = reinstall ] && eneed=$GK3_ESP_REINSTALL_NEED_MIB
                echo "现有 ESP $esp 空闲 ${fm} MiB（需要 ${eneed}）" >&2
                if [ "${fm:-0}" -lt "$eneed" ]; then
                    rm -rf "$parts"
                    gk3_fail esp-full "need_mib=$eneed" "free_mib=$fm" -- "ESP 空间不够：只有 ${fm} MiB，需要 ${eneed} MiB。请先在原系统里清理 EFI 分区（盘还没动过）"
                    return 1
                fi
            else
                rmdir "$em" 2>/dev/null; rm -rf "$parts"
                gk3_fail esp-mount "esp=$esp" -- "挂不上现有 ESP $esp —— 不敢往一个读不了的 ESP 上装引导链（盘还没动过）"
                return 1
            fi
        fi
    fi

    # ── 安全闸 1：不能整盘清空自己正跑在上面的那块盘 ─────────────────────
    # ⚠️ 安装器要么从 U 盘跑、要么从内置盘上的某个分区跑（救援分区，或者免 U 盘安装时
    #    放 live 的那个分区）。后者做整盘清空等于把自己脚下的地板锯掉 —— 而且是【跑到一半】
    #    才死，盘已经毁了。★ 双系统（alongside）不受这道闸：它只往空闲区里建新分区，
    #    不碰任何已有分区 —— 免 U 盘装双系统正是"介质与目标同盘"的情形，必须放行。
    local medium_disk; medium_disk=$(gk3__medium_disk)
    if [ "$mode" = wipe ] && [ -n "$medium_disk" ] && [ "$medium_disk" = "$disk" ]; then
        rm -rf "$parts"
        gk3_fail wipe-medium "disk=$disk" -- "拒绝：安装介质就在目标盘 $disk 上，整盘清空会锯掉自己脚下的地板"
        return 1
    fi

    # ── 安全闸 2：目标盘上不能有已挂载的分区 ────────────────────────────
    local mounted
    mounted=$(lsblk -nro MOUNTPOINT "$disk" 2>/dev/null | grep -v '^$' | tr '\n' ' ')
    if [ -n "$mounted" ] && [ "$mode" = wipe ]; then
        rm -rf "$parts"
        gk3_fail wipe-mounted "disk=$disk" "mounts=$mounted" -- "拒绝：$disk 上还有挂载着的分区（${mounted}）"
        return 1
    fi

    # ── 方案 ────────────────────────────────────────────────────────────
    gk3_prog 2 plan
    local plan
    plan=$(gk3_plan --disk "$disk" --mode "$mode" --rescue "$rescue" --keep-data "$keep" \
                    ${rstart:+--region-start "$rstart"} ${rend:+--region-end "$rend"} \
                    ${esp:+--esp "$esp"} ${ud_mib:+--userdata-mib "$ud_mib"}) \
        || { echo "$plan"; rm -rf "$parts"; return 1; }
    if printf '%s\n' "$plan" | grep -q '^PLANERR'; then
        printf '%s\n' "$plan" | grep '^PLANERR'; rm -rf "$parts"; return 1
    fi

    # ── 安全闸 3（重新安装）：要写的分区一个都不能挂着 ─────────────────────
    # 首先是安装器自己所在的那个（免 U 盘时它就在这块盘上）。整盘清空有安全闸 1，双系统只碰空闲区；
    # 重新安装却是往【已有】分区里写 —— 所以逐个查。
    if [ "$mode" = reinstall ]; then
        local rp busy=""
        for rp in $(printf '%s\n' "$plan" | grep '^PLAN op=reuse' | sed 's/.* path=\([^ ]*\).*/\1/'); do
            findmnt -rn -S "$rp" >/dev/null 2>&1 && busy="$busy $rp"
        done
        if [ -n "$busy" ]; then
            rm -rf "$parts"
            gk3_fail reinstall-busy "parts=${busy# }" -- "拒绝：要重写的分区还挂着（${busy# }）—— 安装器是不是就从它上面跑的？"
            return 1
        fi
    fi

    # ── 建分区 ──────────────────────────────────────────────────────────
    # 从这里往下就可能已经改了目标盘（宁可早报"动过"：说没动过而其实动了，用户会放心地重启回旧系统）
    GK3__TOUCHED=yes
    # ⚠️★ ④ 动手之前先把分区表备份到介质上。出事能一条命令还原：
    #     sgdisk --load-backup=<文件> <盘>
    #   代价是几十 KB 和一秒钟；没有它的话，改错分区表就只能靠猜。
    if [ "$DRY" != 1 ] && [ "$mode" != reinstall ]; then
        gk3__gpt_backup "$disk" apply
    fi

    if [ "$mode" = reinstall ]; then
        echo "重新安装：不改分区表，复用现有的 $(printf '%s\n' "$plan" | grep -c '^PLAN op=reuse') 个分区" >&2
    else
    gk3_prog 5 write-gpt
    if printf '%s\n' "$plan" | grep -q '^PLAN op=wipe'; then
        gk3__run sgdisk --zap-all "$disk" || return 1
    fi
    local line name start end ptype
    while read -r line; do
        case "$line" in "PLAN op=mkpart"*) ;; *) continue ;; esac
        name=$(gk3__f "$line" name); start=$(gk3__f "$line" start)
        end=$(gk3__f "$line" end);   ptype=$(gk3__f "$line" type)
        gk3__run sgdisk -n "0:${start}:${end}" -t "0:${ptype}" -c "0:${name}" "$disk" || return 1
    done <<EOF
$(printf '%s\n' "$plan")
EOF
    gk3__run partprobe "$disk" || true
    [ "$DRY" = 1 ] || sleep 2
    fi

    # ── 格式化 ──────────────────────────────────────────────────────────
    gk3_prog 15 format
    # ⚠️★ 每一个分区节点都必须【解析成功且确实是块设备】才往下走。
    #   loop 设备实测暴露过：partprobe 还没沉降时 gk3__bylabel 会返回空串，
    #   于是命令变成 `dd of=` / `mkfs.ext4 -F ""` —— 那种情况下会发生什么
    #   完全不可预料，而此时分区表已经写下去了，盘已经不是原来的盘。
    #   **宁可在这里停住，也不能带着空路径继续。**
    local p_esp p_meta p_data p_super p_boota p_bootb p_resc p_misc
    if [ "$mode" = wipe ]; then
        p_esp=$(gk3__need_part "$disk" esp "$mode") || return 1
    else
        p_esp=$esp    # 别人的 ESP：名字不归我们管（Windows 叫它 "EFI system partition"）
    fi
    p_misc=$(gk3__need_part "$disk" misc "$mode")   || return 1
    p_meta=$(gk3__need_part "$disk" metadata "$mode")   || return 1
    p_data=$(gk3__need_part "$disk" userdata "$mode")   || return 1
    p_super=$(gk3__need_part "$disk" super "$mode")     || return 1
    p_boota=$(gk3__need_part "$disk" boot_a "$mode")    || return 1
    p_bootb=$(gk3__need_part "$disk" boot_b "$mode")    || return 1
    if [ "$rescue" = yes ]; then
        p_resc=$(gk3__need_part "$disk" gk3rescue "$mode") || return 1
    fi

    if [ "$mode" = wipe ]; then
        gk3__run mkfs.vfat -F 32 -n ESP "$p_esp" || return 1
    else
        echo "复用现有 ESP：${p_esp}（不格式化；空间与类型在动盘之前已验过）" >&2
    fi
    if [ "$mode" = reinstall ] && [ "$keep" = yes ]; then
        echo "保留用户数据：userdata（${p_data}）与 metadata（${p_meta}）不格式化" >&2
    else
        gk3__run mkfs.ext4 -q -F -L metadata "$p_meta" || return 1
        # -m 0：不给 root 留 5% 保留块（e2fsprogs 的默认）。Android 的应用全不是 uid 0，那 5% 谁也用不上 ——
        #   开发机 /data 实测 4928507 块（约 18.8 GiB）就这样白占着（v1.0 计划 STOR-5）。系统的保留空间
        #   由 fstab 的 reservedsize= 交给 fs_mgr 管（mount 前 tune2fs -r/-g，
        #   refs/lineage-system-core/fs_mgr/fs_mgr.cpp:412 tune_reserved_size）
        gk3__run mkfs.ext4 -q -F -m 0 -L userdata "$p_data" || return 1
    fi
    # misc：先整块清零（BCB、旧的 A/B 状态、VAB 合并状态、旧的 GK3 记录全不留），再写一份合法的初始状态。
    # ⚠️ 按分区的【实际大小】清零，不按 GK3_MISC_MIB：重新安装时复用的 misc 可能比 4 MiB 小
    #    （本机 1007 KiB）—— 按 4 MiB 写会在写满之后报 No space left，整个安装失败在这一步
    local misc_kib; misc_kib=$(( $(blockdev --getsize64 "$p_misc" 2>/dev/null || echo $(( GK3_MISC_MIB << 20 ))) / 1024 ))
    # conv=nocreat：节点要是在这之前被 udev 删了又没建回来，dd 会在 /dev 里新建一个普通文件、写成功、盘上什么也没有（审查 #4）
    gk3__run dd if=/dev/zero of="$p_misc" bs=1024 count="$misc_kib" conv=fsync,nocreat status=none || return 1
    # 清零之后用 gk3-misc init 写一份合法的初始状态，不再留全零（见 tools/gk3boot/README.md）：
    #   * bootloader_control：_a priority 15 / tries 6 / 未成功，_b priority 0 / tries 0。原先全零 ⇒ libboot_control 读到坏 CRC
    #     重建成"每槽 7/7、当前槽已成功"（libboot_control.cpp:140-180）—— _b 也成了可启动，而新装机器的 _b 没有 system、
    #     LP 里那一槽是陈旧元数据。统一启动入口按 BCAB 选槽，这一槽必须是 0/0。
    #   * GK3 记录（misc+8 KiB）：带迁移标记（BCB 刚清零 = 没有存量请求）⇒ 入口第一次在动作模式下运行时不把之后 Android 写的
    #     BCB（例如第一次开机就恢复出厂）当成存量清掉。HAL 与入口见到的都是一份有效记录。
    #   * 双系统的默认系统：写成 set_default 请求，由入口第一次在动作模式下运行时去写 / 删 LoaderEntryDefault
    #     （core/src/dual.c 的 gk3_dual_plan_default；条目 id 的选法 = 入口的 win_entry_id：有 gk3-windows.conf 用它，否则 auto-windows）。
    #     【不】在这里直接写 LoaderEntryDefault=auto-windows，因为：
    #       ① 装完第一次重启就会进 Windows —— 而第一次开机要进 Android 走开机向导；
    #       ② 第一次开机走的是直连条目（入口要等这一次开机完成时由 HAL 部署，见下面"统一启动入口"），入口没运行就没有
    #          "预置 OneShot"，于是 Android 里的任何重启都会落进 Windows；
    #       ③ 装的要是一个不带入口的旧版本（版本列表里能选），变量就再也没人管，同样每次重启都进 Windows。
    #     走请求这条路：第二次开机（经入口）时生效，此后与 Parts 里改默认系统完全相同；最坏情况是"默认没改成"，不是"进错系统"。
    #   统一启动入口本身【不】在这里部署（条目与 EFI/gk3boot/ 不写）：入口二进制在 vendor 里（/vendor/boot/gk3boot/），
    #     装好的系统在第一次开机完成时由 boot_control HAL 按 persist.vendor.gaokun3.gk3boot 部署它自己那一版 ——
    #     安装器另带一份就是第二份拷贝，版本会和系统对不上；第一次开机本来也没有要它保护的东西（_b 不可启动、没有 OTA）。
    local misc_def=none
    [ "$esp_win" = yes ] && misc_def=$defos
    if [ "$DRY" = 1 ]; then
        echo "DRY: $misc_tool init $p_misc --slot a --default $misc_def" >&2
    else
        local mi
        mi=$("$misc_tool" init "$p_misc" --slot a --default "$misc_def" 2>&1)
        printf '%s\n' "$mi" | grep '^MISCINIT ' >/dev/null \
            || { printf '%s\n' "$mi" >&2; gk3_fail misc-init "dev=$p_misc" -- "初始化 misc（${p_misc}）失败：$(printf '%s' "$mi" | tail -1)"; return 1; }
        echo "misc：$(printf '%s\n' "$mi" | grep '^MISCINIT ')" >&2
    fi
    [ "$rescue" = yes ] && { gk3__run mkfs.ext4 -q -F -L gk3rescue "$p_resc" || return 1; }

    # ── 写 super（30% → 70%，进度由 gk3-unsparse.py 按块推进）──────────
    gk3_prog 30 write-super
    gk3__write_super "$super_src" "$p_super" || return 1

    gk3_prog 70 write-boot
    gk3__run dd if="$rel/boot.img" of="$p_boota" bs=4M conv=fsync,nocreat status=none || return 1
    gk3__run dd if="$rel/boot.img" of="$p_bootb" bs=4M conv=fsync,nocreat status=none || return 1
    # 写过的节点必须还是块设备 —— 否则上面那些字节进了内存里的一个文件（gk3-unsparse 同理：它会 O_CREAT）
    if [ "$DRY" != 1 ]; then
        local wn
        for wn in "$p_misc" "$p_boota" "$p_bootb" "$p_super"; do
            [ -b "$wn" ] || { gk3_fail node-vanished "dev=$wn" -- "$wn 已经不是块设备了 —— 写进去的东西不在盘上（udev 在写盘期间重建了节点？）"; return 1; }
        done
    fi

    # ── 引导链 ──────────────────────────────────────────────────────────
    # ⚠️ 少了这一步，前面所有东西都写对了，机器照样起不来 —— 这台机器是 UEFI，
    #    内核/dtb/ramdisk 是 ESP 上的【普通文件】，不在 boot 分区里被引导。
    #    （boot_a/boot_b 有内容是为了让 update_engine 的 A/B 流程完整。）
    gk3_prog 80 bootloader
    local mid=$esp_mid
    # 挂载点用 mktemp，不用 /mnt/esp —— 共享的挂载点
    # 会被另一个 shell 里"顺手看一眼"的人 umount 掉，于是这一步静默失败。
    local mnt; mnt=$(mktemp -d)
    gk3__run mount -t vfat "$p_esp" "$mnt" || return 1
    # 整盘清空：ESP 是刚格式化的，没有现成目录 → 用 machine-id
    [ -n "$mid" ] || mid=$(gk3__esp_pick_mid "$mnt" "$mid_fb")
    # ★ 每一个写 ESP 的动作都查结果（2026-09-26 M4b：ESP 写满，cp 失败被忽略，安装照样报告成功）
    esp_fail() { gk3_fail esp-write "esp=$p_esp" -- "写 ESP 失败：$1 —— ESP 空间不够，或者介质出了错（${p_esp}）"; umount "$mnt" 2>/dev/null; rmdir "$mnt" 2>/dev/null; }

    if [ "$DRY" != 1 ]; then
        mkdir -p "$mnt/EFI/BOOT" "$mnt/EFI/systemd" "$mnt/loader/entries" \
                 "$mnt/$mid/android/slot_a" "$mnt/$mid/android/slot_b" || { esp_fail "建目录"; return 1; }
        # ⚠️ 别的 <machine-id> 目录下我们的启动项：default 的通配 *-android-<槽>.conf 会同时匹配它们，
        #   开机走哪个看 systemd-boot 的排序（M4b 那次留下的就是这种局面）。改名停用（systemd-boot 只读 *.conf），不删
        # ★ 2026-10-05（统一启动入口 S9）：只动【直连条目】<32 位十六进制>-android-<槽>.conf。统一启动入口的
        #   gk3boot-android-<槽>[+N].conf / gk3prev-android-<槽>.conf 也匹配这个通配，但它们不是"别的目录的启动项"——
        #   default 通配先命中它们是设计如此（sort-key 0gk3，boot-entry-design §4.2），重新安装不该把它们停用
        #   （安装器怎么处理入口条目是 S10 的事）。规则与 postinstall 的 is_direct_entry、gk3__esp_pick_mid 一致。
        local e
        for e in "$mnt"/loader/entries/*-android-[ab].conf; do
            [ -e "$e" ] || continue
            [[ ${e##*/} =~ ^[0-9a-f]{32}-android-[ab]\.conf$ ]] || continue
            case "${e##*/}" in "$mid"-android-*) continue ;; esac
            mv "$e" "$e.disabled" || { esp_fail "停用 ${e##*/}"; return 1; }
            echo "停用了另一个目录的启动项 ${e##*/} → ${e##*/}.disabled（默认项的通配会同时匹配它）" >&2
        done
        # 不装救援系统时，这个目录下旧的救援启动项与 rescue/ 一并删掉（以前装过救援系统的机器重新安装）：留着的话，
        # 菜单里那两项会用新内核配旧的 initramfs 启动。gk3rescue 分区本身不动
        if [ "$rescue" != yes ]; then
            for e in "$mnt/loader/entries/$mid-rescue.conf" "$mnt/loader/entries/$mid-rescue-b.conf"; do
                [ -f "$e" ] || continue
                rm -f "$e" || { esp_fail "删救援启动项 ${e##*/}"; return 1; }
                echo "删掉了旧的救援启动项 ${e##*/}" >&2
            done
            if [ -d "$mnt/$mid/rescue" ]; then rm -rf "$mnt/$mid/rescue" || { esp_fail "删 $mid/rescue"; return 1; }; fi
        fi
        # ★ S10：统一启动入口的条目（gk3boot-android-<槽>[+N[-M]].conf、.staged、gk3prev-android-<槽>.conf、gk3boot-tools.conf）
        #   与没人再引用的 EFI/gk3boot/<版本>/ 一并删掉（log/ 留着）—— 装完的机器回到"还没有入口"，第一次开机走直连条目，
        #   开机完成时由【装上的这个系统】的 HAL 部署它自己那一版（理由见上面 misc 那段）。留着的话：
        #     · 装的是更旧的版本：新入口去引导旧系统，HAL 要到开机完成才对齐；装的是不带入口的版本（0.7.x）：没人清 boot_streak、
        #       没人祝福条目，入口按 bootloop 处理、每次开机掉进执行端；
        #     · misc 刚初始化过，旧入口记的分派 / 计数都没了，它的状态与 misc 对不上。
        #   整盘清空的 ESP 是新格式化的，不会有这些；只动我们自己的名字（设计稿 §4.9.11 的写盘白名单）
        local gn=0
        for e in "$mnt"/loader/entries/gk3boot-android-* "$mnt"/loader/entries/gk3prev-android-* "$mnt/loader/entries/gk3boot-tools.conf"; do
            [ -f "$e" ] || continue
            rm -f "$e" || { esp_fail "删入口条目 ${e##*/}"; return 1; }
            gn=$((gn + 1))
        done
        if [ -d "$mnt/EFI/gk3boot" ]; then
            for e in "$mnt"/EFI/gk3boot/*; do
                [ -d "$e" ] && [ "${e##*/}" != log ] || continue
                # 还有别的条目（手放的实验条目）指着它就留着 —— 同 HAL 回收目录的规则（Gk3Boot.cpp"删没人引用的目录"）
                grep -qsiF "/EFI/gk3boot/${e##*/}/" "$mnt"/loader/entries/*.conf && continue
                rm -rf "$e" || { esp_fail "删 EFI/gk3boot/${e##*/}"; return 1; }
                gn=$((gn + 1))
            done
        fi
        [ "$gn" = 0 ] || echo "统一启动入口：删掉了 $gn 个旧的条目 / 版本目录（装好的系统第一次开机完成时会部署它自己那一版）" >&2
        # --no-variables 那条路的等价物：固件实际走的是可移动介质回落路径
        # EFI/BOOT/BOOTAA64.EFI（内核带 efi=noruntime，不指望 EFI 启动变量）
        #
        # ⚠️ 双系统时这个位置上原本是【别人的】回落引导（Windows 装机会放一份
        #   bootmgfw.efi 的拷贝）。覆盖前留一份 —— 当年手工迁移就是这么做的
        #   （scripts/archive/esp-migrate-to-internal.sh:120-128 的 .bak-windows）。
        #   systemd-boot 会自己认出 EFI/Microsoft/Boot/bootmgfw.efi 并列进菜单，
        #   所以 Windows 照样能进；留这一份是给"想把一切恢复原样"的人。
        #   （FAT 不分大小写：EFI/Boot/bootaa64.efi 就是这个文件。）
        #   只在第一次装时留：重装不能拿我们自己的 systemd-boot 把原件覆盖掉。
        local f
        for f in EFI/BOOT/BOOTAA64.EFI loader/loader.conf; do
            if [ -f "$mnt/$f" ] && [ ! -e "$mnt/$f.before-gaokun3" ] \
               && ! cmp -s "$mnt/$f" "$sdboot"; then
                cp -p "$mnt/$f" "$mnt/$f.before-gaokun3" || { esp_fail "备份 $f"; return 1; }
                echo "原有的 $f 已备份为 $f.before-gaokun3" >&2
            fi
        done
        # ★ U16 规则 1：只在字节不同时写。BitLocker 量的是固件每次开机加载的这个文件（PCR4），同一份字节重写一遍
        #   本身不改 PCR，但"只在不同时写"让"装过一次之后的重新安装从不动它"成为可以断言的事实（test-apply 看 mtime）
        for f in EFI/BOOT/BOOTAA64.EFI EFI/systemd/systemd-bootaa64.efi; do
            if cmp -s "$sdboot" "$mnt/$f"; then
                echo "$f 已是这一份 systemd-boot，不重写" >&2
            else
                cp "$sdboot" "$mnt/$f" || { esp_fail "${f##*/}"; return 1; }
            fi
        done
        local slot
        for slot in a b; do
            cp "$parts/Image" "$parts/gaokun3.dtb" "$parts/ramdisk.img" "$mnt/$mid/android/slot_$slot/" || { esp_fail "slot_$slot 的内核 / dtb / ramdisk"; return 1; }
            # 文件名是承重的：boot_control HAL 按 *-android-a.conf / *-android-b.conf
            # 改写 loader.conf 的 default（EspSlot.cpp:41-43）；OTA postinstall 只改
            # options 那一行、只往 slot_<后缀>/ 写这三个文件名。改一边就要改另一边。
            cat > "$mnt/loader/entries/$mid-android-$slot.conf" <<ENTRY || { esp_fail "启动项 $mid-android-$slot.conf"; return 1; }
title      crDroid 16.0 (gaokun3) — slot _$slot
version    gaokun3-slot-$slot
sort-key   zandroid$slot
options    $cmdline androidboot.slot_suffix=_$slot
linux      /$mid/android/slot_$slot/Image
devicetree /$mid/android/slot_$slot/gaokun3.dtb
initrd     /$mid/android/slot_$slot/ramdisk.img
ENTRY
        done

        # Android recovery：发版不带（docs/INSTALL.md），自己编了才有。
        # ⚠️★ 启动项默认【不】建：2026-08-20 实测它在本机进复位循环且不留 panic 记录，
        #   15 秒菜单里误选一次就得有人跑到机器旁按电源键。ramdisk 照样铺（无害）。
        if [ -f "$rel/recovery-ramdisk.img" ]; then
            for slot in a b; do
                cp "$rel/recovery-ramdisk.img" "$mnt/$mid/android/slot_$slot/" || { esp_fail "slot_$slot 的 recovery-ramdisk"; return 1; }
                if [ "${GK3_ENABLE_RECOVERY_ENTRY:-0}" = 1 ]; then
                    sed -e "s|^initrd .*|initrd     /$mid/android/slot_$slot/recovery-ramdisk.img|" \
                        -e "s|^title .*|title      Recovery (gaokun3) — slot _$slot|" \
                        -e "s|^version .*|version    gaokun3-recovery-$slot|" \
                        -e "s|^sort-key .*|sort-key   zzrecovery$slot|" \
                        "$mnt/loader/entries/$mid-android-$slot.conf" \
                        > "$mnt/loader/entries/$mid-recovery-$slot.conf" || { esp_fail "recovery 启动项"; return 1; }
                fi
            done
        fi

        # ★ 默认落点是 slot_a；救援系统装了的话它排在前面（sort-key linux1），
        #   但【不设成 default】—— 默认必须是能用的系统。
        #   （命令行版原先把救援设成 default，理由是"默认落点要能远程接入"。
        #    但 boot_control HAL 在 Android 第一次标记启动成功时就会把 default
        #    改写成 *-android-<槽>.conf（EspSlot.cpp:120-172）—— 那个选择只活到
        #    第一次开机，代价却是每个新用户第一次重启落进一个他不认识的系统。）
        # ★ 菜单等待：纯 Android 与双系统统一 5 秒、不写 menu-hidden。
        #   OTA postinstall 只把【早先版本写的】15 改成 5，用户改过的不碰
        local tmo=$GK3_LOADER_TIMEOUT
        [ -f "$mnt/EFI/Microsoft/Boot/bootmgfw.efi" ] && tmo=$GK3_LOADER_TIMEOUT_DUAL
        cat > "$mnt/loader/loader.conf" <<LOADER || { esp_fail "loader.conf"; return 1; }
timeout $tmo
console-mode keep
editor no
default *-android-a.conf
LOADER
        if [ "$rescue" = yes ]; then
            mkdir -p "$mnt/$mid/rescue" || { esp_fail "建救援目录"; return 1; }
            cp "$r_initrd" "$mnt/$mid/rescue/initramfs.img" || { esp_fail "救援系统的 initramfs"; return 1; }
            # ⚠️ 标题用 ASCII：开机菜单由 UEFI 固件的字体画，一般不含中文（原先的"救援系统（Alpine，
            #    全内存）"从没在本机菜单上看过，取稳妥的一侧）。
            # 救援系统与 Android 共用内核与 dtb（见 scripts/live/README.md），
            # 只多一个 initramfs；cmdline 从 Android 那份派生（见 gk3__rescue_cmdline）
            # ★ 两条：一条借 slot_a 的内核、一条借 slot_b 的（v1.0 计划 INST-17）。原先只有 slot_a 那条 ——
            #   OTA 往 slot_a 写进一个起不来的内核，救援也跟着起不来，而那正是要用救援的时候。第二条只是一个
            #   .conf（initramfs 共用一份、内核与 dtb 是 slot_b 本来就有的），不多占 ESP 空间。
            #   文件名 / version 是承重的：OTA postinstall 按 <mid>-rescue*.conf + linux 行认出借了哪个槽的内核，
            #   给它同步 options（gaokun3-ota-postinstall.sh 的"救援条目"一节；OTA-10）。
            # ⓘ 标题只写借谁的内核：原先的 "(runs from RAM)" 不准 —— squashfs 是从救援分区只读 loop 挂的，
            #   只有写入层在内存里（initramfs-init 的 overlay 那段）。
            local rs rf rk
            for rs in a b; do
                rf=$mid-rescue.conf; rk=linux1
                [ "$rs" = a ] || { rf=$mid-rescue-$rs.conf; rk=linux2; }
                cat > "$mnt/loader/entries/$rf" <<RESC || { esp_fail "救援启动项 $rf"; return 1; }
title      gaokun3 rescue (slot $rs kernel)
version    gaokun3-rescue-$rs
sort-key   $rk
linux      /$mid/android/slot_$rs/Image
devicetree /$mid/android/slot_$rs/gaokun3.dtb
initrd     /$mid/rescue/initramfs.img
options    $(gk3__rescue_cmdline "$cmdline")
RESC
            done
        fi
        sync
        for slot in a b; do
            [ -s "$mnt/loader/entries/$mid-android-$slot.conf" ] || { esp_fail "启动项 $mid-android-$slot.conf 是空的"; return 1; }
        done
        if [ "$rescue" = yes ]; then
            for f in "$mid-rescue.conf" "$mid-rescue-b.conf"; do
                [ -s "$mnt/loader/entries/$f" ] || { esp_fail "救援启动项 $f 是空的"; return 1; }
            done
        fi
    else
        echo "DRY: 往 $p_esp 写 systemd-boot、两个 Android 启动项（options=$cmdline …）、内核/dtb/ramdisk" >&2
    fi
    gk3__run umount "$mnt" || { gk3_fail esp-umount "esp=$p_esp" -- "ESP 卸不下来（${p_esp}）—— 写进去的东西可能没落盘"; return 1; }
    rmdir "$mnt" 2>/dev/null || true
    if [ "$DRY" != 1 ]; then
        # ★ 写完逐个核对（cp 没报错不等于文件是全的），从介质读回来
        local -a wl
        mapfile -t wl < <(gk3__esp_files "$mid")
        gk3__verify_on "$p_esp" vfat "${wl[@]}" || return 1
    fi

    # ── systemd-boot 的 EFI 变量（§4.9.3"不是合法值就删"、§4.9.11 整盘清空删残留）────────────────
    # LoaderEntryDefault 优先于 loader.conf 的 default（refs/systemd-v257/src/boot/boot.c:1788-1824）。装完它还在的话，
    # 第一次重启进的就不是新装的 Android：指 live / 救援 / 直连条目时那一项被钉住，指 Windows 时直接进了 Windows。
    # 所以不管值是什么都删（选了 Windows 为默认也删：见 misc 那段 —— 默认系统由入口按 GK3 里的请求去写）。
    # 整盘清空时 LoaderEntryOneShot 一并删（盘上已经没有它能指的东西）。删不掉只警告：盘上的东西都已写好。
    if [ "$DRY" = 1 ]; then
        echo "DRY: 删 EFI 变量 LoaderEntryDefault$([ "$mode" = wipe ] && echo ' 与 LoaderEntryOneShot')（如果有）" >&2
    else
        local vn vv vr
        for vn in LoaderEntryDefault $([ "$mode" = wipe ] && echo LoaderEntryOneShot); do
            vv=$(gk3__loader_var "$vn"); vr=$?
            case "$vr" in
                2) echo "读不到 EFI 变量（efivarfs 不可用）—— $vn 没检查" >&2; break ;;
                1) continue ;;
            esac
            if gk3__loader_var_del "$vn"; then
                echo "删掉了 EFI 变量 $vn（原值 '${vv}'）" >&2
            else
                echo "⚠️ EFI 变量 $vn='${vv}' 删不掉" >&2
                echo "NOTE code=loadervar-stuck name=$vn value=$(gk3__enc "$vv")"
            fi
        done
    fi

    # ── 救援系统 ────────────────────────────────────────────────────────
    if [ "$rescue" = yes ]; then
        gk3_prog 92 write-rescue
        local rmnt; rmnt=$(mktemp -d)
        gk3__run mount "$p_resc" "$rmnt" || return 1
        # ★ 每一步都查（2026-09-27 审查：原先全不查，救援系统坏了要等到真要用它的那天才知道；
        #   重新安装时分区刚被格式化过，写失败等于把一个好的救援系统换成了坏的）
        resc_fail() { gk3_die "写救援分区失败：$1（${p_resc}）"; umount "$rmnt" 2>/dev/null; rmdir "$rmnt" 2>/dev/null; }
        local -a rfl=("gaokun3/rescue.squashfs=$r_squash")
        if [ "$DRY" != 1 ]; then
            mkdir -p "$rmnt/gaokun3" || { resc_fail "建目录"; return 1; }
            cp "$r_squash" "$rmnt/gaokun3/rescue.squashfs" || { resc_fail "rescue.squashfs"; return 1; }
            # ⚠️ WiFi 凭据【不打包进镜像】：安装器把用户当前用的那份复制过去，
            #    这样救援系统一开机就能连上同一个网。见 gk3-wifi 的注释。
            #    来源按优先级：发布目录里放的 → 安装器里刚连上的（gk3_wifi_connect 写的）
            #    → 做 U 盘时放在介质上的。
            local wconf
            if wconf=$(gk3__find_file wpa_supplicant.conf "$rel" "$GK3_RUNDIR" /media/gk3/gaokun3); then
                install -Dm600 "$wconf" "$rmnt/gaokun3/wpa_supplicant.conf" || { resc_fail "WiFi 配置"; return 1; }
                rfl+=("gaokun3/wpa_supplicant.conf=$wconf")
                echo "救援系统的 WiFi 配置取自 $wconf" >&2
            else
                echo "警告：没有 WiFi 配置可带给救援系统 —— 它开机后连不上网，只能在机器旁操作" >&2
            fi
            # ★ ssh 公钥同理：公开的 live 镜像不带任何人的公钥，所以从它装出来的救援系统本来
            #   【远程进不去】—— 而远程接入正是救援系统存在的意义。来源按优先级：发布目录里放的
            #   → 安装 U 盘上用户放的 → 正在跑的这个系统自己的（私人构建的镜像带了 --ssh-key）。
            #   救援系统开机时把 /media/gk3/gaokun3/authorized_keys 并进 /root/.ssh（overlay 里的 gk3-ssh-keys）。
            local akeys=""
            akeys=$(gk3__find_file authorized_keys "$rel" /media/gk3/gaokun3) \
                || { [ -s /root/.ssh/authorized_keys ] && akeys=/root/.ssh/authorized_keys; } || true
            if [ -n "$akeys" ]; then
                install -Dm600 "$akeys" "$rmnt/gaokun3/authorized_keys" || { resc_fail "ssh 公钥"; return 1; }
                rfl+=("gaokun3/authorized_keys=$akeys")
                echo "救援系统的 ssh 公钥取自 ${akeys}（$(grep -c '^ssh-\|^ecdsa-' "$akeys") 把）" >&2
            else
                echo "警告：没有 ssh 公钥可带给救援系统 —— 只能在机器旁登录（放一份到 U 盘的 gaokun3/authorized_keys）" >&2
            fi
            sync
        fi
        gk3__run umount "$rmnt" || { gk3_die "救援分区卸不下来（${p_resc}）"; return 1; }
        rmdir "$rmnt" 2>/dev/null || true
        if [ "$DRY" != 1 ]; then
            gk3__verify_on "$p_resc" ext4 "${rfl[@]}" || return 1
        fi
    fi

    rm -rf "$parts"
    gk3_prog 100 done
    return 0
}

# 在几个目录里按顺序找一个文件，找到就打印完整路径。空目录参数跳过。
gk3__find_file() {
    local name=$1 d; shift
    for d in "$@"; do
        [ -n "$d" ] || continue
        [ -f "$d/$name" ] && { echo "$d/$name"; return 0; }
    done
    return 1
}

# 装进救援分区的 squashfs 去哪找。$1=发布目录（可空）。gk3_apply、gk3_release_info、install-gaokun3.sh 共用这一条规则。
#   1. 发布目录里的 rescue.squashfs（命令行安装：从 Release 下载的，release-installer.sh 附带）
#   2. U 盘上专门放的救援镜像 gaokun3/install-rescue/rescue.squashfs（build-usb.sh --rescue-squashfs）
#   3. U 盘上正在跑的这个：build-usb.sh 把 live 镜像就叫 gaokun3/rescue.squashfs
#   4. Windows 安装包的 GK3LIVE 分区上正在跑的这个：build-bundle.sh 把它叫 gaokun3/live.squashfs
#      ★ v1.0 计划 INST-6：原先只按 rescue.squashfs 这个名字找，从 Windows 开始的安装（免 U 盘）永远装不上救援系统。
#        不在 Windows 安装包里另放一份（又是约 200 MiB 要下载、要拷进 GK3LIVE），就用正在跑的这一份。
#   ⚠️ 3、4 是 live 镜像本身（带图形安装器、开机 tty1 就起它），不是 rescue profile（只有 ssh 与工具）——
#      发布版本来就不带 rescue profile（release-installer.sh 的注释：它在真机上一次没起过），所以这是常态。
#   装进救援分区时一律改名叫 rescue.squashfs（救援条目的 gk3.squash= 指的是它，见 gk3__rescue_cmdline）。
gk3__find_rescue_squashfs() {
    gk3__find_file rescue.squashfs "${1:-}" /media/gk3/gaokun3/install-rescue /media/gk3/gaokun3 \
        || gk3__find_file live.squashfs /media/gk3/gaokun3
}

# 写 super。输入是 .zst（发版产物）或 super.img（sparse 或 raw）。
# ⚠️ 不用 simg2img：它喂管道会失败（理由见 gk3-unsparse.py 文件头），
#    而 .zst 要么走管道、要么先落一份临时文件 —— 12 GiB 的 tmpfs 我们没有。
gk3__write_super() {
    local src=$1 dst=$2
    if [ "${GK3_DRYRUN:-0}" = 1 ]; then echo "DRY: 展开 $src → ${dst}（gk3-unsparse.py）"; return 0; fi
    local us="$GK3_LIBDIR/gk3-unsparse.py"
    case "$src" in
        *.zst)
            # 两段的退出码都要看（只看管道尾巴会漏）。
            #    下载断在一半的 .zst → zstd 报错且提前 EOF → unsparse 也报错；
            #    反过来 unsparse 先死时 zstd 会吃 SIGPIPE。
            #    --long=31 只是放宽解压时允许的窗口上限，不多占内存（release.sh 用 -19 --long 压）。
            zstd -dc --long=31 "$src" | python3 "$us" --progress 30 40 "$dst"
            local -a rc=( "${PIPESTATUS[@]}" )
            if [ "${rc[0]}" != 0 ] || [ "${rc[1]}" != 0 ]; then
                gk3_fail super-write "zstd=${rc[0]}" "unsparse=${rc[1]}" -- "super 写入失败（zstd=${rc[0]} gk3-unsparse=${rc[1]}）—— .zst 下载完整吗？"
                return 1
            fi ;;
        *)
            if head -c4 "$src" | od -An -tx1 | tr -d ' \n' | grep -qi '^3aff26ed$'; then
                python3 "$us" --progress 30 40 "$dst" < "$src" || { gk3_fail super-write -- "super 展开失败"; return 1; }
            else
                echo "super.img 不是 sparse 格式，直接写" >&2
                dd if="$src" of="$dst" bs=4M conv=fsync,nocreat status=none || { gk3_fail super-write -- "dd super 失败"; return 1; }
            fi ;;
    esac
    # ★ 判格式不判校验和：偏移 4096 处必须是 LP geometry 魔数。
    #   ⚠️ 命令行版一直有这道检查（install-gaokun3.sh 原 :131-132），这个库里漏了。
    #      dd 了 sparse 镜像的 super "看着字节都对"却没有 LP 元数据，
    #      Android 首阶段挂载失败后主动 reboot()，不留任何日志（见 docs/dev/android-bringup.md）。
    local lp; lp=$(dd if="$dst" bs=1 skip=4096 count=4 2>/dev/null | od -An -tx1 | tr -d ' \n')
    if [ "$lp" != "67446c61" ]; then
        gk3_fail super-bad-lp -- "super 偏移 4096 处不是 LP geometry 魔数（读到 '${lp}'）—— 写进去的不是一份能用的 super"
        return 1
    fi
    echo "super：LP geometry 魔数正确" >&2
}

# 救援系统的 cmdline：从 Android 那份（boot.img 里的 BOARD_KERNEL_CMDLINE）派生，
# 去掉只给 Android 用的参数，再加上救援 initramfs 自己的。
# ★ 不另抄一份：原先手抄的那份缺 usbhid.quirks（键盘盖）和 himax disable_pressure=0 ——
#   和 Android 那份（TODO B15）是同一种漂移。
gk3__rescue_cmdline() {
    local keep
    keep=$(printf '%s\n' "$1" | tr ' ' '\n' \
           | grep -v -e '^androidboot\.' -e '^init=' -e '^firmware_class\.path=' -e '^$' \
           | tr '\n' ' ')
    echo "${keep}loglevel=3 panic=10 gk3.squash=/gaokun3/rescue.squashfs"
}

# 解析分区节点，解析不出来就直接失败。
# dry-run 时分区还不存在，回一个明显是占位的名字，好让打印出来的命令可读。
gk3__need_part() {
    local disk=$1 want=$2 mode=$3 path
    if [ "${GK3_DRYRUN:-0}" = 1 ]; then echo "<${want}分区>"; return 0; fi
    path=$(gk3__bylabel "$disk" "$want")
    if [ -z "$path" ]; then
        gk3_fail part-missing "name=$want" "disk=$disk" -- "分区 $want 没解析出来（$disk 上找不到这个 PARTLABEL）"; return 1
    fi
    # ⚠️ 分区节点的出现是异步的（udev / devtmpfs），刚写完分区表时它可能还没到。
    #    第一版在这里直接判死，结果 loop 设备实测必然失败 —— 而 lsblk 明明
    #    看得见分区。**"还没出现"和"不存在"是两回事**：等它，别判它死。
    local i=0
    while [ ! -b "$path" ] && [ $i -lt 50 ]; do
        [ $i -eq 0 ] && command -v udevadm >/dev/null && udevadm settle --timeout=5 2>/dev/null
        sleep 0.2; i=$((i+1))
    done
    if [ ! -b "$path" ]; then
        gk3_fail part-node-timeout "dev=$path" -- "$path 等了 10 秒还不是块设备 —— 分区表写下去了但内核没认"; return 1
    fi
    gk3__node_matches "$disk" "$path" || return 1
    echo "$path"
}

# 从一行 key=value 里取值
gk3__f() {
    printf '%s\n' "$1" | tr ' ' '\n' | sed -n "s/^$2=//p" | head -1
}

# 按 PARTLABEL 找分区节点。⚠️ 不用 /dev/disk/by-partlabel —— 救援系统里
# 没有 udev 规则时那个目录可能不存在；直接问 sgdisk 才是确定的。
gk3__bylabel() {
    local disk=$1 want=$2 n
    for n in $(sgdisk -p "$disk" 2>/dev/null | awk '/^ *[0-9]+ /{print $1}'); do
        if [ "$(sgdisk -i "$n" "$disk" 2>/dev/null | grep "^Partition name:" | cut -d"'" -f2)" = "$want" ]; then
            gk3_partpath "$disk" "$n"; return 0
        fi
    done
    echo ""
}

# ── NTFS：Windows 休眠 / 快速启动 / 没关干净 ─────────────────────────────────
# hiberfil.sys 开头是 hibr / HIBR ⇒ Windows 处于休眠（快速启动的"关机"也是休眠）。ntfscat 只读打开卷，不写任何东西；
# 判据与 ntfs-3g 自己的休眠检查相同
gk3__ntfs_hibernated() {
    case "$(ntfscat "$1" /hiberfil.sys 2>/dev/null | head -c 4 | od -An -tx1 | tr -d ' \n')" in
        68696272|48494252) return 0 ;;    # "hibr" / "HIBR"
    esac
    return 1
}

# 看一个发布目录里有什么（默认：安装 U 盘上 build-usb.sh 放 payload 的位置）。
#   RELEASE dir=… boot=yes|no super=zst|img|no sha256=yes|no rescue=yes|no version=… super_mib=…
#   rescue=yes：rescue.squashfs 与 initramfs.img 都找得到（发布目录里，或启动介质上）
#   version：从校验清单里那个 OTA zip 的名字取（crDroidAndroid-16.0-<日期>-gaokun3-v12.11）
gk3_release_info() {
    local d=${1:-/media/gk3/gaokun3/payload} boot=no super=no sha=no rescue=no ver="" smib=0
    [ -f "$d/boot.img" ] && boot=yes
    if   [ -f "$d/super.img.zst" ]; then super=zst; smib=$(( $(stat -c%s "$d/super.img.zst" 2>/dev/null || wc -c < "$d/super.img.zst") / 1048576 ))
    elif [ -f "$d/super.img" ];     then super=img; smib=$(( $(stat -c%s "$d/super.img" 2>/dev/null || wc -c < "$d/super.img") / 1048576 )); fi
    if [ -f "$d/install-artifacts.sha256" ]; then
        sha=yes
        ver=$(awk '{sub(/^\*/, "", $2)} $2 ~ /\.zip$/ {sub(/\.zip$/, "", $2); print $2; exit}' "$d/install-artifacts.sha256")
    fi
    gk3__find_rescue_squashfs "$d" >/dev/null \
        && gk3__find_file initramfs.img "$d" /media/gk3/gaokun3 >/dev/null && rescue=yes
    echo "RELEASE dir=$(gk3__enc "$d") boot=$boot super=$super sha256=$sha rescue=$rescue version=$(gk3__enc "${ver:-?}") super_mib=$smib"
}

# 看一个现有 ESP 的状况（只读挂载）。双系统之前界面要用它判断"装不装得下"，
# 并且在用户往下走【之前】就写明原因 —— 而不是等 gk3_apply 在动盘前一刻才拒绝。
#   ESP part=… size_mib=… free_mib=… need_mib=150 windows=yes|no gaokun3=yes|no mountable=yes|no \
#       small=yes|no bootaa64=none|sdboot|other bitlocker=yes|no hibernated=yes|no|unknown \
#       entry=<版本>|none entry_mode=action|observe|none entry_state=<a+3,b…>|none staged=yes|no prev=yes|no \
#       loader_default=<值>|none|unknown boot_entries=<N>|unknown
#   windows=yes：EFI/Microsoft/Boot/bootmgfw.efi 在 —— 装完之后 systemd-boot 会把它列进菜单
#   gaokun3=yes：上面已经有我们的启动项（重装 / 已经装过）
#   ── 双系统与统一启动入口字段（见 tools/gk3boot/README.md）──
#   small=yes：分区小于 GK3_ESP_MIN_SIZE_MIB（100 MiB 那种），双系统明确不装
#   bootaa64：EFI/BOOT/BOOTAA64.EFI 与 live 自带的 systemd-boot 比 —— none 没有 / sdboot 同一份字节 / other 不同
#     （other + bitlocker=yes ⇒ 安装会换掉它，界面要用户确认拿到恢复密钥）
#   bitlocker / hibernated：同一块盘上的 Windows 卷（gk3__win_state）；hibernated=yes ⇒ 不许写 ESP
#   entry*：统一启动入口在这个 ESP 上的样子（只报告；重新安装会把它们删掉，由装好的系统自己部署，见 gk3_apply）
#     entry = 现役 gk3boot-android-* 条目里的版本；entry_state = 各条目去掉前缀 / .conf 后的名字（a+3 = 槽 a、还剩 3 次计数）
#   loader_default：EFI 变量 LoaderEntryDefault（none = 没设 = 默认 Android；unknown = efivarfs 读不到）
#   boot_entries：固件里 Boot#### 的个数（efibootmgr；没有它 ⇒ unknown）
# ★ Windows 默认建的 ESP 只有 100 MiB，放不下 GK3_ESP_NEED_MIB —— 这是双系统最常见的
#   "装不了"，不是边角情况（scripts/live/test-apply.sh 的 C 组有这条）。
gk3_esp_info() {
    local part=$1 m free size win=no ours=no small=no aa=none sd ver=none emode=none est="" staged=no prev=no e n ld be ws
    [ -b "$part" ] || { gk3_fail not-block "dev=$part" -- "不是块设备：$part"; return 1; }
    size=$(( $(blockdev --getsize64 "$part" 2>/dev/null || echo 0) / 1048576 ))
    [ "$size" -lt "$GK3_ESP_MIN_SIZE_MIB" ] && small=yes
    ld=$(gk3__loader_var LoaderEntryDefault); case $? in 0) ld=$(gk3__enc "$ld") ;; 1) ld=none ;; *) ld=unknown ;; esac
    be=unknown
    command -v efibootmgr >/dev/null 2>&1 && be=$(efibootmgr 2>/dev/null | grep -cE '^Boot[0-9A-Fa-f]{4}')
    m=$(mktemp -d)
    if ! mount -o ro -t vfat "$part" "$m" 2>/dev/null; then
        rmdir "$m"; echo "ESP part=$part size_mib=$size free_mib=0 need_mib=$GK3_ESP_NEED_MIB windows=no gaokun3=no mountable=no small=$small bootaa64=none bitlocker=no hibernated=unknown entry=none entry_mode=none entry_state=none staged=no prev=no loader_default=$ld boot_entries=$be"
        return 0
    fi
    free=$(df -m "$m" | awk 'NR==2{print $4}')
    # vfat 挂载本来就不分大小写，不用自己列大小写组合
    [ -f "$m/EFI/Microsoft/Boot/bootmgfw.efi" ] && win=yes
    ls "$m"/loader/entries/*-android-*.conf >/dev/null 2>&1 && ours=yes
    if [ -f "$m/EFI/BOOT/BOOTAA64.EFI" ]; then
        aa=other; sd=$(gk3__sdboot) && cmp -s "$m/EFI/BOOT/BOOTAA64.EFI" "$sd" && aa=sdboot
    fi
    for e in "$m"/loader/entries/gk3boot-android-*.conf; do
        [ -f "$e" ] || continue
        n=${e##*/}; n=${n#gk3boot-android-}; est="$est,${n%.conf}"
        ver=$(sed -n 's/^version[[:space:]]*gk3boot-//p' "$e" | head -1 | tr -d '\r ')
        if grep -qE '^options[[:space:]].*gk3\.observe=1([[:space:]]|$)' "$e"; then emode=observe; else emode=action; fi
    done
    ls "$m"/loader/entries/gk3boot-android-*.conf.staged >/dev/null 2>&1 && staged=yes
    ls "$m"/loader/entries/gk3prev-android-*.conf >/dev/null 2>&1 && prev=yes
    umount "$m"; rmdir "$m"
    est=${est#,}
    ws=$(gk3__win_state "$part")
    echo "ESP part=$part size_mib=$size free_mib=${free:-0} need_mib=$GK3_ESP_NEED_MIB windows=$win gaokun3=$ours mountable=yes" \
         "small=$small bootaa64=$aa bitlocker=$(gk3__f "$ws" bitlocker) hibernated=$(gk3__f "$ws" hibernated)" \
         "entry=$(gk3__enc "${ver:-none}") entry_mode=$emode entry_state=${est:-none} staged=$staged prev=$prev loader_default=$ld boot_entries=$be"
}

# ── 删除分区 ────────────────────────────────────────────────────────────────
# 安装方式页最后一项"删除分区"：腾出空间（例如删掉 Windows 恢复分区、华为一键恢复分区，"磁盘管理"删不了它们）。
# 每次删除单独确认、立即执行。缩小分区与新建、格式化不在这里做：空间在 Windows 里用"压缩卷"腾出。
#
#   gk3_part_delete <分区>
# 成功时 stdout 一行 RESULT op=delete part=…；失败走 gk3_die（盘没动过时会说）。
#
# ★ 闸（gk3__edit_guard）：块设备；不是 ESP（删了它，盘上所有系统都无法启动 —— 要重建 ESP
#   就用整盘清空）；没挂着（首先是安装器所在的那个分区）；动手之前把分区表备份到介质。
# ★ 删除【不】抹文件系统签名：分区表备份还原回去，数据就还在（Windows 的删除也是这样）。
GK3_ESP_GUID=C12A7328-F81F-11D2-BA4B-00A0C93EC93B

gk3__edit_guard() {    # $1=分区 → 设 GK3_E_DISK / GK3_E_NUM
    local part=$1
    [ -b "$part" ] || { gk3_fail not-block "dev=$part" -- "不是块设备：$part"; return 1; }
    if [ "$(blkid -p -o value -s PART_ENTRY_TYPE "$part" 2>/dev/null | tr 'a-f' 'A-F')" = "$GK3_ESP_GUID" ]; then
        gk3_fail edit-esp -- "$part 是 EFI 系统分区：动它，盘上所有系统都起不来（要重建 ESP，用整盘清空）"; return 1
    fi
    if findmnt -rn -S "$part" >/dev/null 2>&1; then
        gk3_fail edit-mounted -- "$part 正挂着（安装器是不是就从它上面跑的？）—— 不动它"; return 1
    fi
    GK3_E_DISK=/dev/$(lsblk -no PKNAME "$part" 2>/dev/null | head -1)
    GK3_E_NUM=$(cat "/sys/class/block/$(basename "$part")/partition" 2>/dev/null)
    [ -b "$GK3_E_DISK" ] && [ -n "$GK3_E_NUM" ] || { gk3_fail part-unknown "part=$part" -- "认不出 $part 属于哪块盘的第几个分区"; return 1; }
    # MBR 盘：删除分区项是 sgdisk 写 GPT，一定失败；
    # 读不出的盘不动（设计稿 S1 / S2）
    gk3__table_guard "$GK3_E_DISK"
}

# 动盘之前备份分区表到介质（出事一条命令还原）。gk3_apply 与删除分区共用这一份；路径留在 GK3_GPT_BK。
# ⚠️ "还原：sgdisk --load-backup=" 这几个字界面在认（screens_finish.dart 的失败页），别改
gk3__gpt_backup() {    # $1=盘 $2=标签
    local disk=$1 dir=/media/gk3/gaokun3
    mount -o remount,rw /media/gk3 2>/dev/null || true
    [ -d "$dir" ] && [ -w "$dir" ] || dir=/tmp
    GK3_GPT_BK="$dir/gpt-before-$2-$(basename "$disk")-$(date +%Y%m%d-%H%M%S).bin"
    if sgdisk --backup="$GK3_GPT_BK" "$disk" >/dev/null 2>&1; then
        sync
        echo "分区表备份：${GK3_GPT_BK}（还原：sgdisk --load-backup=$GK3_GPT_BK ${disk}）" >&2
        # 2026-09-27 审查：落到 /tmp 的备份在内存里，重启就没了 —— 说清楚，别让人以为有还原点
        [ "$dir" != /tmp ] || echo "警告：安装介质写不进去，分区表只备份到了内存里（${GK3_GPT_BK}），重启就没了 —— 要留着就先把它拷走" >&2
    else
        GK3_GPT_BK=""
        echo "警告：分区表备份失败（继续，但出事就没有还原点了）" >&2
    fi
}

gk3__settle() { partprobe "$1" 2>/dev/null || true; command -v udevadm >/dev/null && udevadm settle --timeout=5 2>/dev/null; sleep 1; }

gk3__wait_node() {     # 分区节点是异步出现的："还没出现"和"不存在"是两回事（gk3__need_part 的注释）
    local i=0; while [ ! -b "$1" ] && [ $i -lt 50 ]; do sleep 0.2; i=$((i+1)); done; [ -b "$1" ] || return 1
    local d; d=$(gk3__disk_of "$1") && gk3__node_matches "$d" "$1"
}

# 分区所在的整块盘（/dev/nvme0n1p5 → /dev/nvme0n1，/dev/loop3p2 → /dev/loop3），按 sysfs 找，不按名字猜
gk3__disk_of() {
    local n; n=$(readlink -f "/sys/class/block/${1##*/}/.." 2>/dev/null) || return 1
    [ -b "/dev/${n##*/}" ] && echo "/dev/${n##*/}"
}

# ⚠️ 节点存在不等于它对：partprobe 失败时（有分区被占着），内核还拿着【旧】分区表，同号的节点指着旧的起点 ——
#   往它上面 mkfs / dd 就写进了别处（2026-09-27 审查）。拿内核看到的起点 / 大小（sysfs，512 字节为单位）
#   对一遍盘上的分区表（sgdisk，逻辑扇区为单位）。$1=盘 $2=分区节点
gk3__node_matches() {
    local disk=$1 path=$2 n ss kst ksz st en
    n=${path##*[!0-9]}
    ss=$(( $(blockdev --getss "$disk" 2>/dev/null || echo 512) / 512 ))
    kst=$(cat "/sys/class/block/${path##*/}/start" 2>/dev/null); ksz=$(cat "/sys/class/block/${path##*/}/size" 2>/dev/null)
    st=$(sgdisk -i "$n" "$disk" 2>/dev/null | awk '/^First sector:/{print $3}')
    en=$(sgdisk -i "$n" "$disk" 2>/dev/null | awk '/^Last sector:/{print $3}')
    [ -n "$kst" ] && [ -n "$st" ] || return 0          # 读不到就不拦（不在这里制造新的失败）
    if [ "$kst" != $(( st * ss )) ] || [ "$ksz" != $(( (en - st + 1) * ss )) ]; then
        gk3_fail kernel-stale "dev=$path" -- "内核看到的 ${path}（起点 ${kst}、${ksz} 扇区）与盘上的分区表（${st}–${en}）对不上 —— 新分区表没生效（有分区被占着？）。重启后再来"
        return 1
    fi
}

gk3_part_delete() {
    local part=$1
    gk3__edit_guard "$part" || return 1
    gk3_prog 10 gpt-backup
    gk3__gpt_backup "$GK3_E_DISK" delete
    gk3_prog 50 part-delete "part=$part"
    sgdisk -d "$GK3_E_NUM" "$GK3_E_DISK" >/dev/null 2>&1 || { gk3_fail gpt-rewrite -- "删分区项失败（分区表备份见上）"; return 1; }
    gk3__settle "$GK3_E_DISK"
    gk3_prog 100 done
    echo "RESULT op=delete part=$part"
}

# ── 写盘进程脱离界面（v1.0 计划 GUI-11）──────────────────────────────────────
# 问题：图形安装器起的子进程全在 gk3-installer.service 的 cgroup 里（cage → gk3-installer-session → Flutter → bash）。
#   Flutter 一崩，cage 退出，服务停下，systemd 按默认的 KillMode=control-group 把整个 cgroup 杀掉 ——
#   正在写 super、正在缩 NTFS 的那个 bash 一起死，留下半块盘。
# 做法：写盘的那一次调用放进一个【独立的临时单元】（systemd-run），输出写进文件、结束时写状态文件；
#   界面只是"跟着读"。界面死了，单元照样跑完；界面重新起来，按 id 接着跟（gk3_job_status 找得到它）。
#
#   gk3_job_run <函数> [参数…]    = gk3_job_start + gk3_job_follow。★ 前端要做的只有一件事：写盘的调用把
#                                   `… "$0" gk3_apply …` 换成 `… "$0" gk3_job_run gk3_apply …`。stdout / stderr / 退出码
#                                   与直接调用逐行相同（JOB 记录打在 stderr 上，不混进 stdout 的记录流）
#   gk3_job_start <函数> [参数…]  起单元，立刻返回。stdout：JOB id=… state=running mode=systemd-run|setsid unit=…
#   gk3_job_follow <id>           转发这个 job 的输出（从头），等它结束，退出码 = job 的退出码；
#                                   job 没写状态就没了（被杀 / 机器出错）⇒ 125，stderr 有一行 `!! …`
#   gk3_job_status [id]           每个 job 一行：JOB id=… fn=… state=running|done|lost rc=… mode=… unit=…
#
# ★ systemd-inhibit 套在【单元里面】：锁跟着写盘进程活，不跟着界面活（ShellBackend 自己套的那层在界面死时就放掉了）。
#   前端那一层可以留着（两把锁无害），也可以去掉。
# ★ 单元拿不到调用者的环境：GK3_* 一律用 --setenv 带过去（GK3_DRYRUN、GK3_MACHINE_ID、GK3_SDBOOT……）；
#   工作目录用 --working-directory 带过去（发布目录一般是绝对路径，带上更稳）。
# ★ 不是 systemd 当 PID 1 的环境（容器、别的 live）：退回 setsid -f，只脱离进程组与终端 —— 那样界面所在的 cgroup
#   被整个杀掉时它仍会被连带，stderr 上会说一句。判据与 sd_booted(3) 相同：/run/systemd/system/ 在不在
#   （refs/systemd-v257/man/sd_booted.xml:55）。
# ★ 结束时把 job 的输出另存一份到启动介质的 gaokun3/diag/job-<id>.log：/run 在内存里，重启就没了；
#   界面要是在写盘途中死掉，installer.log 里也就没有后半段。
#   systemd-run 的选项都在 refs/systemd-v257/man/systemd-run.xml 里（--unit :121、--service-type :238、
#   --working-directory :275、--setenv :297、--quiet :376、--collect :477）；Type=exec 见 systemd.service.xml:159-182。
# ⓘ 测试用：GK3_SYSTEMD_RUN=<假的 systemd-run>（同时当作"systemd 在跑"）。
GK3_JOBDIR=${GK3_JOBDIR:-$GK3_RUNDIR/jobs}
GK3_INHIBIT_WHAT=shutdown:sleep:handle-power-key:handle-suspend-key:handle-hibernate-key:handle-lid-switch

# job 单元里跑的那段：source 库、跑、写状态、存一份到介质。$1=job 目录 $2=库 $3…=命令
# shellcheck disable=SC2016
GK3__JOB_BODY='d=$1; lib=$2; shift 2
echo $$ > "$d/pid"
if . "$lib" >/dev/null 2>"$d/err"; then
    "$@" > "$d/out" 2>> "$d/err"; rc=$?
else
    echo "!! 加载不了 $lib" >> "$d/err"; rc=127
fi
echo "$rc" > "$d/rc.tmp" && mv -f "$d/rc.tmp" "$d/rc"
gk3__job_keep "$d" 2>/dev/null
exit "$rc"'

gk3_job_start() {
    [ $# -ge 1 ] || { gk3_fail usage -- "用法：gk3_job_start <函数> [参数…]"; return 1; }
    local id d mode unit="" sr v
    local -a pre=() env=()
    id=$(date +%Y%m%d-%H%M%S)-$$-$RANDOM
    d=$GK3_JOBDIR/$id
    mkdir -p "$d" || { gk3_fail job-start -- "建不了 $d"; return 1; }
    printf '%s\n' "$1" > "$d/fn"
    printf '%s\n' "$PWD" > "$d/cwd"
    date +%s > "$d/started"
    # 写盘期间不许关机 / 睡 / 按键关机（B5 的第三层）。先试拿一次：logind 没在跑时 systemd-inhibit 直接失败
    if command -v systemd-inhibit >/dev/null 2>&1 \
       && systemd-inhibit --what="$GK3_INHIBIT_WHAT" --who="gaokun3 installer" --why=probe true >/dev/null 2>&1; then
        pre=("$(command -v systemd-inhibit)" --what="$GK3_INHIBIT_WHAT" --who="gaokun3 installer" --why="writing the disk ($1)" --mode=block)
    else
        echo "⚠️ 拿不到 systemd-inhibit（logind 没在跑？）—— 照样执行；电源键与合盖仍由 logind.conf.d 屏蔽、睡眠 target 已 mask" >&2
    fi
    sr=${GK3_SYSTEMD_RUN:-systemd-run}
    if command -v "$sr" >/dev/null 2>&1 && { [ -n "${GK3_SYSTEMD_RUN:-}" ] || [ -d /run/systemd/system ]; }; then
        mode=systemd-run; unit=gk3-job-$id
        while IFS='=' read -r v _; do env+=(--setenv="$v"); done < <(env | grep '^GK3_[A-Za-z0-9_]*=' || true)
        # 脚本留给 bash 展开：systemd 默认会把 $$ 变成 $，导致 pid 文件无效、跟读误报 job-lost。
        "$sr" --quiet --collect --expand-environment=no --unit="$unit" --service-type=exec --description="gaokun3 installer: $1" \
              --working-directory="$PWD" "${env[@]}" \
              ${pre[@]+"${pre[@]}"} "$(command -v bash)" -c "$GK3__JOB_BODY" gk3-job "$d" "$GK3_LIBDIR/installer-lib.sh" "$@" >&2 \
            || { echo "systemd-run 起不来 $unit" > "$d/start-error"; gk3_fail job-start -- "systemd-run 起不来写盘单元（${unit}）"; return 1; }
    else
        mode=setsid
        echo "⚠️ 不是 systemd 系统（或没有 systemd-run）：写盘进程只用 setsid 脱离 —— 界面所在的 cgroup 被整个杀掉时它仍会被连带" >&2
        command -v setsid >/dev/null 2>&1 || { gk3_fail tool-missing tool=setsid -- "没有 setsid"; return 1; }
        setsid -f ${pre[@]+"${pre[@]}"} bash -c "$GK3__JOB_BODY" gk3-job "$d" "$GK3_LIBDIR/installer-lib.sh" "$@" \
            </dev/null >/dev/null 2>&1 || { gk3_fail job-start -- "setsid 起不来写盘进程"; return 1; }
    fi
    printf '%s\n' "$mode" > "$d/mode"; printf '%s\n' "${unit:--}" > "$d/unit"
    echo "JOB id=$id state=running mode=$mode unit=${unit:--}"
}

gk3__job_state() {   # $1=job 目录 → running|done|lost
    local d=$1 pid
    [ -f "$d/rc" ] && { echo done; return; }
    [ -f "$d/start-error" ] && { echo lost; return; }
    pid=$(cat "$d/pid" 2>/dev/null)
    if [ -z "$pid" ]; then
        # 刚起、还没写 pid：给 30 秒，过了还没有就算没起来
        [ $(( $(date +%s) - $(cat "$d/started" 2>/dev/null || echo 0) )) -lt 30 ] && echo running || echo lost
        return
    fi
    kill -0 "$pid" 2>/dev/null && { echo running; return; }
    [ -f "$d/rc" ] && echo done || echo lost      # 进程刚好在这两步之间写完
}

gk3_job_status() {
    local d id
    for d in "$GK3_JOBDIR"/${1:-*}; do
        [ -d "$d" ] && [ -f "$d/fn" ] || continue
        id=${d##*/}
        echo "JOB id=$id fn=$(gk3__enc "$(cat "$d/fn")") state=$(gk3__job_state "$d") rc=$(cat "$d/rc" 2>/dev/null || echo -)" \
             "mode=$(cat "$d/mode" 2>/dev/null || echo -) unit=$(cat "$d/unit" 2>/dev/null || echo -)"
    done
}

# 跟着读：只转发【完整的行】（写盘进程写到一半的那行等它写完），stdout 对 stdout、stderr 对 stderr。
# 用 python3（预检本来就要它）：bash 按字节偏移读文件要绕很多弯，而这里错一个字节就是一条被切开的协议记录。
gk3_job_follow() {
    local d=$GK3_JOBDIR/${1:-}
    [ -n "${1:-}" ] && [ -f "$d/fn" ] || { gk3_fail job-missing "id=${1:-}" -- "没有这个 job：${1:-（没给 id）}"; return 1; }
    # 已转发到哪个字节：记在跟读者自己的临时目录里 —— 同一个 job 可以有几个人同时跟（界面崩了又起来），各记各的
    local st od rc
    od=$(mktemp -d) || return 1
    while :; do
        st=$(gk3__job_state "$d")
        python3 - "$d" "$st" "$od" <<'PY' || { rc=$?; rm -rf "$od"; return "$rc"; }
import os, sys
d, st, od = sys.argv[1], sys.argv[2], sys.argv[3]
final = st != "running"
for name, out in (("out", sys.stdout), ("err", sys.stderr)):
    off_f = os.path.join(od, name)
    try: off = int(open(off_f).read() or 0)
    except (OSError, ValueError): off = 0
    try:
        with open(os.path.join(d, name), "rb") as f:
            f.seek(off); data = f.read()
    except OSError:
        data = b""
    if not final:
        cut = data.rfind(b"\n") + 1      # 只吐完整的行
        data = data[:cut]
    elif data and not data.endswith(b"\n"):
        data += b"\n"; off -= 1          # 最后一行没换行：补一个（off 少记一个，免得下次重读时错位）
    if data:
        out.buffer.write(data); out.flush()
        open(off_f, "w").write(str(off + len(data)))
sys.exit(0)
PY
        case "$st" in
            done) rm -rf "$od"; return "$(cat "$d/rc")" ;;
            lost) rm -rf "$od"; gk3_fail job-lost "id=$1" touched=yes -- "写盘进程（job ${1}）没写完成状态就没了（被杀？机器出错？）—— 盘可能只写了一半；日志在 $d/"; return 125 ;;
        esac
        sleep "${GK3_JOB_POLL:-0.3}"
    done
}

gk3_job_run() {
    local rec id
    rec=$(gk3_job_start "$@") || return $?
    echo "$rec" >&2
    id=$(gk3__f "$rec" id)
    gk3_job_follow "$id"
}

# job 结束时存一份到介质（由单元里那段调用；介质只读挂着就临时改成读写，写完改回去 —— 与 gk3-diag 同一个规矩）
gk3__job_keep() {
    local d=$1 m=/media/gk3 was f
    [ -d "$m/gaokun3" ] || return 0
    was=$(awk -v m="$m" '$2 == m { split($4, o, ","); r = o[1] } END { print r }' /proc/mounts 2>/dev/null)
    mount -o remount,rw "$m" 2>/dev/null || return 0
    mkdir -p "$m/gaokun3/diag" && f=$m/gaokun3/diag/job-${d##*/}.log && {
        echo "=== job ${d##*/}：$(cat "$d/fn") · 退出码 $(cat "$d/rc" 2>/dev/null) · 方式 $(cat "$d/mode" 2>/dev/null)"
        echo "--- stdout"; cat "$d/out" 2>/dev/null
        echo "--- stderr"; cat "$d/err" 2>/dev/null
    } > "$f"
    sync
    [ "$was" = ro ] && mount -o remount,ro "$m" 2>/dev/null
    return 0
}

# ── 把日志另存到用户拿得到的地方（v1.0 计划 GUI-12 的后端）──────────────────────
# 问题：安装 U 盘只有一个分区，类型是 EFI System（build-usb.sh 的 sgdisk -t 1:ef00），日志写在它的 gaokun3/diag/ 下。
#   Windows 与 macOS 默认都不给 ESP 分配盘符 / 不自动挂载 —— 装失败的人把 U 盘插回电脑，看不到日志。
#   （把 U 盘的分区类型改成 0700 要先实测华为固件能不能从那种分区回落启动，那是 GUI-12 的另一半，要上机。）
#   Windows 安装包那条路上，介质是内置盘上的 GK3LIVE（Basic data、有盘符），回 Windows 就能看到 gaokun3\diag\。
#
#   gk3_log_targets               能存日志的地方，一行一个（第一个是默认）：
#     LOGTARGET part=/dev/sda1 fs=vfat size_mib=… label=… removable=yes medium=no esp=no
#     只列 FAT / exFAT：不往 NTFS 写（Windows 休眠 / 快速启动时写它会损坏数据，见 gk3__ntfs_hibernated 那段）。
#     顺序：另插的 U 盘（可移动、不是启动介质）→ 启动介质本身（esp=yes 的话电脑上不好读）
#   gk3_save_logs [分区|目录]      不给就用 gk3_log_targets 的第一个；给目录就直接写进去（测试 / 高级用法）。
#     写一个 gaokun3-logs-<时间>/：installer.log、各 job 的输出、dmesg、本次启动的 journal、磁盘探测、
#     sgdisk -p、分区表备份（gpt-before-*.bin）、/etc/gaokun3-release、最近的 gk3-diag 报告。
#     stdout：LOGSAVED dir=… part=…|- files=N esp=yes|no
#     ⓘ 不含 WiFi 密码：wpa_supplicant.conf 不拷；installer.log 里 ShellBackend 已经把密码遮掉了。
gk3__log_removable() {   # $1=整块盘（/dev/sda）→ 0 = 可移动（removable=1 或走 USB；测试时 GK3_ALLOW_LOOP=1 的 loop 也算）
    local n=${1##*/}
    [ "$(cat "/sys/block/$n/removable" 2>/dev/null)" = 1 ] && return 0
    [ "$(lsblk -dno TRAN "$1" 2>/dev/null | tr -d ' ')" = usb ] && return 0
    case "$n" in loop*) [ "${GK3_ALLOW_LOOP:-0}" = 1 ] && return 0 ;; esac
    return 1
}

gk3_log_targets() {
    local medium s dev disk fs rem esp lab sz line first="" rest="" mdev
    mdev=$(findmnt -no SOURCE /media/gk3 2>/dev/null || echo "")
    for s in /sys/class/block/*; do
        [ -f "$s/partition" ] || continue
        dev=/dev/${s##*/}; [ -b "$dev" ] || continue
        fs=$(blkid -p -o value -s TYPE "$dev" 2>/dev/null)
        case "$fs" in vfat|exfat) ;; *) continue ;; esac
        disk=$(gk3__disk_of "$dev") || continue
        medium=no; [ -n "$mdev" ] && [ "$(readlink -f "$mdev")" = "$(readlink -f "$dev")" ] && medium=yes
        rem=no; gk3__log_removable "$disk" && rem=yes
        # 内置盘上的分区（ESP、Windows 的分区）不当存日志的地方 —— 除非它就是启动介质（免 U 盘时的 GK3LIVE）
        [ "$rem" = yes ] || [ "$medium" = yes ] || continue
        esp=no; [ "$(blkid -p -o value -s PART_ENTRY_TYPE "$dev" 2>/dev/null | tr 'A-F' 'a-f')" = c12a7328-f81f-11d2-ba4b-00a0c93ec93b ] && esp=yes
        lab=$(blkid -o value -s LABEL "$dev" 2>/dev/null)
        sz=$(( $(blockdev --getsize64 "$dev" 2>/dev/null || echo 0) / 1048576 ))
        line="LOGTARGET part=$dev fs=$fs size_mib=$sz label=$(gk3__enc "$lab") removable=$rem medium=$medium esp=$esp"
        if [ "$medium" = no ]; then first="$first$line"$'\n'; else rest="$rest$line"$'\n'; fi
    done
    printf '%s%s' "$first" "$rest"
}

gk3_save_logs() {
    local tgt=${1:-} part="-" m="" own="" was="" esp=no dir n=0 f
    if [ -z "$tgt" ]; then
        tgt=$(gk3_log_targets | head -1 | sed -n 's/^LOGTARGET part=\([^ ]*\).*/\1/p')
        [ -n "$tgt" ] || { gk3_fail logs-no-target -- "找不到能存日志的地方：插一个 FAT / exFAT 的 U 盘再试"; return 1; }
    fi
    if [ -d "$tgt" ]; then
        m=$tgt
    elif [ -b "$tgt" ]; then
        part=$tgt
        case "$(blkid -p -o value -s TYPE "$part" 2>/dev/null)" in vfat|exfat) ;; *) gk3_fail logs-not-fat "part=$part" -- "$part 不是 FAT / exFAT —— 不往别的文件系统写日志"; return 1 ;; esac
        [ "$(blkid -p -o value -s PART_ENTRY_TYPE "$part" 2>/dev/null | tr 'A-F' 'a-f')" = c12a7328-f81f-11d2-ba4b-00a0c93ec93b ] && esp=yes
        m=$(findmnt -rno TARGET -S "$part" 2>/dev/null | head -1)
        if [ -n "$m" ]; then
            was=$(findmnt -rno OPTIONS -S "$part" 2>/dev/null | head -1 | cut -d, -f1)
            [ "$was" = rw ] || mount -o remount,rw "$m" 2>/dev/null || { gk3_fail logs-readonly "part=$part" -- "$part 改不成读写（写保护？）"; return 1; }
        else
            m=$(mktemp -d /tmp/gk3-logs.XXXX); own=1
            mount "$part" "$m" 2>/dev/null || { rmdir "$m"; gk3_fail logs-mount "part=$part" -- "挂不上 $part"; return 1; }
        fi
    else
        gk3_fail usage -- "不是分区也不是目录：$tgt"; return 1
    fi
    dir=$m/gaokun3-logs-$(date +%Y%m%d-%H%M%S)
    if mkdir -p "$dir/jobs"; then
        for f in /media/gk3/gaokun3/diag/installer.log /run/gk3-installer/installer.log /etc/gaokun3-release \
                 /media/gk3/gaokun3/release.txt; do
            [ -f "$f" ] && cp "$f" "$dir/$(basename "$(dirname "$f")")-$(basename "$f")" 2>/dev/null && n=$((n + 1))
        done
        f=$(ls -t /media/gk3/gaokun3/diag/boot-*.log 2>/dev/null | head -1)
        [ -n "$f" ] && cp "$f" "$dir/" && n=$((n + 1))
        for f in "$GK3_JOBDIR"/*/; do
            [ -f "$f/fn" ] || continue
            { echo "=== $(cat "$f/fn") 退出码 $(cat "$f/rc" 2>/dev/null || echo 没有)"; echo "--- stdout"; cat "$f/out" 2>/dev/null
              echo "--- stderr"; cat "$f/err" 2>/dev/null; } > "$dir/jobs/$(basename "$f").log" && n=$((n + 1))
        done
        for f in /media/gk3/gaokun3/gpt-before-*.bin /tmp/gpt-before-*.bin; do
            [ -f "$f" ] && cp "$f" "$dir/" && n=$((n + 1))
        done
        dmesg > "$dir/dmesg.txt" 2>&1 && n=$((n + 1))
        command -v journalctl >/dev/null && journalctl -b --no-pager 2>&1 | tail -5000 > "$dir/journal.txt" && n=$((n + 1))
        gk3_probe > "$dir/probe.txt" 2>&1 && n=$((n + 1))
        for f in /sys/block/*; do
            case "${f##*/}" in nvme*|sd*|mmcblk*) sgdisk -p "/dev/${f##*/}" ;; esac
        done > "$dir/sgdisk.txt" 2>&1 && n=$((n + 1))
        sync
    else
        n=-1
    fi
    if [ -n "$own" ]; then umount "$m" 2>/dev/null; rmdir "$m" 2>/dev/null
    elif [ -n "$part" ] && [ "$part" != - ] && [ "$was" = ro ]; then mount -o remount,ro "$m" 2>/dev/null; fi
    [ "$n" -ge 0 ] || { gk3_fail logs-mkdir -- "在 $tgt 上建不了目录（满了？）"; return 1; }
    echo "LOGSAVED dir=$(gk3__enc "${dir#"$m"/}") part=$part files=$n esp=$esp"
}
