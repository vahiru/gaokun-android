/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 * 见 ssc_client.h 的说明与 docs/dev/sensors-ssc-protocol.md 的协议规格。
 */
#include "ssc_client.h"

#include <errno.h>
#include <poll.h>
#include <string.h>
#include <unistd.h>

#include <sys/socket.h>
#include <linux/qrtr.h>

#include <algorithm>

#include "ssc-sensor-suid.pb.h"

namespace gaokun3 {
namespace {

// QRTR 上的 QMI 没有 QMUX 头，也没有 libqmi 内部那个 marker 字节。
// 上线字节 = 7 字节 service_header + TLV
//   service_header: flags(u8) transaction(u16) message(u16) tlv_length(u16)
//   tlv           : type(u8) length(u16) value[]
// 出处：qmi-endpoint-qrtr.c:547 的注释与 qmi_message_get_data()，
//       qmi-message.c:98-108 的两个结构体。
constexpr size_t kQmiHeaderLen = 7;

constexpr uint8_t kFlagRequest = 0x00;      // qmi-enums-private.h:79-84
constexpr uint8_t kFlagResponse = 0x02;
constexpr uint8_t kFlagIndication = 0x04;

// SSC 服务的三条消息（libqmi/data/qmi-service-ssc.json）
constexpr uint16_t kMsgControl = 0x0020;
constexpr uint16_t kMsgReportSmall = 0x0021;
constexpr uint16_t kMsgReportLarge = 0x0022;

constexpr uint8_t kTlvControlData = 0x01;        // u16 长度前缀 + 字节流
constexpr uint8_t kTlvControlReportType = 0x10;  // u8
constexpr uint8_t kTlvReportData = 0x02;         // u16 长度前缀 + 字节流

constexpr uint8_t kReportTypeSmall = 0x00;       // qmi-enums-ssc.h
constexpr uint32_t kQmiServiceSsc = 400;

void Push16(std::vector<uint8_t>* v, uint16_t x) {
    v->push_back(static_cast<uint8_t>(x & 0xff));
    v->push_back(static_cast<uint8_t>(x >> 8));
}

uint16_t Read16(const uint8_t* p) {
    return static_cast<uint16_t>(p[0] | (p[1] << 8));
}

}  // namespace

SscClient::~SscClient() {
    if (fd_ >= 0) close(fd_);
}

bool SscClient::Open(std::string* err) {
    fd_ = socket(AF_QIPCRTR, SOCK_DGRAM, 0);
    if (fd_ < 0) {
        *err = std::string("socket(AF_QIPCRTR): ") + strerror(errno) +
               " (内核缺 CONFIG_QRTR 时是 EAFNOSUPPORT)";
        return false;
    }

    struct sockaddr_qrtr sq;
    memset(&sq, 0, sizeof(sq));
    socklen_t slen = sizeof(sq);
    if (getsockname(fd_, reinterpret_cast<struct sockaddr*>(&sq), &slen) < 0) {
        *err = std::string("getsockname: ") + strerror(errno);
        return false;
    }
    const uint32_t local_node = sq.sq_node;

    struct qrtr_ctrl_pkt pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.cmd = QRTR_TYPE_NEW_LOOKUP;
    pkt.server.service = kQmiServiceSsc;

    struct sockaddr_qrtr ctrl;
    memset(&ctrl, 0, sizeof(ctrl));
    ctrl.sq_family = AF_QIPCRTR;
    ctrl.sq_node = local_node;
    ctrl.sq_port = QRTR_PORT_CTRL;
    if (sendto(fd_, &pkt, sizeof(pkt), 0,
               reinterpret_cast<struct sockaddr*>(&ctrl), sizeof(ctrl)) < 0) {
        *err = std::string("sendto(NEW_LOOKUP): ") + strerror(errno);
        return false;
    }

    for (;;) {
        struct pollfd p;
        p.fd = fd_;
        p.events = POLLIN;
        p.revents = 0;
        // 服务不存在时全零终止包不会来，所以必须带超时，不能死等
        if (poll(&p, 1, 2000) <= 0) break;
        struct qrtr_ctrl_pkt rp;
        memset(&rp, 0, sizeof(rp));
        if (recv(fd_, &rp, sizeof(rp), 0) < static_cast<ssize_t>(sizeof(rp)))
            continue;
        if (rp.cmd != QRTR_TYPE_NEW_SERVER) continue;
        if (!rp.server.service && !rp.server.node && !rp.server.port) break;
        if (rp.server.service == kQmiServiceSsc) {
            node_ = rp.server.node;
            port_ = rp.server.port;
            return true;
        }
    }

    *err = "QRTR 上找不到 SSC 服务 400。hexagonrpcd 在跑吗？"
           "用 gaokun3-qrtr-lookup 400 确认。";
    return false;
}

bool SscClient::SendRequest(uint64_t uid_low, uint64_t uid_high, uint32_t msg_id,
                            const std::string& sub_msg, std::string* err) {
    SscClientRequest req;
    req.mutable_uid()->set_low(uid_low);
    req.mutable_uid()->set_high(uid_high);
    req.set_msg_id(msg_id);
    // proto2 的 required 字段即使带 default 也必须显式设置，否则序列化失败
    req.mutable_config()->set_processor(1);     // APPS
    req.mutable_config()->set_suspend_mode(0);  // WAKEUP
    if (!sub_msg.empty())
        req.mutable_request()->set_msg(sub_msg);
    else
        req.mutable_request();  // request 也是 required，必须存在

    std::string pb;
    if (!req.SerializeToString(&pb)) {
        *err = "SscClientRequest 序列化失败，proto2 required 字段没填全";
        return false;
    }

    std::vector<uint8_t> tlvs;
    tlvs.push_back(kTlvControlData);
    Push16(&tlvs, static_cast<uint16_t>(pb.size() + 2));
    Push16(&tlvs, static_cast<uint16_t>(pb.size()));
    tlvs.insert(tlvs.end(), pb.begin(), pb.end());
    tlvs.push_back(kTlvControlReportType);
    Push16(&tlvs, 1);
    tlvs.push_back(kReportTypeSmall);

    std::vector<uint8_t> sdu;
    sdu.push_back(kFlagRequest);
    Push16(&sdu, txn_++);
    Push16(&sdu, kMsgControl);
    Push16(&sdu, static_cast<uint16_t>(tlvs.size()));
    sdu.insert(sdu.end(), tlvs.begin(), tlvs.end());

    struct sockaddr_qrtr dst;
    memset(&dst, 0, sizeof(dst));
    dst.sq_family = AF_QIPCRTR;
    dst.sq_node = node_;
    dst.sq_port = port_;
    if (sendto(fd_, sdu.data(), sdu.size(), 0,
               reinterpret_cast<struct sockaddr*>(&dst), sizeof(dst)) < 0) {
        *err = std::string("sendto(SSC Control): ") + strerror(errno);
        return false;
    }
    return true;
}

bool SscClient::RecvSdu(std::vector<uint8_t>* sdu, int timeout_ms) {
    struct pollfd p;
    p.fd = fd_;
    p.events = POLLIN;
    p.revents = 0;
    if (poll(&p, 1, timeout_ms) <= 0) return false;
    sdu->assign(64 * 1024, 0);
    ssize_t n = recv(fd_, sdu->data(), sdu->size(), 0);
    if (n < static_cast<ssize_t>(kQmiHeaderLen)) return false;
    sdu->resize(static_cast<size_t>(n));
    return true;
}

bool SscClient::FindTlv(const std::vector<uint8_t>& sdu, uint8_t type,
                        const uint8_t** val, uint16_t* len) {
    if (sdu.size() < kQmiHeaderLen) return false;
    const uint16_t tlv_total = Read16(&sdu[5]);
    size_t off = kQmiHeaderLen;
    const size_t end = std::min(sdu.size(), kQmiHeaderLen + tlv_total);
    while (off + 3 <= end) {
        const uint8_t t = sdu[off];
        const uint16_t l = Read16(&sdu[off + 1]);
        if (off + 3 + l > end) return false;
        if (t == type) {
            *val = &sdu[off + 3];
            *len = l;
            return true;
        }
        off += 3 + l;
    }
    return false;
}

bool SscClient::ReadReports(std::vector<SscReport>* out, int timeout_ms,
                            std::string* err) {
    std::vector<uint8_t> sdu;
    if (!RecvSdu(&sdu, timeout_ms)) return false;

    const uint8_t flags = sdu[0];
    const uint16_t msg = Read16(&sdu[3]);

    // Control 的响应里是 Operation Result / Client ID / Response，对读数没用
    if (flags & kFlagResponse) return false;
    if (!(flags & kFlagIndication)) return false;
    if (msg != kMsgReportSmall && msg != kMsgReportLarge) return false;

    const uint8_t* val = nullptr;
    uint16_t len = 0;
    if (!FindTlv(sdu, kTlvReportData, &val, &len) || len < 2) return false;
    const uint16_t pb_len = Read16(val);
    if (static_cast<size_t>(pb_len) + 2 > len) return false;

    SscClientResponse resp;
    if (!resp.ParseFromArray(val + 2, pb_len)) {
        *err = "SscClientResponse 解析失败";
        return false;
    }
    for (int i = 0; i < resp.response_size(); i++) {
        const SscClientResponseBody& body = resp.response(i);
        SscReport r;
        r.msg_id = body.msg_id();
        r.timestamp = body.timestamp();
        r.uid_low = resp.uid().low();
        r.uid_high = resp.uid().high();
        r.payload = body.msg();
        out->push_back(r);
    }
    return !out->empty();
}

bool SscClient::WaitForService(int timeout_ms, std::string* err) {
    // libssc 的判据：向哨兵查 data_type 为 registry，能应答就说明 SSC 活了
    // (libssc-sensor.c:297,318,627-631)
    SscSuidRequest suid;
    suid.set_data_type("registry");
    std::string sub;
    suid.SerializeToString(&sub);

    int waited = 0;
    while (waited < timeout_ms) {
        if (!SendRequest(kSuidSentinel, kSuidSentinel, kMsgRequestSuid, sub, err))
            return false;
        std::vector<SscReport> reports;
        std::string ignore;
        if (ReadReports(&reports, 1000, &ignore)) {
            for (size_t i = 0; i < reports.size(); i++)
                if (reports[i].msg_id == kMsgResponseSuid) return true;
        }
        waited += 1000;
    }
    *err = "等 SSC 服务超时。hexagonrpcd 重启后约需 20 秒沉降，"
           "把超时放大再试；实测 6 秒就读会拿到 0 行。";
    return false;
}

bool SscClient::FindSensor(const std::string& data_type, SscUid* out,
                           std::string* err) {
    std::vector<SscUid> all;
    if (!Lookup(data_type, true, &all, err)) return false;
    *out = all[0];
    return true;
}

bool SscClient::FindSensors(const std::string& data_type,
                            std::vector<SscUid>* out, std::string* err) {
    return Lookup(data_type, false, out, err);
}

bool SscClient::Lookup(const std::string& data_type, bool default_only,
                       std::vector<SscUid>* out, std::string* err) {
    SscSuidRequest suid;
    suid.set_data_type(data_type);
    suid.set_only_default_values(default_only);
    std::string sub;
    suid.SerializeToString(&sub);
    if (!SendRequest(kSuidSentinel, kSuidSentinel, kMsgRequestSuid, sub, err))
        return false;

    for (int i = 0; i < 10; i++) {
        std::vector<SscReport> reports;
        std::string ignore;
        if (!ReadReports(&reports, 1000, &ignore)) continue;
        for (size_t k = 0; k < reports.size(); k++) {
            if (reports[k].msg_id != kMsgResponseSuid) continue;
            SscSuidResponse sr;
            if (!sr.ParseFromString(reports[k].payload)) continue;
            // 可能收到的是别的 data_type 查询的应答，要核对
            if (sr.data_type() != data_type) continue;
            if (sr.uid_size() == 0) {
                *err = "SSC 说没有传感器提供 data_type=" + data_type;
                return false;
            }
            out->assign(sr.uid().begin(), sr.uid().end());
            return true;
        }
    }
    *err = "查 data_type=" + data_type + " 没有收到应答";
    return false;
}

bool SscClient::EnableContinuous(const SscUid& uid, float rate_hz,
                                 std::string* err) {
    SscEnableConfigRequest cfg;
    cfg.set_sample_rate(rate_hz);
    std::string sub;
    cfg.SerializeToString(&sub);
    return SendRequest(uid.low(), uid.high(), kMsgRequestEnableContinuous, sub,
                       err);
}

bool SscClient::EnableOnChange(const SscUid& uid, float rate_hz,
                               std::string* err) {
    // libssc 的 on-change 使能【不带载荷】（libssc-sensor.c:231-235，
    // "on-change do not need any configuration"）。rate<=0 时照它发空载荷；
    // rate>0 时仍带 SscEnableConfigRequest，留作对照。
    std::string sub;
    if (rate_hz > 0) {
        SscEnableConfigRequest cfg;
        cfg.set_sample_rate(rate_hz);
        cfg.SerializeToString(&sub);
    }
    return SendRequest(uid.low(), uid.high(), kMsgRequestEnableOnChange, sub,
                       err);
}

bool SscClient::RequestAttributes(const SscUid& uid, std::string* err) {
    SscAttrRequest req;
    std::string sub;
    req.SerializeToString(&sub);
    return SendRequest(uid.low(), uid.high(), kMsgRequestGetAttributes, sub,
                       err);
}

bool SscClient::GetIntAttribute(const SscUid& uid, int32_t attr_id,
                                int64_t* value, std::string* err) {
    if (!RequestAttributes(uid, err)) return false;
    for (int t = 0; t < 3; t++) {
        std::vector<SscReport> reports;
        std::string ignore;
        if (!ReadReports(&reports, 1000, &ignore)) continue;
        for (const SscReport& r : reports) {
            if (r.msg_id != kMsgResponseGetAttributes) continue;
            if (r.uid_low != uid.low() || r.uid_high != uid.high()) continue;
            SscAttrResponse resp;
            if (!resp.ParseFromString(r.payload)) {
                *err = "attribute response does not parse";
                return false;
            }
            for (int a = 0; a < resp.attr_size(); a++) {
                const SscAttr& at = resp.attr(a);
                if (at.id() != attr_id) continue;
                for (int v = 0; v < at.value_array().v_size(); v++) {
                    if (!at.value_array().v(v).has_i()) continue;
                    *value = static_cast<int64_t>(at.value_array().v(v).i());
                    return true;
                }
            }
            *err = "no integer attribute " + std::to_string(attr_id);
            return false;
        }
    }
    *err = "no attribute response within 3 s";
    return false;
}

bool SscClient::Disable(const SscUid& uid, std::string* err) {
    return SendRequest(uid.low(), uid.high(), kMsgRequestDisableReport, "", err);
}

}  // namespace gaokun3
