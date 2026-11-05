/*
 * SscClient —— 极简 SSC 客户端（QMI over QRTR + protobuf）
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * 为什么不用 libssc：它依赖 glib / gio / gobject / qmi-glib / libprotobuf-c，
 * 整套 GLib 栈搬不进 Android。协议逻辑本身很薄，照规格重写便宜得多。
 *
 * ★ 协议规格（每条事实都注了来源文件与行号）见 docs/dev/sensors-ssc-protocol.md。
 *   本文件里凡是"魔数"都能在那份文档里查到出处，没有一个是猜的。
 *
 * 许可证说明：本类使用 libssc 的 .proto（GPL-3.0），因此本身也是 GPL-3.0。
 * hexagonrpcd 同样是 GPL-3.0，vendor 分区里已经有先例。
 */
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "ssc-common.pb.h"

namespace gaokun3 {

// protobuf 层的消息 ID（libssc/src/libssc-sensor*-private.h）
enum : uint32_t {
    kMsgRequestGetAttributes = 1,
    kMsgRequestDisableReport = 10,
    kMsgResponseGetAttributes = 128,
    kMsgRequestSuid = 512,
    kMsgRequestEnableContinuous = 513,
    kMsgRequestEnableOnChange = 514,
    // ⚠️ SUID 响应与"使能已生效"响应共用 768，靠 uid 区分是哪一个
    kMsgResponseSuid = 768,
    kMsgResponseEnableReport = 768,
    kMsgReportMeasurementProximity = 769,
    kMsgReportMeasurement = 1025,
};

// SUID 查找用的哨兵 UID：向这个"虚拟传感器"提问，
// 问某个 data_type 由哪些真实传感器提供。
constexpr uint64_t kSuidSentinel = 0xABABABABABABABABULL;

struct SscReport {
    uint32_t msg_id = 0;
    uint64_t timestamp = 0;   // DSP 内部计时器，19.2 MHz
    uint64_t uid_low = 0;
    uint64_t uid_high = 0;
    std::string payload;      // 传感器专属的 protobuf
};

class SscClient {
  public:
    ~SscClient();

    // 打开 AF_QIPCRTR socket，并用 QRTR lookup 找到 SSC 服务（400）的 node/port。
    bool Open(std::string* err);

    // 反复向哨兵查 data_type="registry"，直到 SSC 应答 —— libssc 就是这么判断
    // "SSC 活了没有"的。
    // ⚠️ hexagonrpcd 起来后约需 20 秒沉降，实测 6 秒就读会拿到 0 行。
    //    别把超时设成几秒然后判定失败。
    bool WaitForService(int timeout_ms, std::string* err);

    // 查某个 data_type 对应的真实传感器 UID（多个时取第一个）。
    // Asks for the default provider only (the SUID sensor's own default for the
    // request), which is what registry default_sensors.<data_type> selects.
    bool FindSensor(const std::string& data_type, SscUid* out, std::string* err);
    // 同上，但返回全部 UID（一个 data_type 可能有多个提供者，比如物理芯片 + 虚拟传感器）。
    // Sends only_default_values = false: SEE defaults that field to true, and
    // without it only the default provider comes back.
    bool FindSensors(const std::string& data_type, std::vector<SscUid>* out,
                     std::string* err);

    // 发属性请求（msg_id=1）；应答是 msg_id=128 的 SscAttrResponse，由 ReadReports 收。
    bool RequestAttributes(const SscUid& uid, std::string* err);

    // Request the attributes of uid and return the first integer value of
    // attribute attr_id (for example 16, SNS_STD_SENSOR_ATTRID_STREAM_TYPE).
    // Reports for other sensors that arrive meanwhile are dropped, so call it
    // before enabling anything on this client.
    bool GetIntAttribute(const SscUid& uid, int32_t attr_id, int64_t* value,
                         std::string* err);

    // 变化时上报（msg_id=514）。环境光、接近这类 on-change 传感器要用这个，不是 513。
    bool EnableOnChange(const SscUid& uid, float rate_hz, std::string* err);

    // 以指定采样率使能连续上报。
    bool EnableContinuous(const SscUid& uid, float rate_hz, std::string* err);

    // 停止上报。
    bool Disable(const SscUid& uid, std::string* err);

    // 收一批上报（一条 QMI 指示里可能带多条）。超时返回 false 且不置 err。
    bool ReadReports(std::vector<SscReport>* out, int timeout_ms, std::string* err);

    uint32_t service_node() const { return node_; }
    uint32_t service_port() const { return port_; }

  private:
    bool Lookup(const std::string& data_type, bool default_only,
                std::vector<SscUid>* out, std::string* err);
    bool SendRequest(uint64_t uid_low, uint64_t uid_high, uint32_t msg_id,
                     const std::string& sub_msg, std::string* err);
    bool RecvSdu(std::vector<uint8_t>* sdu, int timeout_ms);
    static bool FindTlv(const std::vector<uint8_t>& sdu, uint8_t type,
                        const uint8_t** val, uint16_t* len);

    int fd_ = -1;
    uint32_t node_ = 0;
    uint32_t port_ = 0;
    uint16_t txn_ = 1;
};

}  // namespace gaokun3
