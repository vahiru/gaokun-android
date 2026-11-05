/*
 * gaokun3-ssc-test —— 从 Android 侧直接读 SLPI 传感器
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * 对标 Linux 上的 ssccli。存在的意义：它验证的正是将来 sensors HAL 逻辑的
 * 90%（SUID 查找 → 使能 → 解读数），但不牵扯 AIDL、不牵扯 SensorService，
 * 失败时容易定位。
 *
 * 前提：hexagonrpcd 在跑（否则 QRTR 上没有服务 400）。
 *   /vendor/bin/hexagonrpcd -f /dev/fastrpc-sdsp -d sdsp -s -R /vendor/etc/hexagonrpcd-root
 *
 * 用法：
 *   gaokun3-ssc-test                     读加速度计 10 Hz 10 秒
 *   gaokun3-ssc-test accel 20 5          指定 data_type / 采样率 / 秒数
 *   gaokun3-ssc-test gyro
 *   gaokun3-ssc-test ambient_light 5 10            the streaming light provider
 *   gaokun3-ssc-test accel 50 20 toggle  同会话循环开关：50 Hz、循环 20 轮
 *                                        ★ toggle 模式下第 3 个参数是【轮数】，不是秒数
 * 非 toggle 模式每次都会列出该 data_type 的【全部】UID 并打印其属性（名字、厂商等
 * 字符串/数值），用来分清提供者是物理芯片还是虚拟传感器。
 *
 * toggle 模式（v1.0 计划 HW-3 / LIVE-2 复核意见，上新 sensors HAL 之前的前置实验）：
 *   在【同一个 SscClient】、同一个 UID 上每轮做
 *     EnableContinuous → 收 2 秒（数测量条数、打印首条 X/Y/Z）
 *     → Disable → 静默 2 秒（照样收，停用后仍到达的测量单独计数）
 *   任一轮测量为 0 立刻停止、退出码 3；全部轮次结束后再在同一会话上查一次
 *   FindSensor，枚举坏了退出码 4。调用序列与 sensors-hal/SscHub.cpp 的
 *   PWR-3 启停路径一致（逐条出处见 RunToggle 上方的注释）。
 *   只接受 accel / gyro（HAL 只开这两路）；不与 onchange 组合。
 *   退出码：0 全过；1 打开 / 就绪 / 找传感器 / 使能失败或参数错；2 非 toggle 模式
 *   一条读数都没有；3 toggle 某一轮没读数；4 toggle 结束后 SSC 枚举坏了。
 * data_type 可用值见 docs/dev/sensors-ssc-protocol.md（accel / gyro / mag /
 * ambient_light / proximity / rotv）。
 *
 * ambient_light: tcs3701 registers two providers. The default one is
 *    on-change and is the driver's internal sensor: enabling it makes the SLPI's
 *    sensor_process assert (sns_stream_service.c:436) and the SLPI restarts.
 *    The streaming one works:
 *      gaokun3-ssc-test ambient_light 5 10      (513, picks the streaming provider)
 *    The tool picks the provider whose stream type fits the request and refuses
 *    ambient_light with onchange; toggle mode takes accel / gyro only.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <string>
#include <vector>

#include "ssc_client.h"
#include "ssc-sensor-accelerometer.pb.h"

using gaokun3::SscClient;
using gaokun3::SscReport;

// 收属性应答（msg_id=128）并把每个属性的字符串/数值打出来。最多等 3 秒。
static void PrintAttributes(SscClient* client, const SscUid& uid) {
    for (int t = 0; t < 3; t++) {
        std::vector<SscReport> reports;
        std::string ignore;
        if (!client->ReadReports(&reports, 1000, &ignore)) continue;
        for (size_t i = 0; i < reports.size(); i++) {
            const SscReport& r = reports[i];
            if (r.msg_id != gaokun3::kMsgResponseGetAttributes) continue;
            if (r.uid_low != uid.low() || r.uid_high != uid.high()) continue;
            SscAttrResponse resp;
            if (!resp.ParseFromString(r.payload)) {
                printf("      （属性应答解析失败，%zu 字节）\n", r.payload.size());
                return;
            }
            for (int a = 0; a < resp.attr_size(); a++) {
                const SscAttr& at = resp.attr(a);
                printf("      attr %d:", at.id());
                for (int v = 0; v < at.value_array().v_size(); v++) {
                    const SscAttrValue& x = at.value_array().v(v);
                    if (x.has_s()) printf(" \"%s\"", x.s().c_str());
                    if (x.has_i()) printf(" %lld", static_cast<long long>(x.i()));
                    if (x.has_f()) printf(" %g", x.f());
                    if (x.has_b()) printf(" %s", x.b() ? "true" : "false");
                    if (x.has_a()) printf(" [数组 %d]", x.a().element_size());
                }
                printf("\n");
            }
            return;
        }
    }
    printf("      （3 秒内没收到属性应答）\n");
}

static int64_t NowMs() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<int64_t>(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000;
}

// ---------------------------------------------------------------------------
// toggle 模式：同一会话上反复 EnableContinuous / Disable
//
// 为什么要它：sensors-hal/SscHub.cpp 的 PWR-3 启停路径假设"在同一个 client 上
// 反复开关是无害的"。已有的实测只说明"反复【重建会话】有害"（SscHub.cpp:83-88，
// 2026-08-20），复核者指出那推不出"同会话开关无害"——后者从没实测过。
//
// 调用序列照 HAL 来（行号指 sensors-hal/SscHub.cpp，写本文件时的版本）：
//   * 会话建立：Open → WaitForService(:70-79) → FindSensor 带重试，同一 client 上
//     每 2 秒一次、最多 30 次（:94-99；物理传感器比 registry 晚注册）。
//     HAL 不发属性请求，所以 toggle 模式也不发。
//   * 使能：EnableContinuous(uid, 速率)（:144-145，HAL 固定 kRateHz=50，:16）。
//     ★ 重新使能前【不】重新 FindSensor，沿用会话开始时查到的 UID（:92-105 只在
//     会话建立时查一次），也没有任何额外请求。
//   * 停用：Disable(uid)，即 msg_id=10（:146；ssc_client.cpp 的 Disable）。
//     失败只打日志、照样当作已停用（:147-149），这里同样只警告不退出。
//   * 停用后：HAL 读掉 kDrainMs=500 ms 的在途上报（:21、:114-122、:166），然后
//     睡在条件变量上【不再读 socket】，直到下次 activate。这里静默窗口照样收
//     2 秒，把 500 ms 之后才到的测量另记一笔：那些在 HAL 里会留在 socket 里，
//     下次使能后被当成新样本写进缓存（:206-210 只核对 uid，不看时间戳）。
//   * HAL 的看门狗（15 秒无读数在同一会话重新使能、60 秒重建，:180-187）在
//     2 秒窗口里不会触发，这里不模拟 —— 判据就是"每轮 2 秒内必须有读数"。
// ---------------------------------------------------------------------------
namespace {
constexpr int kToggleOnMs = 2000;    // 每轮使能后收数的窗口
constexpr int kToggleOffMs = 2000;   // 每轮停用后的静默窗口
constexpr int kHalDrainMs = 500;     // = SscHub.cpp 的 kDrainMs（:21）

struct WindowStats {
    int meas = 0;          // 本 UID 的测量（1025）条数
    int meas_late = 0;     // 其中窗口开始 kHalDrainMs 之后才到的
    int other = 0;         // 其它消息（非 1025，或别的 UID 的）
    bool have_first = false;
    float first[3] = {0.f, 0.f, 0.f};
    int first_accuracy = 0;
};

// 在 window_ms 内反复 ReadReports，按 uid 统计测量条数。
// 每次 ReadReports 的超时不超过窗口剩余时间，免得窗口被拉长到 3 秒。
void CollectWindow(SscClient* client, const SscUid& uid, int window_ms,
                   WindowStats* st) {
    const int64_t start = NowMs();
    const int64_t deadline = start + window_ms;
    for (int64_t now = start; now < deadline; now = NowMs()) {
        int64_t left = deadline - now;
        if (left > 1000) left = 1000;
        std::vector<SscReport> reports;
        std::string ignore;
        if (!client->ReadReports(&reports, static_cast<int>(left), &ignore)) continue;
        const bool late = (NowMs() - start) >= kHalDrainMs;
        for (size_t i = 0; i < reports.size(); i++) {
            const SscReport& r = reports[i];
            if (r.msg_id != gaokun3::kMsgReportMeasurement ||
                r.uid_low != uid.low() || r.uid_high != uid.high()) {
                st->other++;
                continue;
            }
            st->meas++;
            if (late) st->meas_late++;
            if (st->have_first) continue;
            SscAccelerometerResponse m;
            if (m.ParseFromString(r.payload) && m.acceleration_size() >= 3) {
                st->first[0] = m.acceleration(0);
                st->first[1] = m.acceleration(1);
                st->first[2] = m.acceleration(2);
                st->first_accuracy = m.accuracy();
                st->have_first = true;
            }
        }
    }
}

int RunToggle(const std::string& data_type, float rate_hz, int rounds) {
    SscClient client;
    std::string err;

    if (!client.Open(&err)) {
        fprintf(stderr, "打开失败: %s\n", err.c_str());
        return 1;
    }
    printf("SSC 服务 400 在 node %u port %u\n", client.service_node(),
           client.service_port());
    printf("等 SSC 就绪（最多 40 秒，刚重启过 hexagonrpcd 时确实要等）…\n");
    if (!client.WaitForService(40000, &err)) {
        fprintf(stderr, "SSC 没就绪: %s\n", err.c_str());
        return 1;
    }

    // 同 SscHub.cpp:94-99：在同一个 client 上每 2 秒查一次，最多 30 次
    SscUid uid;
    bool found = false;
    for (int t = 0; t < 30 && !found; t++) {
        found = client.FindSensor(data_type, &uid, &err);
        if (!found) sleep(2);
    }
    if (!found) {
        fprintf(stderr, "60 秒内找不到传感器 %s: %s\n", data_type.c_str(),
                err.c_str());
        return 1;
    }
    printf("传感器 %s 的 UID = %016llx%016llx\n", data_type.c_str(),
           static_cast<unsigned long long>(uid.high()),
           static_cast<unsigned long long>(uid.low()));
    printf("toggle：%.1f Hz，%d 轮；每轮 使能收 %d ms → 停用静默 %d ms，"
           "全程同一个 client、同一个 UID\n",
           rate_hz, rounds, kToggleOnMs, kToggleOffMs);

    int total_meas = 0, min_meas = -1, max_meas = 0;
    int total_resid = 0, total_resid_late = 0;
    for (int n = 1; n <= rounds; n++) {
        if (!client.EnableContinuous(uid, rate_hz, &err)) {
            fprintf(stderr, "第 %d 轮：使能失败: %s\n", n, err.c_str());
            return 1;
        }
        WindowStats on;
        CollectWindow(&client, uid, kToggleOnMs, &on);

        if (!client.Disable(uid, &err))
            fprintf(stderr, "第 %d 轮：停用请求发送失败（照 HAL 继续）: %s\n", n,
                    err.c_str());

        if (on.meas == 0) {
            printf("第 %d 轮：测量 0 条（其它消息 %d 条）\n", n, on.other);
            fprintf(stderr,
                    "第 %d 轮 %d ms 内一条测量都没有 —— 同会话开关【不】无害，"
                    "停止。\n恢复：重启 hexagonrpcd 再等约 20 秒（见 ssc/README.md "
                    "的 toggle 一节）。\n",
                    n, kToggleOnMs);
            return 3;
        }

        WindowStats off;
        CollectWindow(&client, uid, kToggleOffMs, &off);

        printf("第 %d 轮：测量 %d 条，停用后残留 %d 条", n, on.meas, off.meas);
        if (off.meas > 0) printf("（其中 %d ms 后 %d 条）", kHalDrainMs, off.meas_late);
        if (on.have_first) {
            printf("  首条 X=%9.6f Y=%9.6f Z=%9.6f accuracy=%d", on.first[0],
                   on.first[1], on.first[2], on.first_accuracy);
        } else {
            printf("  首条解析失败");
        }
        if (on.other > 0 || off.other > 0)
            printf("  其它消息 %d / %d", on.other, off.other);
        printf("\n");
        fflush(stdout);

        total_meas += on.meas;
        if (min_meas < 0 || on.meas < min_meas) min_meas = on.meas;
        if (on.meas > max_meas) max_meas = on.meas;
        total_resid += off.meas;
        total_resid_late += off.meas_late;
    }

    // 判据之一"SSC 枚举不坏"：全部轮次结束后在同一会话上再查一次。
    // ⚠️ 这一步不是 HAL 的序列（HAL 只在会话建立时查），只是事后体检。
    SscUid again;
    const bool enum_ok = client.FindSensor(data_type, &again, &err);

    printf("\n汇总：%d 轮全部有读数；每轮测量 %d–%d 条、共 %d 条；"
           "停用后残留共 %d 条（其中 %d ms 后 %d 条）\n",
           rounds, min_meas, max_meas, total_meas, total_resid, kHalDrainMs,
           total_resid_late);
    if (!enum_ok) {
        fprintf(stderr, "结束后再查 %s 失败: %s —— SSC 枚举坏了\n",
                data_type.c_str(), err.c_str());
        return 4;
    }
    if (again.low() != uid.low() || again.high() != uid.high())
        printf("⚠️ 结束后查到的 UID 与开头不同: %016llx%016llx\n",
               static_cast<unsigned long long>(again.high()),
               static_cast<unsigned long long>(again.low()));
    printf("结束后 SSC 枚举正常（%s 仍可查到）\n", data_type.c_str());
    if (total_resid_late > 0)
        printf("注意：有 %d 条测量在停用 %d ms 后才到 —— HAL 停用后只读 %d ms，"
               "这些会在下次使能时被当成新样本（见 RunToggle 上方注释）\n",
               total_resid_late, kHalDrainMs, kHalDrainMs);
    // 静止平放时 accel 的 Z 应 ≈ 9.8 m/s²，gyro 各轴 ≈ 0 —— 看上面每轮的首条
    return 0;
}
}  // namespace

int main(int argc, char** argv) {
    const std::string data_type = (argc > 1) ? argv[1] : "accel";
    const float rate_hz = (argc > 2) ? strtof(argv[2], nullptr) : 10.0f;
    const int seconds = (argc > 3) ? atoi(argv[3]) : 10;   // toggle 模式下是轮数
    const std::string mode = (argc > 4) ? argv[4] : "";

    if (!mode.empty() && mode != "onchange" && mode != "toggle") {
        fprintf(stderr, "第 4 个参数只认 onchange 或 toggle，收到 \"%s\"\n",
                mode.c_str());
        return 1;
    }
    if (argc > 5) {
        fprintf(stderr, "参数太多：toggle 不与 onchange 组合，第 4 个参数只能二选一\n");
        return 1;
    }
    if (mode == "toggle") {
        // Toggle mode enables the default provider (FindSensor), which for
        // ambient_light is the one that crashes the SLPI (see the file header).
        if (data_type != "accel" && data_type != "gyro") {
            fprintf(stderr, "toggle 模式只接受 accel / gyro（收到 %s）。"
                            "ambient_light 尤其不行：默认的那个光感一使能 SLPI 就崩溃\n",
                    data_type.c_str());
            return 1;
        }
        if (seconds < 1 || rate_hz <= 0.f) {
            fprintf(stderr, "toggle 模式要求 采样率 > 0、轮数 ≥ 1\n");
            return 1;
        }
        return RunToggle(data_type, rate_hz, seconds);
    }

    SscClient client;
    std::string err;

    if (!client.Open(&err)) {
        fprintf(stderr, "打开失败: %s\n", err.c_str());
        return 1;
    }
    printf("SSC 服务 400 在 node %u port %u\n", client.service_node(),
           client.service_port());

    // hexagonrpcd 刚起来时 SSC 要沉降约 20 秒，给足 40 秒
    printf("等 SSC 就绪（最多 40 秒，刚重启过 hexagonrpcd 时确实要等）…\n");
    if (!client.WaitForService(40000, &err)) {
        fprintf(stderr, "SSC 没就绪: %s\n", err.c_str());
        return 1;
    }
    printf("SSC 已就绪\n");

    std::vector<SscUid> uids;
    if (!client.FindSensors(data_type, &uids, &err)) {
        fprintf(stderr, "找不到传感器 %s: %s\n", data_type.c_str(), err.c_str());
        return 1;
    }
    printf("data_type=%s 共 %zu 个提供者\n", data_type.c_str(), uids.size());
    for (size_t k = 0; k < uids.size(); k++) {
        printf("  [%zu] UID = %016llx%016llx\n", k,
               static_cast<unsigned long long>(uids[k].high()),
               static_cast<unsigned long long>(uids[k].low()));
        if (client.RequestAttributes(uids[k], &err)) PrintAttributes(&client, uids[k]);
    }
    const bool on_change = (mode == "onchange");
    if (on_change && data_type == "ambient_light") {
        fprintf(stderr, "ambient_light 不能用 onchange：变化时上报的那个光感一使能 SLPI 就崩溃，"
                        "用连续上报（去掉 onchange）\n");
        return 1;
    }
    // Several providers can share a data_type (ambient_light has two); use the
    // first whose stream type (attribute 16) fits the request: 0 (streaming)
    // for 513, 1 (on-change) for 514.
    SscUid uid = uids[0];
    for (const SscUid& u : uids) {
        int64_t type = -1;
        std::string e;
        if (client.GetIntAttribute(u, 16, &type, &e) && type == (on_change ? 1 : 0)) {
            uid = u;
            break;
        }
    }
    printf("传感器 %s 的 UID = %016llx%016llx\n", data_type.c_str(),
           static_cast<unsigned long long>(uid.high()),
           static_cast<unsigned long long>(uid.low()));
    const bool ok = on_change ? client.EnableOnChange(uid, rate_hz, &err)
                              : client.EnableContinuous(uid, rate_hz, &err);
    if (!ok) {
        fprintf(stderr, "使能失败: %s\n", err.c_str());
        return 1;
    }
    printf("已请求 %.1f Hz %s上报，收 %d 秒\n", rate_hz,
           on_change ? "变化时（514）" : "连续（513）", seconds);

    const int64_t deadline = NowMs() + seconds * 1000;
    int n_meas = 0, n_other = 0;
    while (NowMs() < deadline) {
        std::vector<SscReport> reports;
        std::string ignore;
        if (!client.ReadReports(&reports, 1000, &ignore)) continue;
        for (size_t i = 0; i < reports.size(); i++) {
            const SscReport& r = reports[i];
            if (r.msg_id != gaokun3::kMsgReportMeasurement) {
                n_other++;
                printf("  [其它消息] msg_id=%u  %zu 字节:", r.msg_id,
                       r.payload.size());
                // 载荷原样十六进制打出来（前 32 字节），不猜它是什么结构
                for (size_t j = 0; j < r.payload.size() && j < 32; j++)
                    printf(" %02x", static_cast<unsigned char>(r.payload[j]));
                printf("\n");
                continue;
            }
            n_meas++;
            if (data_type == "accel" || data_type == "gyro" ||
                data_type == "mag") {
                // 这三个的载荷布局相同：repeated float + accuracy
                SscAccelerometerResponse m;
                if (!m.ParseFromString(r.payload)) {
                    printf("  [解析失败，%zu 字节]\n", r.payload.size());
                    continue;
                }
                if (m.acceleration_size() >= 3) {
                    printf("  X=%9.6f Y=%9.6f Z=%9.6f  accuracy=%d\n",
                           m.acceleration(0), m.acceleration(1),
                           m.acceleration(2), m.accuracy());
                } else {
                    printf("  [只有 %d 个分量]\n", m.acceleration_size());
                }
            } else {
                // 其它传感器的事件也是 repeated float（字段 1）+ accuracy，
                // 环境光的第 0 个就是 lux —— 全部打出来，不猜含义。
                SscAccelerometerResponse m;
                if (!m.ParseFromString(r.payload)) {
                    printf("  msg_id=%u  %zu 字节（解析失败）\n", r.msg_id,
                           r.payload.size());
                    continue;
                }
                printf("  data[%d] =", m.acceleration_size());
                for (int j = 0; j < m.acceleration_size(); j++)
                    printf(" %.3f", m.acceleration(j));
                printf("  accuracy=%d\n", m.accuracy());
            }
        }
    }

    client.Disable(uid, &err);
    printf("\n共 %d 条测量、%d 条其它消息\n", n_meas, n_other);
    if (n_meas == 0) {
        fprintf(stderr,
                "一条读数都没有。排查顺序：\n"
                "  1) gaokun3-qrtr-lookup 400 —— 服务还在吗\n"
                "  2) hexagonrpcd 的日志里 DSP 还在请求文件吗\n"
                "  3) 之前是不是试过 ambient_light？它会污染整个会话，"
                "重启 hexagonrpcd 再等 20 秒\n");
        return 2;
    }
    // 静止平放时加速度计 Z 应该 ≈ 9.8 m/s²，这是整条通路的硬判据
    return 0;
}
