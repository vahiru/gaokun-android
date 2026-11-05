/*
 * 统一启动入口（gk3boot.efi）的 Android 侧：开机完成线程（tools/gk3boot/README.md）。
 *
 * ── 为什么放在 boot_control HAL 里 ────────────────────────────────────────────
 *   要做的三件事正好是这个 HAL 已经有权限的：挂 ESP 改文件（EspSlot.cpp，sepolicy/hal_bootctl_default.te）、
 *   读写 misc（AOSP vendor/hal_bootctl_default.te:15-16 的 misc_block_device rw）。另起一个 vendor 小程序
 *   就要再开一个域、再给一遍 ESP 与 misc 的权限，还要和 HAL 抢同一个挂载点。所以是 HAL 里的一个线程。
 *
 * ── 触发 ──────────────────────────────────────────────────────────────────────
 *   android.hardware.boot-service.gaokun3.rc：on property:sys.boot_completed=1 → setprop vendor.gaokun3.boot.done 1
 *   （vendor_init 读得到 boot_status_prop：refs/lineage-sepolicy/private/vendor_init.te:316）。
 *   HAL 只等自己的 vendor 属性（WaitForProperty，用法见 refs/lineage-system-core/init/subcontext_test.cpp:76），
 *   不直接读 sys.boot_completed（boot_status_prop 是 system_restricted_prop，设计稿 §4.6.1 说的"走 vendor 属性中转"）。
 *   每次开机只做一次；HAL 中途重启时看到 vendor.gaokun3.bootentry.done 已有值就不再做。
 *
 * ── 做什么（顺序有意义）──────────────────────────────────────────────────────
 *   1. GK3 记录（misc+8 KiB，tools/gk3boot/README.md §5）：boot_streak 清零；把没通知过的事件取出来、置"已通知"；
 *      写后读回（gk3_blk_write_bytes_verify，O_DIRECT 读的是盘上内容，不是页缓存）。记录无效 = 入口没在动作模式下
 *      跑过 = 什么都不写（不替入口建记录）。
 *   2. bless：ro.boot.gk3boot.entry（入口把 LoaderBootCountPath 的文件名报在 cmdline 里）带 +N[-M] 计数的话，
 *      改名成不带计数的 gk3boot-android-<x>.conf。计数是 systemd-boot 每次开机递减的，开机完成才祝福 ⇒
 *      新部署 / 新激活的入口连续 3 次没走到这里，systemd-boot 就自己改走直连条目（E6 实测）。
 *   3. 按 persist.vendor.gaokun3.gk3boot 对齐 ESP（见下面"部署"）。
 *      2、3 两步先【只读】挂 ESP 算一遍（dry），真有事要做才读写挂第二遍 —— 正常开机对 ESP 零写入
 *      （vfat 读写挂载本身就会写"脏"位；设计稿 §4.12、U5）。
 *   4. 导出 vendor.gaokun3.bootentry.*（Parts 据此发通知），最后写 .done（Parts 等它）。
 *
 * ── 部署（persist.vendor.gaokun3.gk3boot）────────────────────────────────────
 *   off（缺省）：删掉全部 gk3boot / gk3prev 条目（含 .staged 与 gk3boot-tools.conf）和 EFI/gk3boot/<ver>/ 目录
 *     （log/ 留着）⇒ 下次开机走直连条目。
 *   observe / action：让"现役入口"= 本槽 vendor 里那一版（/vendor/boot/gk3boot/{gk3boot.efi,version[,fastboot.img]}），
 *   模式 = 属性：
 *     · 二进制不同或没有 → 拷到 EFI/gk3boot/<ver>/gk3boot.efi（.new → fsync → 读回比对 → rename）；
 *     · 执行端 initramfs fastboot.img（设计稿 §4.1、§4.3.5；与 gk3boot.efi 同版本、同目录、一起轮换）同一条规则拷到
 *       EFI/gk3boot/<ver>/fastboot.img。它写失败【不挡】入口部署（只记日志 + error）：没有执行端时 gk3boot 照常启动
 *       Android。vendor 没带它时，ESP 上同版本目录里的那份（违反了"换任一个就换版本串"的规矩才会有）删掉；
 *     · 非默认条目 gk3boot-tools.conf（sort-key 0gk3tools，options gk3.action=fastboot：从 systemd-boot 菜单直接进
 *       执行端）只在 action 且这一版的 fastboot.img 已在 ESP 上时部署，总是指向现役那一版；observe 时删掉。
 *       它不带计数、不 bless，也不是 default 通配 *-android-<x>.conf 能命中的名字；
 *       ESP 空间：写 fastboot.img 之前看 statvfs，剩余 < 它的大小 + 1 MiB 就不写（入口照常部署）。
 *       常态最多两版目录共存（现役 + gk3prev）；OTA 后、新槽开机完成之前是三版（再加 postinstall 铺的 staged），
 *       激活时最老那版被下面的"删没人引用的目录"回收 —— 激活本身只写条目、不新增二进制；
 *     · 现役条目（gk3boot-android-{a,b}[+N].conf）已是这一版、这个模式 → 不动（只清掉过期的 .staged）；
 *     · 否则：现役条目是【另一版】且【被祝福过】（至少一个不带计数 = 那一版在这台机器上起来过）→ 改写成
 *       gk3prev-android-{a,b}.conf（上一版入口，sort-key 0gk3prev，设计稿 §4.11 第 3 步）；
 *       然后写新的 gk3boot-android-{a,b}+3.conf（带计数 = 新激活的入口要靠开机完成来证明），删旧的现役条目与 .staged；
 *     · 最后删掉没有任何条目（任何 .conf 的 efi 行，gk3boot-tools.conf 也算）引用的 EFI/gk3boot/<ver>/（整目录，
 *       gk3boot.efi 与 fastboot.img 一起走）。
 *   postinstall（OTA 时，新 vendor 的脚本）在 ESP 上还没有入口时直接部署 +3，已有入口时只铺新版本目录 + .staged；
 *   .staged 在这里被"激活"（就是上面的对齐：vendor 版本 = staged 版本）。OTA 回滚到旧槽时 vendor 版本 ≠ staged 版本，
 *   .staged 被删、现役入口留在旧槽那一版 —— "入口的版本跟着正在跑的系统走"。
 *   ⚠️ 属性改了要【下一次开机完成】才生效（这里只在开机完成时做一次）。
 *   ⚠️ 入口条目计数用完（+0-N）不会被这里重新武装：那说明入口这一版在这台机器上连续起不来，留给人看
 *      （bypassed 通知）；要重来就 setprop off → 重启 → setprop observe|action → 重启。
 *
 * ── 导出的属性（vendor_gaokun3_prop，Parts = system_app 能读：sepolicy/vendor_gaokun3_props.te）──
 *   vendor.gaokun3.bootentry.via       gk3boot | gk3prev | direct（这次开机经过谁）
 *   vendor.gaokun3.bootentry.event     ro.boot.gk3boot.event 原样（none / fallback / bcab_invalid / forced）
 *   vendor.gaokun3.bootentry.notify    要通知的事件，逗号分隔（fallback、boot_corrupt、bcb_dropped、wipe_failed、
 *                                      refused_merging、bootloop、noslot）；取自 GK3 事件环里没通知过的，
 *                                      记录无效（观察模式）时退回 cmdline 的 event
 *   vendor.gaokun3.bootentry.bypassed  1 = 入口已部署、这次却没经过它（计数用完回落直连 / 菜单里手选直连 / fail-open）
 *   vendor.gaokun3.bootentry.streak    清零之前的 boot_streak（没有记录时为空）
 *   vendor.gaokun3.bootentry.mode      对齐之后 ESP 上现役入口的模式：off / observe / action / unknown
 *   vendor.gaokun3.bootentry.version   对齐之后现役入口的版本（off 时为空）
 *   vendor.gaokun3.bootentry.error     出错时一句话（≤ 91 字节），没错为空
 *   vendor.gaokun3.bootentry.done      本次开机的令牌（时间戳-pid），最后写；Parts 按它去重
 *   —— S15 双系统（设计稿 §4.6.1 动作 6、7，§4.9.3、§4.9.4）——
 *   vendor.gaokun3.bootentry.windows   1 = ESP 上有 EFI/Microsoft/Boot/bootmgfw.efi（Parts 据此显示"重启到 Windows"和"默认系统"）
 *   vendor.gaokun3.bootentry.default   GK3 记录里入口写下的默认系统缓存：windows / android / unknown
 *   vendor.gaokun3.bootentry.default_pending  已请求、入口还没应用的 set_default：windows / android / 空
 *   vendor.gaokun3.bootentry.menu      1 = "重启到引导菜单"（reboot,bootloader）真能到执行端：这次经入口、分派开着
 *                                      （ro.boot.gk3boot.dispatch=1）、现役那一版的 fastboot.img 在 ESP 上
 *   vendor.gaokun3.bootentry.ack       请求的回执 <请求>:<ok|error:原因>:<pid>-<序号>（见下面"请求"）
 *   notify 另可有 intent_dropped / default_reset（入口作废了双系统意图 / 删了不合法的 LoaderEntryDefault）与
 *   loader_replaced（有 Windows 且 EFI/BOOT/BOOTAA64.EFI 与 EFI/systemd/systemd-bootaa64.efi 不同 = Windows 换掉了
 *   回落路径上的启动器；只通知、不修 —— 改它会让 BitLocker 要密钥，§4.9.6）
 *
 * ── 请求（S15，动作 7）────────────────────────────────────────────────────────
 *   Parts（system_app，coredomain）写不了任何 vendor 属性（vendor_gaokun3_props.te 顶上那条 neverallow），所以走 init 中转：
 *   Parts 设 sys.gaokun3.bootreq=next_windows|default_windows|default_android（system_prop）→ 本 HAL 的 rc 在 vendor_init 里
 *   setprop vendor.gaokun3.bootentry.request <同值> + vendor.gaokun3.bootentry.ring 1 → 这里的请求线程等 ring=1、清 ring、
 *   把请求写进 misc 的 GK3 记录（next=windows 或 set_default，写后读回）→ 设 ack。Parts 看到 ack 再发起重启（或提示"下次开机生效"）。
 *   Android 不碰 efivarfs（U14 选 a）：真正改 LoaderEntryDefault / 写 OneShot 的是下一次开机的入口。
 *   记录无效（入口没在动作模式下跑过）⇒ error:no-record：入口不在，意图也没人消费。
 *
 * ── 关机标记（S15，§4.9.3"冷开机进 Windows"）───────────────────────────────────
 *   rc 的 on shutdown 里 exec 本二进制 --gk3-mark-poweroff（MarkPoweroffMain）：sys.powerctl 以 shutdown 开头、实际默认是
 *   Windows（GK3 的缓存 / 未应用的 set_default）、而且这次是经动作模式的入口开的机（ro.boot.gk3boot.mode=action ——
 *   只有那样入口才预置了 OneShot，下次冷开机才会先进入口去消费标记）⇒ 置 clean_poweroff。
 *   设计稿写的是 gk3-misc mark-poweroff；实现成 HAL 二进制的一个模式，是因为写 misc 的域被核心 neverallow 限定在一张白名单里
 *   （refs/lineage-sepolicy/private/domain.te:1596-1612，crDroid 树 :1661-1677，user 构建生效），hal_bootctl_server 在单子上、新建的 gk3_misc 域不在；
 *   exec 本二进制经 init_daemon_domain（refs/lineage-sepolicy/vendor/hal_bootctl_default.te:5-6）落到 hal_bootctl_default，
 *   misc 读写已有，只补了读 sys.powerctl（sepolicy/hal_bootctl_default.te）。同一个库函数 gk3_rec_mark_poweroff 也在
 *   主机 gk3-misc mark-poweroff 里（离线演练 / QEMU 夹具对照）。时序：init 收到 sys.powerctl 后先排 shutdown 触发器、
 *   再排 shutdown_done（真正关机），refs/lineage-system-core/init/reboot.cpp:984-992，crDroid 树 system/core/init/reboot.cpp:1185-1193（2026-10-05 构建机只读核对）；exec 会让 init 等它退出。
 *   失败（misc 读写错、5 秒还没完）的后果只是"下次开机进 Android"，所以这里绝不阻塞关机：alarm(5) 自杀。
 *   已知限制：从 Android 关机后，下次开机在 systemd-boot 菜单里手动改选 Windows，标记会留到下一次入口运行 ——
 *   那时会多跳一次 Windows（入口分不出"经预置的 OneShot 进来"还是"菜单里手选进来"：两种 systemd-boot 都把 OneShot 删了）。
 *
 * ⬜ 未编译、未上机（2026-10-05；S15 部分同样）。
 *
 * Copyright 2026 The gaokun-android contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "Gk3Boot.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <functional>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <android-base/file.h>
#include <android-base/logging.h>
#include <android-base/properties.h>
#include <android-base/strings.h>
#include <android-base/unique_fd.h>

#include <gk3core.h>

#include "EspSlot.h"

namespace gaokun3 {
namespace {

using android::base::EndsWith;
using android::base::GetProperty;
using android::base::SetProperty;
using android::base::StartsWith;

constexpr char kTrigger[] = "vendor.gaokun3.boot.done";
constexpr char kModeProp[] = "persist.vendor.gaokun3.gk3boot";
constexpr char kOutPrefix[] = "vendor.gaokun3.bootentry.";
constexpr char kVendorEfi[] = "/vendor/boot/gk3boot/gk3boot.efi";
constexpr char kVendorVer[] = "/vendor/boot/gk3boot/version";
constexpr char kVendorFb[] = "/vendor/boot/gk3boot/fastboot.img";
constexpr char kMiscDev[] = "/dev/block/by-name/misc";
constexpr char kActivePrefix[] = "gk3boot-android-";
constexpr char kPrevPrefix[] = "gk3prev-android-";
constexpr char kToolsEntry[] = "gk3boot-tools.conf";
// S15：ESP 上 Windows 的启动管理器（systemd-boot 据它生成 auto-windows，refs/systemd-v257/src/boot/boot.c:2146-2148）
constexpr char kBootmgfw[] = "/EFI/Microsoft/Boot/bootmgfw.efi";
constexpr char kFallbackLoader[] = "/EFI/BOOT/BOOTAA64.EFI";
constexpr char kSdBoot[] = "/EFI/systemd/systemd-bootaa64.efi";
constexpr char kReqProp[] = "vendor.gaokun3.bootentry.request";
constexpr char kRingProp[] = "vendor.gaokun3.bootentry.ring";
// 写 fastboot.img 之前 ESP 上至少要剩 它的大小 + 这么多（.new 与旧文件并存的那一刻也算在"它的大小"里）
constexpr uint64_t kFbReserve = 1 << 20;
constexpr size_t kPropMax = 91;  // PROP_VALUE_MAX - 1

void Out(const char* key, const std::string& value) {
    std::string v = value.substr(0, kPropMax);
    if (!SetProperty(std::string(kOutPrefix) + key, v))
        LOG(WARNING) << "gk3boot: setprop " << kOutPrefix << key << "=" << v << " failed";
}

// ──────────────────────────────────────────────────────────────── misc：GK3 记录

// O_DIRECT 要求缓冲区、偏移、长度都按逻辑块对齐；NVMe 的逻辑块是 512 或 4096，统一按 4096 走两者都满足。
constexpr uint32_t kBlk = 4096;

int BlkRead(void* ctx, uint64_t lba, uint32_t count, void* buf) {
    int fd = *static_cast<int*>(ctx);
    ssize_t n = static_cast<ssize_t>(count) * kBlk;
    return TEMP_FAILURE_RETRY(pread(fd, buf, n, static_cast<off_t>(lba * kBlk))) == n ? 0 : -1;
}
int BlkWrite(void* ctx, uint64_t lba, uint32_t count, const void* buf) {
    int fd = *static_cast<int*>(ctx);
    ssize_t n = static_cast<ssize_t>(count) * kBlk;
    return TEMP_FAILURE_RETRY(pwrite(fd, buf, n, static_cast<off_t>(lba * kBlk))) == n ? 0 : -1;
}
int BlkFlush(void* ctx) {
    return fsync(*static_cast<int*>(ctx)) == 0 ? 0 : -1;
}

bool Interesting(uint16_t code) {
    switch (code) {
        case GK3_EV_FALLBACK:
        case GK3_EV_BOOT_CORRUPT:
        case GK3_EV_BCB_DROPPED:
        case GK3_EV_WIPE_FAILED:
        case GK3_EV_REFUSED_MERGING:
        case GK3_EV_BOOTLOOP:
        case GK3_EV_NOSLOT:
        case GK3_EV_DEFAULT_RESET:    // S15：LoaderEntryDefault 被入口删掉（= 默认回到 Android）
        case GK3_EV_INTENT_DROPPED:   // S15："重启到 Windows" / 关机进 Windows / 设默认没执行
            return true;
        default:  // migrated / bcb_ignored / to_windows / default_set 只是记录（后两样是用户自己要的），不打扰用户
            return false;
    }
}

// 开机完成线程、请求线程、关机模式三处都读改写 misc 的 GK3 记录：同一进程里用这把锁串起来
// （关机模式是另一个进程，与开机完成同时发生的概率可以不计；最坏是一方的改动被另一方覆盖，见文件头）。
std::mutex& MiscMu() {
    static std::mutex m;
    return m;
}

// 读改写 misc+8 KiB 的 GK3 记录：edit 返回 true 才写（seal → 写 → flush → 逐块读回，再按字节读一遍与写的比对、过 CRC）。
// *valid = 记录有效（无效时不调 edit、不写 —— 不替入口建记录）。返回 false = 出错（*err 有原因）。
bool RecRmw(const std::function<bool(uint8_t*)>& edit, bool* valid, std::string* err) {
    std::lock_guard<std::mutex> g(MiscMu());
    *valid = false;
    android::base::unique_fd fd(TEMP_FAILURE_RETRY(open(kMiscDev, O_RDWR | O_DIRECT | O_CLOEXEC)));
    if (fd < 0) {
        *err = std::string("open misc: ") + strerror(errno);
        return false;
    }
    off_t size = lseek(fd.get(), 0, SEEK_END);
    if (size < static_cast<off_t>(GK3_MISC_GK3_OFF + GK3_REC_SIZE)) {
        *err = "misc too small";
        return false;
    }
    int raw = fd.get();
    gk3_blk dev = {&raw, kBlk, static_cast<uint64_t>(size) / kBlk, BlkRead, BlkWrite, BlkFlush};

    void* scratch = nullptr;
    if (posix_memalign(&scratch, kBlk, 2 * kBlk) != 0) {
        *err = "posix_memalign failed";
        return false;
    }
    std::unique_ptr<void, decltype(&free)> scratch_guard(scratch, &free);

    uint8_t rec[GK3_REC_SIZE];
    gk3_err e = gk3_blk_read_bytes(&dev, 0, dev.num_blocks, GK3_MISC_GK3_OFF, rec, sizeof(rec), scratch, 2 * kBlk);
    if (e != GK3_OK) {
        *err = std::string("read GK3 record: ") + gk3_strerror(e);
        return false;
    }
    if (gk3_rec_validate(rec) != GK3_OK) {
        // 无记录 = 入口没在动作模式下跑过（或者记录被断电写坏了 —— 入口下次会重建，§4.5）。不替它建。
        LOG(INFO) << "gk3boot: no valid GK3 record at misc+8KiB (" << gk3_strerror(gk3_rec_validate(rec)) << ")";
        return true;
    }
    *valid = true;
    if (!edit(rec)) return true;  // 没有要改的：一个字节都不写
    gk3_rec_seal(rec);
    e = gk3_blk_write_bytes_verify(&dev, 0, dev.num_blocks, GK3_MISC_GK3_OFF, rec, sizeof(rec), scratch, 2 * kBlk);
    if (e != GK3_OK) {
        *err = std::string("write GK3 record: ") + gk3_strerror(e);
        return false;
    }
    // 再按字节读一遍、过一遍 CRC（write_verify 比的是整块，这里确认读回来的是一份有效记录、就是写下去的那份）
    uint8_t back[GK3_REC_SIZE];
    e = gk3_blk_read_bytes(&dev, 0, dev.num_blocks, GK3_MISC_GK3_OFF, back, sizeof(back), scratch, 2 * kBlk);
    if (e != GK3_OK || gk3_rec_validate(back) != GK3_OK || memcmp(back, rec, sizeof(rec)) != 0) {
        *err = "GK3 record read-back mismatch";
        return false;
    }
    return true;
}

struct RecResult {
    bool valid = false;               // misc 里有有效的 GK3 记录
    int streak = -1;                  // 清零之前的值
    std::vector<std::string> notify;  // 没通知过、值得通知的事件名（去重，从旧到新）
    std::string error;
    gk3_os default_os = GK3_OS_UNKNOWN;    // S15：入口写下的默认系统缓存
    gk3_setdef pending = GK3_SETDEF_NONE;  // S15：还没被入口应用的 set_default 请求
};

const char* OsName(gk3_os o) {
    return o == GK3_OS_WINDOWS ? "windows" : o == GK3_OS_ANDROID ? "android" : "unknown";
}
const char* SetdefName(gk3_setdef d) {
    return d == GK3_SETDEF_WINDOWS ? "windows" : d == GK3_SETDEF_ANDROID ? "android" : "";
}

RecResult ClearStreakAndTakeEvents() {
    RecResult r;
    uint32_t upto = 0;
    bool pending = false;
    bool ok = RecRmw(
            [&](uint8_t* rec) {
                r.streak = gk3_rec_boot_streak(rec);
                r.default_os = gk3_rec_default_os(rec);
                r.pending = gk3_rec_set_default_req(rec);
                gk3_event ev[GK3_EV_N];
                uint32_t n = std::min<uint32_t>(gk3_rec_events(rec, ev, GK3_EV_N), GK3_EV_N);
                for (uint32_t i = 0; i < n; i++) {
                    if (ev[i].flags & GK3_EVF_NOTIFIED) continue;
                    pending = true;
                    upto = std::max(upto, ev[i].seq);
                    if (!Interesting(ev[i].code)) continue;
                    std::string name = gk3_ev_name(static_cast<gk3_ev_code>(ev[i].code));
                    if (std::find(r.notify.begin(), r.notify.end(), name) == r.notify.end()) r.notify.push_back(name);
                    LOG(INFO) << "gk3boot: event seq=" << ev[i].seq << " " << name << " slot=" << unsigned(ev[i].slot)
                              << " aux=" << ev[i].aux;
                }
                if (r.streak == 0 && !pending) return false;  // 正常开机的常态：一个字节都不写
                gk3_rec_set_boot_streak(rec, 0);
                if (pending) gk3_rec_events_mark_notified(rec, upto);
                return true;
            },
            &r.valid, &r.error);
    if (ok && r.valid && (r.streak != 0 || pending))
        LOG(INFO) << "gk3boot: GK3 record: boot_streak " << r.streak << " -> 0"
                  << (pending ? ", events up to seq " + std::to_string(upto) + " marked notified" : "")
                  << " (written, read back OK)";
    if (!r.valid) r.notify.clear();
    return r;
}

// ──────────────────────────────────────────────────────────────── ESP：条目

enum class Kind { kActive, kStaged, kPrev, kTools };

struct Gk3Entry {
    std::string file;      // loader/entries 下的文件名
    Kind kind = Kind::kActive;
    char slot = 'a';
    bool counted = false;  // 文件名带 +N[-M]
    std::string version;   // efi 行 /EFI/gk3boot/<ver>/gk3boot.efi 里的 <ver>；解析不出为空
    bool observe = false;  // options 里有 gk3.observe=1
};

std::string EntriesDir() { return std::string(kEspRoot) + "/loader/entries"; }
std::string Gk3Dir() { return std::string(kEspRoot) + "/EFI/gk3boot"; }

bool AllDigits(const std::string& s) {
    return !s.empty() && std::all_of(s.begin(), s.end(), [](char c) { return c >= '0' && c <= '9'; });
}

// 版本串：与 tools/gk3boot/efi/Makefile 的 BOOT_VERSION 同一个字符集（[A-Za-z0-9._+-]），≤ 64；
// 另外不能是 log（EFI/gk3boot/log 是入口的日志目录）、. 或 ..
bool ValidVersion(const std::string& v) {
    if (v.empty() || v.size() > 64 || v == "." || v == ".." || strcasecmp(v.c_str(), "log") == 0) return false;
    return std::all_of(v.begin(), v.end(), [](char c) {
        return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' ||
               c == '_' || c == '+' || c == '-';
    });
}

// <32 位小写十六进制>-android-<x>.conf：直连条目（与 postinstall 的 is_direct_entry、gk3boot.c 的 direct_cb 同规则）
bool IsDirectEntry(const std::string& n, char* slot) {
    if (n.size() != 32 + 9 + 1 + 5) return false;
    for (size_t i = 0; i < 32; i++)
        if (!((n[i] >= '0' && n[i] <= '9') || (n[i] >= 'a' && n[i] <= 'f'))) return false;
    if (n.compare(32, 9, "-android-") != 0 || (n[41] != 'a' && n[41] != 'b') || n.compare(42, 5, ".conf") != 0)
        return false;
    *slot = n[41];
    return true;
}

// gk3boot-android-<x>.conf / gk3boot-android-<x>+N[-M].conf / gk3boot-android-<x>.conf.staged / gk3prev-android-<x>.conf
// / gk3boot-tools.conf（只认这一个精确名字：不带计数、没有槽，slot 填 'a' 不用）
bool ParseEntryName(const std::string& n, Gk3Entry* e) {
    std::string rest;
    if (n == kToolsEntry) {
        e->kind = Kind::kTools;
        e->slot = 'a';
        e->file = n;
        e->counted = false;
        return true;
    }
    if (StartsWith(n, kActivePrefix)) {
        e->kind = Kind::kActive;
        rest = n.substr(strlen(kActivePrefix));
    } else if (StartsWith(n, kPrevPrefix)) {
        e->kind = Kind::kPrev;
        rest = n.substr(strlen(kPrevPrefix));
    } else {
        return false;
    }
    if (rest.empty() || (rest[0] != 'a' && rest[0] != 'b')) return false;
    e->slot = rest[0];
    rest = rest.substr(1);
    e->file = n;
    e->counted = false;
    if (rest == ".conf") return true;
    if (e->kind == Kind::kActive && rest == ".conf.staged") {
        e->kind = Kind::kStaged;
        return true;
    }
    // systemd-boot 的计数：+LEFT[-DONE]（boot.c 的 boot_entry_parse_tries）；只有现役条目会带
    if (e->kind != Kind::kActive || rest.size() < 7 || rest[0] != '+' || !EndsWith(rest, ".conf")) return false;
    std::string c = rest.substr(1, rest.size() - 1 - 5);
    size_t dash = c.find('-');
    if (dash == std::string::npos ? !AllDigits(c) : (!AllDigits(c.substr(0, dash)) || !AllDigits(c.substr(dash + 1))))
        return false;
    e->counted = true;
    return true;
}

// 条目里 efi 行指向 /EFI/gk3boot/<ver>/… 时返回 <ver>（不限文件名：手放的实验条目也算"引用"了那个目录）
std::string EfiDirOf(const std::string& content) {
    for (const auto& raw : android::base::Split(content, "\n")) {
        std::string line = android::base::Trim(raw);
        if (!StartsWith(line, "efi") || line.size() < 4 || (line[3] != ' ' && line[3] != '\t')) continue;
        std::string path = android::base::Trim(line.substr(3));
        std::replace(path.begin(), path.end(), '\\', '/');
        // systemd-boot 比较路径不分大小写（vfat），这里也按不分大小写认前缀
        static const std::string kPfx = "/EFI/gk3boot/";
        if (path.size() <= kPfx.size() || strncasecmp(path.c_str(), kPfx.c_str(), kPfx.size()) != 0) return "";
        std::string rest = path.substr(kPfx.size());
        size_t slash = rest.find('/');
        return slash == std::string::npos ? "" : rest.substr(0, slash);
    }
    return "";
}

bool OptionsObserve(const std::string& content) {
    for (const auto& raw : android::base::Split(content, "\n")) {
        std::string line = android::base::Trim(raw);
        if (!StartsWith(line, "options")) continue;
        for (const auto& tok : android::base::Split(line, " \t"))
            if (tok == "gk3.observe=1") return true;
    }
    return false;
}

struct EspScan {
    std::vector<Gk3Entry> gk3;     // 我们的入口条目
    bool direct[2] = {false, false};
    std::set<std::string> referenced;  // 任何 .conf / .conf.staged 的 efi 行引用的 EFI/gk3boot/<d>（小写比较）
    bool ok = false;
};

std::string Lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(tolower(c)); });
    return s;
}

EspScan ScanEsp() {
    EspScan s;
    std::unique_ptr<DIR, decltype(&closedir)> d(opendir(EntriesDir().c_str()), &closedir);
    if (!d) {
        PLOG(ERROR) << "gk3boot: opendir " << EntriesDir();
        return s;
    }
    while (struct dirent* de = readdir(d.get())) {
        std::string n = de->d_name;
        char slot;
        if (IsDirectEntry(n, &slot)) {
            s.direct[slot - 'a'] = true;
            continue;
        }
        if (!EndsWith(n, ".conf") && !EndsWith(n, ".conf.staged")) continue;
        std::string content;
        if (!android::base::ReadFileToString(EntriesDir() + "/" + n, &content)) {
            PLOG(WARNING) << "gk3boot: read " << n;
            continue;
        }
        std::string dir = EfiDirOf(content);
        if (!dir.empty()) s.referenced.insert(Lower(dir));
        Gk3Entry e;
        if (!ParseEntryName(n, &e)) continue;
        e.version = dir;
        e.observe = OptionsObserve(content);
        s.gk3.push_back(e);
    }
    s.ok = true;
    return s;
}


// ──────────────────────────────────────────────────────────────── ESP：两遍走

// ★ 正常开机对 ESP 零写入（设计稿 §4.12、U5 的理由）：vfat 以读写方式挂上就会在盘上置"脏"位、卸载时再清，
//   哪怕一个文件都没改。所以先【只读】挂上把要做的事算一遍（dry），确实有事要做才读写挂第二遍。
//   下面每个会改 ESP 的函数都接一个 Pass：dry 时只记 changed、不动盘。
struct Pass {
    bool dry = true;
    bool changed = false;
};

// 写一个小文件：<path>.new → fsync → rename。vfat 上 rename 不保证原子，但能把"截断的条目"窗口缩到最小
// （与 EspSlot.cpp 写 loader.conf 同一个做法）。
bool WriteFileAtomic(Pass* p, const std::string& path, const std::string& data) {
    std::string have;
    if (android::base::ReadFileToString(path, &have) && have == data) return true;
    p->changed = true;
    if (p->dry) return true;
    const std::string tmp = path + ".new";
    {
        android::base::unique_fd fd(
                TEMP_FAILURE_RETRY(open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644)));
        if (fd < 0 || !android::base::WriteFully(fd.get(), data.data(), data.size()) || fsync(fd.get()) != 0) {
            PLOG(ERROR) << "gk3boot: write " << tmp;
            unlink(tmp.c_str());
            return false;
        }
    }
    if (rename(tmp.c_str(), path.c_str()) != 0) {
        PLOG(ERROR) << "gk3boot: rename " << tmp << " -> " << path;
        unlink(tmp.c_str());
        return false;
    }
    return true;
}

bool MkdirP(const std::string& path) {
    std::string cur;
    for (const auto& part : android::base::Split(path, "/")) {
        if (part.empty()) continue;
        cur += "/" + part;
        if (mkdir(cur.c_str(), 0755) != 0 && errno != EEXIST) {
            PLOG(ERROR) << "gk3boot: mkdir " << cur;
            return false;
        }
    }
    return true;
}

bool RemoveTree(const std::string& path) {
    std::unique_ptr<DIR, decltype(&closedir)> d(opendir(path.c_str()), &closedir);
    if (!d) return unlink(path.c_str()) == 0;
    bool ok = true;
    while (struct dirent* de = readdir(d.get())) {
        std::string n = de->d_name;
        if (n == "." || n == "..") continue;
        std::string p = path + "/" + n;
        struct stat st;
        if (lstat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) {
            ok = RemoveTree(p) && ok;
        } else if (unlink(p.c_str()) != 0) {
            PLOG(WARNING) << "gk3boot: unlink " << p;
            ok = false;
        }
    }
    d.reset();
    if (rmdir(path.c_str()) != 0) {
        PLOG(WARNING) << "gk3boot: rmdir " << path;
        return false;
    }
    return ok;
}

// 把 want 放到 EFI/gk3boot/<ver>/<name>：已经逐字节相同就不写；否则 .new → fsync → 读回比对 → rename
// （与安装器 / postinstall"写完 cmp"同一条规矩，M4b 的教训）。gk3boot.efi 与 fastboot.img 共用。
// space_check：真写之前先看 ESP 剩余（fastboot.img 用；gk3boot.efi 约 100 KB，沿用原来"写失败就报错"）。
bool PutVerified(Pass* p, const std::string& ver, const std::string& name, const std::string& want, bool space_check,
                 std::string* err) {
    const std::string dir = Gk3Dir() + "/" + ver;
    const std::string dst = dir + "/" + name;
    std::string have;
    if (android::base::ReadFileToString(dst, &have) && have == want) return true;
    // 空间在 dry 那一遍就看（只读挂也能 statvfs）：不够时两遍都判"不写"，ESP 长期满着也不会每次开机都读写挂一遍
    if (space_check) {
        // statvfs：fs_type:filesystem getattr，所有域都有（refs/lineage-sepolicy/private/domain.te:276）
        struct statvfs sv;
        if (statvfs(kEspRoot, &sv) == 0) {
            uint64_t avail = static_cast<uint64_t>(sv.f_bavail) * sv.f_frsize;
            if (avail < want.size() + kFbReserve) {
                if (!p->dry)
                    LOG(ERROR) << "gk3boot: ESP has " << avail << " bytes free, " << name << " needs " << want.size()
                               << " + " << kFbReserve << " reserve; not writing it";
                *err = name + ": ESP too full";
                return false;
            }
        } else if (!p->dry) {
            PLOG(WARNING) << "gk3boot: statvfs " << kEspRoot << " (writing " << name << " anyway)";
        }
    }
    p->changed = true;
    if (p->dry) return true;
    if (!MkdirP(dir)) {
        *err = "mkdir EFI/gk3boot/" + ver + " failed";
        return false;
    }
    const std::string tmp = dst + ".new";
    {
        android::base::unique_fd fd(
                TEMP_FAILURE_RETRY(open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644)));
        if (fd < 0 || !android::base::WriteFully(fd.get(), want.data(), want.size()) || fsync(fd.get()) != 0) {
            PLOG(ERROR) << "gk3boot: write " << tmp;
            unlink(tmp.c_str());
            *err = "write " + name + " failed (ESP full?)";
            return false;
        }
    }
    std::string back;
    if (!android::base::ReadFileToString(tmp, &back) || back != want) {
        unlink(tmp.c_str());
        *err = name + " read-back mismatch";
        return false;
    }
    if (rename(tmp.c_str(), dst.c_str()) != 0) {
        PLOG(ERROR) << "gk3boot: rename " << tmp;
        unlink(tmp.c_str());
        *err = "rename " + name + " failed";
        return false;
    }
    LOG(INFO) << "gk3boot: installed " << dst << " (" << want.size() << " bytes)";
    return true;
}

// vendor 里那一版的入口二进制 → EFI/gk3boot/<ver>/gk3boot.efi。失败 = 这次不部署（调用方 keep_as_is）。
bool InstallBinary(Pass* p, const std::string& ver, std::string* err) {
    std::string want;
    if (!android::base::ReadFileToString(kVendorEfi, &want) || want.size() < 1024 || want.compare(0, 2, "MZ") != 0) {
        *err = "vendor gk3boot.efi unreadable or not a PE";
        return false;
    }
    return PutVerified(p, ver, "gk3boot.efi", want, false, err);
}

// 执行端 initramfs（gzip cpio，scripts/gk3boot/build-fastboot-img.sh 产出）→ EFI/gk3boot/<ver>/fastboot.img。
//   kAbsent  vendor 这一版不带执行端（ESP 上同版本目录里若有一份，删掉：同一个版本串只能对应一套文件）
//   kReady   ESP 上的那份与 vendor 逐字节相同（dry 时 = 第二遍会写成这样）
//   kFailed  vendor 那份坏了 / 写失败 / 空间不够 —— 只记 *err，入口照常部署（gk3boot 找不到执行端会照常启动 Android）
enum class FbState { kAbsent, kReady, kFailed };

FbState InstallFastboot(Pass* p, const std::string& ver, std::string* err) {
    if (access(kVendorFb, F_OK) != 0) {
        const std::string stale = Gk3Dir() + "/" + ver + "/fastboot.img";
        if (access(stale.c_str(), F_OK) == 0) {
            p->changed = true;
            if (!p->dry) {
                LOG(WARNING) << "gk3boot: vendor has no fastboot.img but " << stale
                             << " exists (same version string, different file set?); removing it";
                if (unlink(stale.c_str()) != 0) PLOG(WARNING) << "gk3boot: unlink " << stale;
            }
        }
        return FbState::kAbsent;
    }
    std::string want;
    if (!android::base::ReadFileToString(kVendorFb, &want) || want.size() < 18 ||
        static_cast<unsigned char>(want[0]) != 0x1f || static_cast<unsigned char>(want[1]) != 0x8b) {
        *err = "vendor fastboot.img unreadable or not gzip";
        return FbState::kFailed;
    }
    return PutVerified(p, ver, "fastboot.img", want, true, err) ? FbState::kReady : FbState::kFailed;
}

// 条目正文。postinstall（bin/gaokun3-ota-postinstall.sh 的 gk3_entry_text）写的是同一个格式，改一边要改另一边。
// title 只用 ASCII（设计稿 §4.13：UEFI ConOut 的 CJK 字形没验证过）。
std::string EntryText(Kind kind, char slot, const std::string& ver, bool observe) {
    std::string title = kind == Kind::kPrev ? "Android (previous loader)"
                                            : (observe ? "Android (gk3boot observe)" : "Android");
    std::string sort = kind == Kind::kPrev ? "0gk3prev" : "0gk3";
    return "title      " + title + "\n" +
           "version    gk3boot-" + ver + "\n" +
           "sort-key   " + sort + "\n" +
           "efi        /EFI/gk3boot/" + ver + "/gk3boot.efi\n" +
           "options    gk3.observe=" + (observe ? "1" : "0") + " gk3.hint=" + slot +
           // ★ 1.0（2026-10-05，E7 真机过）：动作模式顺带打开 BCB 分派 —— D4 把恢复出厂交给统一启动入口，
           //   分派关着时 Settings 写的 --wipe_data 不会被执行（B6）。观察模式不分派。postinstall 的 gk3_entry_text 逐字相同。
           (observe ? "" : " gk3.dispatch=1") + "\n";
}

// gk3boot-tools.conf 的正文（设计稿 §4.1、§4.3.5：菜单里直接进执行端）。postinstall 的 gk3_tools_text 同一格式。
// 不带 gk3.hint / gk3.observe：去执行端不需要选槽，观察模式也不部署它。
std::string ToolsText(const std::string& ver) {
    return "title      Android fastboot / boot menu\n"
           "version    gk3boot-" + ver + "\n" +
           "sort-key   0gk3tools\n"
           "efi        /EFI/gk3boot/" + ver + "/gk3boot.efi\n" +
           "options    gk3.action=fastboot\n";
}

std::string ActiveName(char slot, bool counted) {
    return std::string(kActivePrefix) + slot + (counted ? "+3" : "") + ".conf";
}
std::string PrevName(char slot) { return std::string(kPrevPrefix) + slot + ".conf"; }

void RemoveEntry(Pass* p, const Gk3Entry& e) {
    p->changed = true;
    if (p->dry) return;
    std::string path = EntriesDir() + "/" + e.file;
    if (unlink(path.c_str()) != 0 && errno != ENOENT) PLOG(WARNING) << "gk3boot: unlink " << path;
    else LOG(INFO) << "gk3boot: removed entry " << e.file;
}

// 删掉没有任何条目引用的 EFI/gk3boot/<d>/（log/ 除外）。dry 时 refs 由调用方给（盘上还没删的条目不能算引用）。
void CollectGarbage(Pass* p, const std::set<std::string>& refs) {
    std::unique_ptr<DIR, decltype(&closedir)> d(opendir(Gk3Dir().c_str()), &closedir);
    if (!d) return;
    std::vector<std::string> doomed;
    while (struct dirent* de = readdir(d.get())) {
        std::string n = de->d_name;
        if (n == "." || n == ".." || strcasecmp(n.c_str(), "log") == 0) continue;
        std::string path = Gk3Dir() + "/" + n;
        struct stat st;
        if (lstat(path.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) continue;
        if (refs.count(Lower(n))) continue;
        doomed.push_back(path);
    }
    d.reset();
    if (!doomed.empty()) p->changed = true;
    if (p->dry) return;
    for (const auto& path : doomed) {
        if (RemoveTree(path)) LOG(INFO) << "gk3boot: removed unreferenced " << path;
    }
}

// 对齐之后还会留在盘上的条目引用了哪些目录：其他人的条目（手放的实验条目等）照算，我们的只算 keep 里的
std::set<std::string> RefsAfter(const EspScan& s, const std::vector<std::string>& keep_versions) {
    std::set<std::string> refs = s.referenced;
    for (const auto& e : s.gk3) refs.erase(Lower(e.version));  // 我们的条目全部重算
    // 别人的条目可能与我们的条目指向同一目录：重扫一遍不是我们的 .conf
    std::unique_ptr<DIR, decltype(&closedir)> d(opendir(EntriesDir().c_str()), &closedir);
    if (d) {
        while (struct dirent* de = readdir(d.get())) {
            std::string n = de->d_name;
            Gk3Entry tmp;
            char slot;
            if (ParseEntryName(n, &tmp) || IsDirectEntry(n, &slot)) continue;
            if (!EndsWith(n, ".conf") && !EndsWith(n, ".conf.staged")) continue;
            std::string content;
            if (!android::base::ReadFileToString(EntriesDir() + "/" + n, &content)) continue;
            std::string dir = EfiDirOf(content);
            if (!dir.empty()) refs.insert(Lower(dir));
        }
    }
    for (const auto& v : keep_versions)
        if (!v.empty()) refs.insert(Lower(v));
    return refs;
}

enum class Mode { kOff, kObserve, kAction, kUnknown };

Mode ParseMode(const std::string& v) {
    if (v.empty() || v == "off") return Mode::kOff;
    if (v == "observe") return Mode::kObserve;
    if (v == "action") return Mode::kAction;
    return Mode::kUnknown;
}

struct EspResult {
    std::string via;  // gk3boot / gk3prev / direct
    bool bypassed = false;
    bool windows = false;          // S15：ESP 上有 bootmgfw.efi
    bool loader_replaced = false;  // S15：有 Windows 且回落路径上的 BOOTAA64.EFI 不再是 systemd-boot
    bool executor = false;         // S15：对齐之后现役那一版（action）的 fastboot.img 在 ESP 上
    std::string mode;  // 对齐之后：off / observe / action / unknown
    std::string version;
    std::string error;
};

// bless：去掉本次启动所用条目的 +N[-M]
void Bless(Pass* p, const std::string& entry) {
    Gk3Entry e;
    if (!ParseEntryName(entry, &e) || e.kind != Kind::kActive || !e.counted) return;
    const std::string from = EntriesDir() + "/" + entry;
    const std::string to = EntriesDir() + "/" + ActiveName(e.slot, false);
    if (access(from.c_str(), F_OK) != 0) {
        if (!p->dry) LOG(WARNING) << "gk3boot: bless: " << entry << " is not on the ESP (renamed meanwhile?)";
        return;
    }
    p->changed = true;
    if (p->dry) return;
    if (rename(from.c_str(), to.c_str()) != 0) {
        PLOG(ERROR) << "gk3boot: bless: rename " << from << " -> " << to;
        return;
    }
    LOG(INFO) << "gk3boot: blessed " << entry << " -> " << ActiveName(e.slot, false);
}

// gk3boot-tools.conf：want（action、这一版的 fastboot.img 已在 ESP 上）⇒ 写成指向 <ver> 的那份（已一样就不写）；
// 否则删掉（observe、这一版不带执行端、执行端没写上）。写失败只记进 *soft —— 它是非默认条目，没有它照常开机。
void AlignTools(Pass* p, const std::vector<Gk3Entry>& tools, bool want, const std::string& ver, std::string* soft) {
    if (!want) {
        for (const auto& e : tools) RemoveEntry(p, e);
        return;
    }
    if (!WriteFileAtomic(p, EntriesDir() + "/" + kToolsEntry, ToolsText(ver))) {
        *soft += (soft->empty() ? "" : "; ") + std::string("write ") + kToolsEntry + " failed";
        return;
    }
    if (!p->dry && (tools.empty() || tools[0].version != ver))
        LOG(INFO) << "gk3boot: " << kToolsEntry << " -> " << ver << " (gk3.action=fastboot)";
}

void Reconcile(Pass* p, Mode mode, EspResult* r) {
    EspScan s = ScanEsp();
    if (!s.ok) {
        r->error = "cannot list loader/entries";
        r->mode = "unknown";
        return;
    }
    std::vector<Gk3Entry> active, staged, prev, tools;
    for (const auto& e : s.gk3) {
        switch (e.kind) {
            case Kind::kActive: active.push_back(e); break;
            case Kind::kStaged: staged.push_back(e); break;
            case Kind::kPrev: prev.push_back(e); break;
            case Kind::kTools: tools.push_back(e); break;
        }
    }
    auto keep_as_is = [&]() {
        r->mode = active.empty() ? "off" : (active[0].observe ? "observe" : "action");
        r->version = active.empty() ? "" : active[0].version;
    };

    if (mode == Mode::kUnknown) {
        if (!p->dry)
            LOG(ERROR) << "gk3boot: " << kModeProp << "=" << GetProperty(kModeProp, "")
                       << " is not off|observe|action; leaving the ESP as it is";
        r->error = std::string(kModeProp) + " invalid";
        keep_as_is();
        return;
    }
    if (mode == Mode::kOff) {
        for (const auto& e : s.gk3) RemoveEntry(p, e);
        CollectGarbage(p, RefsAfter(s, {}));
        r->mode = "off";
        return;
    }

    const bool observe = mode == Mode::kObserve;
    std::string ver;
    if (!android::base::ReadFileToString(kVendorVer, &ver)) {
        r->error = "no /vendor/boot/gk3boot in this build";
        keep_as_is();
        return;
    }
    ver = android::base::Trim(ver);
    if (!ValidVersion(ver)) {
        r->error = "bad /vendor/boot/gk3boot/version";
        keep_as_is();
        return;
    }
    // 入口 fail-open 的去处是直连条目（gk3boot.c 的 oneshot_direct：本槽没有就用另一槽的）—— 一个都没有就不部署
    if (!s.direct[0] && !s.direct[1]) {
        r->error = "no <machine-id>-android-x.conf on ESP";
        keep_as_is();
        return;
    }
    if (!InstallBinary(p, ver, &r->error)) {
        keep_as_is();
        return;
    }
    // 执行端：失败只进 fb_err（最后并进 error），不挡下面的入口部署
    std::string fb_err;
    const FbState fb = InstallFastboot(p, ver, &fb_err);
    if (fb == FbState::kFailed && !p->dry)
        LOG(ERROR) << "gk3boot: fastboot.img not deployed (" << fb_err << "); the loader is deployed anyway, "
                   << "gk3boot boots Android when the executor is missing";
    const bool want_tools = !observe && fb == FbState::kReady;
    auto soft = [&]() {
        if (!fb_err.empty()) r->error += (r->error.empty() ? "" : "; ") + fb_err;
    };

    bool have[2] = {false, false}, up_to_date = !active.empty(), proven = false;
    std::string old_ver = active.empty() ? "" : active[0].version;
    bool old_observe = !active.empty() && active[0].observe;
    for (const auto& e : active) {
        have[e.slot - 'a'] = true;
        if (e.version != ver || e.observe != observe) up_to_date = false;
        if (e.version != old_ver) old_ver.clear();  // 两个条目版本不一致：不当成一个"已知可用的上一版"
        if (!e.counted) proven = true;
    }
    if (!have[0] || !have[1]) up_to_date = false;

    std::string prev_ver = prev.empty() ? "" : prev[0].version;
    if (up_to_date) {
        for (const auto& e : staged) RemoveEntry(p, e);
        AlignTools(p, tools, want_tools, ver, &fb_err);
        CollectGarbage(p, RefsAfter(s, {ver, prev_ver}));
        r->mode = observe ? "observe" : "action";
        r->version = ver;
        soft();
        return;
    }

    // 现役入口是另一版、且在这台机器上被祝福过 ⇒ 它就是"已知可用的上一版"，改成 gk3prev（§4.11 第 3 步）。
    // 没被祝福过（全带计数）的旧版不升格：留着原来的 gk3prev（若有）。
    if (!old_ver.empty() && old_ver != ver && proven && ValidVersion(old_ver)) {
        for (const auto& e : prev) RemoveEntry(p, e);
        for (char x : {'a', 'b'}) {
            if (!WriteFileAtomic(p, EntriesDir() + "/" + PrevName(x), EntryText(Kind::kPrev, x, old_ver, old_observe)))
                r->error = "write " + PrevName(x) + " failed";
        }
        prev_ver = old_ver;
        if (!p->dry) LOG(INFO) << "gk3boot: previous loader " << old_ver << " kept as gk3prev-android-{a,b}.conf";
    }

    // 新的现役条目（带 +3）：先写新的、再删旧的 —— 中途断电最坏是新旧并存，systemd-boot 照样能选
    for (char x : {'a', 'b'}) {
        if (!WriteFileAtomic(p, EntriesDir() + "/" + ActiveName(x, true), EntryText(Kind::kActive, x, ver, observe))) {
            r->error = "write " + ActiveName(x, true) + " failed";
            keep_as_is();
            soft();
            return;  // 旧条目不删：至少还有原来那一套（gk3boot-tools.conf 也不动）
        }
    }
    // gk3boot-tools.conf 跟着现役换到这一版（新现役条目写好之后、删旧的之前）
    AlignTools(p, tools, want_tools, ver, &fb_err);
    for (const auto& e : active) {
        if (e.file != ActiveName(e.slot, true)) RemoveEntry(p, e);
    }
    for (const auto& e : staged) RemoveEntry(p, e);
    CollectGarbage(p, RefsAfter(s, {ver, prev_ver}));
    if (!p->dry)
        LOG(INFO) << "gk3boot: active loader now " << ver << " (" << (observe ? "observe" : "action")
                  << "), entries gk3boot-android-{a,b}+3.conf";
    r->mode = observe ? "observe" : "action";
    r->version = ver;
    soft();
}

// 一遍：bless + 对齐。返回这一遍里算出来的结果；p->changed 表示（dry 时）还有事要做
EspResult OnePass(Pass* p, const std::string& entry, Mode mode) {
    EspResult r;
    r.via = entry.empty() ? "direct" : StartsWith(entry, kPrevPrefix) ? "gk3prev" : "gk3boot";
    MountedEsp esp(p->dry);
    if (!esp.ok()) {
        r.error = "cannot mount ESP";
        r.mode = "unknown";
        return r;
    }
    // bypassed：入口（现役条目）在 ESP 上，这次却是直连开机。在对齐之前看（对齐可能刚把入口装上）
    EspScan before = ScanEsp();
    bool had_active = false;
    for (const auto& e : before.gk3) had_active |= e.kind == Kind::kActive;
    r.bypassed = mode != Mode::kOff && had_active && entry.empty();

    if (!entry.empty()) Bless(p, entry);
    Reconcile(p, mode, &r);
    // S15（动作 6）：有没有 Windows、Windows 有没有换掉回落路径上的启动器、"重启到引导菜单"有没有执行端可去。
    // 都只读；dry 那一遍看的是对齐之前的盘，所以有改动时以第二遍（真写之后）为准（DoEsp 返回的就是那一遍）。
    const std::string root = kEspRoot;
    r.windows = access((root + kBootmgfw).c_str(), F_OK) == 0;
    if (r.windows) {
        std::string a, b;
        if (android::base::ReadFileToString(root + kFallbackLoader, &a) &&
            android::base::ReadFileToString(root + kSdBoot, &b) && !b.empty())
            r.loader_replaced = a != b;
    }
    r.executor = r.mode == "action" && !r.version.empty() &&
                 access((Gk3Dir() + "/" + r.version + "/fastboot.img").c_str(), F_OK) == 0;
    return r;
}

EspResult DoEsp(const std::string& entry, Mode mode) {
    Pass dry;
    EspResult r = OnePass(&dry, entry, mode);
    if (r.bypassed)
        LOG(WARNING) << "gk3boot: loader entries are on the ESP but this boot did not go through gk3boot "
                        "(entry counter used up, direct entry picked in the menu, or fail-open)";
    if (!dry.changed) {
        LOG(INFO) << "gk3boot: ESP already in the wanted state (mode=" << r.mode << " version=" << r.version
                  << "), mounted read-only, nothing written";
        return r;
    }
    Pass real;
    real.dry = false;
    EspResult w = OnePass(&real, entry, mode);
    w.bypassed = r.bypassed;  // 第二遍时入口可能已经被第一遍之后的谁改了；以第一遍（开机时的样子）为准
    return w;
}

void Worker() {
    if (!GetProperty(std::string(kOutPrefix) + "done", "").empty()) {
        LOG(INFO) << "gk3boot: boot-completed work already done this boot (HAL restarted?)";
        return;
    }
    android::base::WaitForProperty(kTrigger, "1");

    const std::string entry = GetProperty("ro.boot.gk3boot.entry", "");
    const std::string event = GetProperty("ro.boot.gk3boot.event", "");
    const Mode mode = ParseMode(GetProperty(kModeProp, ""));
    LOG(INFO) << "gk3boot: boot completed; entry=" << (entry.empty() ? "<direct>" : entry)
              << " event=" << (event.empty() ? "-" : event) << " " << kModeProp << "="
              << GetProperty(kModeProp, "<unset>");

    RecResult rec = ClearStreakAndTakeEvents();
    std::vector<std::string> notify = rec.notify;
    // 记录无效（观察模式不写 misc）时退回 cmdline 的 event；有记录时只认事件环（同一件事只通知一次）
    if (!rec.valid && event == "fallback") notify.push_back(event);

    EspResult esp = DoEsp(entry, mode);
    if (esp.windows && esp.loader_replaced) {
        // 动作 6：只通知、不修（§4.9.6）。经入口进 Android 时这不会发生（回落路径就是入口的来路），U 盘 / live 进来时才有用
        LOG(WARNING) << "gk3boot: " << kFallbackLoader << " differs from " << kSdBoot
                     << " (Windows replaced the fallback loader?); NOT repairing";
        notify.push_back("loader_replaced");
    }

    std::string err = rec.error;
    if (!esp.error.empty()) err += (err.empty() ? "" : "; ") + esp.error;
    if (!err.empty()) LOG(ERROR) << "gk3boot: " << err;

    Out("via", esp.via);
    Out("event", event);
    Out("notify", android::base::Join(notify, ","));
    Out("bypassed", esp.bypassed ? "1" : "0");
    Out("streak", rec.streak < 0 ? "" : std::to_string(rec.streak));
    Out("mode", esp.mode);
    Out("version", esp.version);
    // S15
    Out("windows", esp.windows ? "1" : "0");
    Out("default", rec.valid ? OsName(rec.default_os) : "unknown");
    Out("default_pending", rec.valid ? SetdefName(rec.pending) : "");
    const bool dispatch = GetProperty("ro.boot.gk3boot.dispatch", "") == "1";
    Out("menu", dispatch && esp.via == "gk3boot" && esp.executor ? "1" : "0");
    Out("error", err);
    Out("done", std::to_string(time(nullptr)) + "-" + std::to_string(getpid()));
}

// ──────────────────────────────────────────────────────────────── S15：请求（动作 7）与关机标记

bool EspHasWindows(std::string* err) {
    MountedEsp esp(true);
    if (!esp.ok()) {
        *err = "esp";
        return false;
    }
    return access((std::string(kEspRoot) + kBootmgfw).c_str(), F_OK) == 0;
}

std::string HandleRequest(const std::string& req) {
    gk3_setdef sd = GK3_SETDEF_NONE;
    if (req == "default_windows") sd = GK3_SETDEF_WINDOWS;
    else if (req == "default_android") sd = GK3_SETDEF_ANDROID;
    else if (req != "next_windows") return "error:unknown-request";
    if (sd != GK3_SETDEF_ANDROID) {
        std::string e;
        bool win = EspHasWindows(&e);
        if (!e.empty()) return "error:" + e;
        if (!win) return "error:no-windows";
    }
    bool valid = false;
    std::string err;
    bool ok = RecRmw(
            [&](uint8_t* rec) {
                if (sd == GK3_SETDEF_NONE)
                    gk3_rec_set_next(rec, GK3_NEXT_WINDOWS, 0);
                else
                    gk3_rec_put_set_default_req(rec, sd);
                return true;
            },
            &valid, &err);
    if (!ok) {
        LOG(ERROR) << "gk3boot: request " << req << ": " << err;
        return "error:misc";
    }
    if (!valid) return "error:no-record";
    LOG(INFO) << "gk3boot: request " << req << " written to the GK3 record (read back OK); the loader acts on it "
              << "at the next boot";
    if (sd != GK3_SETDEF_NONE) Out("default_pending", SetdefName(sd));
    return "ok";
}

void RequestWorker() {
    unsigned n = 0;
    for (;;) {
        android::base::WaitForProperty(kRingProp, "1");
        SetProperty(kRingProp, "0");
        const std::string req = GetProperty(kReqProp, "");
        const std::string res = HandleRequest(req);
        Out("ack", req + ":" + res + ":" + std::to_string(getpid()) + "-" + std::to_string(++n));
    }
}

}  // namespace

void StartBootCompletedWorker() {
    std::thread(Worker).detach();
    std::thread(RequestWorker).detach();  // S15：Parts 的"重启到 Windows" / "默认系统"
}

int MarkPoweroffMain() {
    alarm(5);  // 绝不拖住关机：5 秒没完就让 SIGALRM 结束自己（init 的 exec 在等我们）
    const std::string pc = GetProperty("sys.powerctl", "");
    const std::string mode = GetProperty("ro.boot.gk3boot.mode", "");
    if (mode != "action") {
        LOG(INFO) << "gk3boot: mark-poweroff: this boot did not come through the loader in action mode (mode="
                  << (mode.empty() ? "<none>" : mode) << "); nothing to mark";
        return 0;
    }
    bool valid = false, marked = false;
    std::string err;
    bool ok = RecRmw([&](uint8_t* rec) { return marked = gk3_rec_mark_poweroff(rec, pc.c_str()); }, &valid, &err);
    if (!ok)
        LOG(ERROR) << "gk3boot: mark-poweroff: " << err << " (next power-on boots Android)";
    else
        LOG(INFO) << "gk3boot: mark-poweroff: sys.powerctl=" << pc << ": "
                  << (!valid ? "no GK3 record" : marked ? "clean_poweroff set (next power-on goes to Windows)"
                                                        : "not marked (not a shutdown, or the default is not Windows)");
    return 0;
}

}  // namespace gaokun3
