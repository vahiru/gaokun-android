// SPDX-License-Identifier: Apache-2.0
// Copyright (C) 2026 Akisaira
//
// gk3denoised: noise suppression service (DeepFilterNet3 on the CDSP/HTP v68 through QNN) for the
// capture hook of the audio HAL.
//
// Protocol: denoise_protocol.h, copied unchanged from the HAL tree
// (build/ga-pr-micarray/device/huawei/gaokun3/micarray/denoise_protocol.h, sha256 30d69f1d...).
// One SOCK_SEQPACKET connection per stream: Hello -> HelloReply, then one answer per BlockHeader +
// hops of interleaved S16, each channel filtered independently (dfn3_engine.h). Inside a message,
// every full group of 8 hops goes through the 8-hop graph and the rest through the 1-hop graph.
//
// The listening socket comes from init (rc: socket gk3denoise seqpacket ...), or --socket PATH for
// tests. The model is loaded at start in the background; while it is not loaded, Hello is answered
// with kUnavailable; the service tries again every --retry-s seconds (a connect may start an attempt
// sooner), and exits if one attempt runs longer than --load-timeout-s (init restarts it).
// The HTP power vote (--perf, default low) is held while at least one connection uses the engine.
// After a failed graph execute the engine is reloaded once all its connections are closed.
//
// The process answers QNN's read of /sys/devices/soc0/soc_id itself (socid_override.c).
//
// Logs go to logcat, tag gk3denoise (and to stderr with --stderr).
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#ifdef __ANDROID__
#include <android/log.h>
#endif
#include <dlfcn.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "denoise_protocol.h"
#include "dfn3_engine.h"
#include "socid_override.h"

#ifndef GK3DENOISE_VERSION
#define GK3DENOISE_VERSION "dev"
#endif

namespace dn = gaokun::micarray::denoise;
using namespace gk3dn;

static_assert(dn::kRate == (uint32_t)SR && dn::kHop == (uint32_t)HOP, "protocol and model disagree");
static_assert(dn::kLatencyHops == (uint32_t)kLatencyHops, "protocol latency must be the filter's");
static_assert(sizeof(dn::Hello) == 16 && sizeof(dn::HelloReply) == 16 && sizeof(dn::BlockHeader) == 8,
              "unexpected protocol struct layout");

namespace {

constexpr char kTag[] = "gk3denoise";
// Defaults for an install in the vendor partition: the QNN and FastRPC libraries in a private
// directory that only this daemon loads from (by path), the DSP-side files in another, the model in
// /vendor/etc. Every one can be changed on the command line (the KernelSU module does).
constexpr char kDefaultSocketName[] = "gk3denoise";  // init's control socket (ANDROID_SOCKET_<name>)
constexpr char kDefaultLibDir[] = "/vendor/lib64/gk3denoise";
constexpr char kDefaultDspDir[] = "/vendor/dsp/gk3denoise";
constexpr char kDefaultCtx[] = "/vendor/etc/gk3denoise/dfn3_v2_cal2_x8_a16w8_v68.bin";

struct Options {
  std::string socketPath;  // bind this path; empty: the init socket socketName
  std::string socketName = kDefaultSocketName;
  std::string ctx = kDefaultCtx, libDir = kDefaultLibDir, dspDir = kDefaultDspDir;
  std::string perf = "low";
  int rpcLatencyUs = 100;
  int qnnLog = QNN_LOG_LEVEL_ERROR;
  int retryS = 30;
  // A load still running after this long has hung inside FastRPC or QNN: exit, init restarts us.
  int loadTimeoutS = 60;
  int maxClients = 8;
  // Gain in front of the network, undone on its output. The quantized model loses speech below about
  // -40 dBFS (speech median over 100 ms; -13 dB at -44 dBFS, against -2 dB for the float model), and
  // even the float model below about -50 dBFS; from -38 to -20 dBFS it keeps speech within 2 dB, and
  // slowly loses more above (-3.6 dB at +4 dBFS). The built-in microphones give -55 to -15 dBFS of
  // speech depending on talker and decimator gain; 18 dB moves that into the good range.
  // (experiments/denoise-hal-20261103/lowlevel/)
  float preGainDb = 18.f;
  bool verbose = false;
};

// ---------------------------------------------------------------------------------------------
// Logging
bool g_stderr = false;
int g_minPrio = kLogInfo;

void logSink(int prio, const char* fmt, va_list ap) {
  if (prio < g_minPrio) return;
  char msg[1024];
  vsnprintf(msg, sizeof(msg), fmt, ap);
#ifdef __ANDROID__
  __android_log_write(prio, kTag, msg);
  if (!g_stderr) return;
#endif
  static const char* names = "??VDIWEF";
  timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  struct tm tmv;
  localtime_r(&ts.tv_sec, &tmv);
  fprintf(stderr, "%02d:%02d:%02d.%03ld %c %s: %s\n", tmv.tm_hour, tmv.tm_min, tmv.tm_sec, ts.tv_nsec / 1000000,
          names[prio & 7], kTag, msg);
}

std::string socIdLine() {
  gk3_socid_stats st;
  gk3_socid_override_stats(&st);
  char real[32];
  int r = gk3_socid_read_real(real, sizeof(real));
  char b[256];
  if (!gk3_socid_override_enabled())
    snprintf(b, sizeof(b), "soc_id override: off (real soc_id %s)", r > 0 ? real : "unreadable");
  else
    snprintf(b, sizeof(b), "soc_id override: %s, %d hit(s), first via %s (%s), %d failure(s); reported %d, real %s",
             st.hits > 0 ? "HIT" : "not hit", st.hits, st.first_via, st.first_kind, st.failures, st.value,
             r > 0 ? real : "unreadable");
  return b;
}

std::string selfContext() {
  char b[128] = "";
  int fd = open("/proc/self/attr/current", O_RDONLY | O_CLOEXEC);
  if (fd >= 0) {
    ssize_t n = read(fd, b, sizeof(b) - 1);
    close(fd);
    b[n > 0 ? n : 0] = 0;
    for (char* p = b; *p; p++)
      if (*p == '\n') *p = 0;
  }
  return b[0] ? b : "-";
}

bool exists(const std::string& p) { return access(p.c_str(), F_OK) == 0; }

// ---------------------------------------------------------------------------------------------
// The shared engine and its availability.
class Service {
 public:
  explicit Service(const Options& o) : o_(o) {}

  void configure(const LoadConfig& lc) { lc_ = lc; }
  const Dsp* dsp() const { return &dsp_; }
  Engine* engine() { return &engine_; }

  // At start: load in the background, so that the socket answers at once.
  void startInitialLoad() {
    std::lock_guard<std::mutex> lock(mu_);
    startLoadLocked();
  }

  // A connection wants the engine. False (and why) if it is not ready; may start a new load.
  bool acquire(std::string* why) {
    std::lock_guard<std::mutex> lock(mu_);
    if (state_ == kReady) {
      if (users_++ == 0) {
        std::string m;
        engine_.perfVote(o_.perf, o_.rpcLatencyUs, &m);
        logf(kLogInfo, "%s (first client)", m.c_str());
      }
      return true;
    }
    if (state_ == kLoading) {
      *why = "model still loading";
    } else if (state_ == kBroken) {
      *why = "engine failed, reloading once its connections have closed";
    } else {
      const double since = (nowUs() - lastAttemptUs_) / 1e6;
      if (since >= o_.retryS) {
        startLoadLocked();
        *why = "model not loaded, loading again now";
      } else {
        char b[96];
        snprintf(b, sizeof(b), "model not loaded, next load attempt in %.0f s", o_.retryS - since);
        *why = b;
      }
    }
    return false;
  }

  void release() {
    std::lock_guard<std::mutex> lock(mu_);
    if (--users_ > 0) return;
    std::string m;
    engine_.perfRelease(&m);
    logf(kLogInfo, "%s (no clients)", m.c_str());
    if (state_ == kBroken) {
      engine_.unload();
      state_ = kIdle;
      lastAttemptUs_ = -1e18;  // reload on the next connect
      logf(kLogWarn, "engine unloaded after a failure; the next connect loads it again");
    }
  }

  void markBroken(const std::string& why) {
    std::lock_guard<std::mutex> lock(mu_);
    if (state_ == kReady) {
      state_ = kBroken;
      logf(kLogError, "engine marked failed: %s", why.c_str());
    }
  }

  std::atomic<int> conns{0};
  std::atomic<uint64_t> connSeq{0};

 private:
  enum State { kIdle, kLoading, kReady, kBroken };

  void startLoadLocked() {
    beginAttemptLocked();
    std::thread([this] { loadThread(); }).detach();
  }

  void beginAttemptLocked() {
    state_ = kLoading;
    lastAttemptUs_ = nowUs();
    const uint64_t attempt = ++attempt_;
    std::thread([this, attempt] {
      std::this_thread::sleep_for(std::chrono::seconds(o_.loadTimeoutS));
      std::lock_guard<std::mutex> lock(mu_);
      if (state_ == kLoading && attempt_ == attempt) {
        logf(kLogError, "model load still running after %d s; exiting so that init restarts the service",
             o_.loadTimeoutS);
        _exit(1);
      }
    }).detach();
  }

  // Loads; after a failure tries again every retryS on its own (a connect in between may start the
  // next attempt sooner), so an early failure at boot does not leave the first calls unfiltered.
  void loadThread() {
    uint64_t mine;
    {
      std::lock_guard<std::mutex> lock(mu_);
      mine = attempt_;
    }
    for (;;) {
      const double t0 = nowUs();
      std::string err;
      const bool ok = engine_.load(lc_, &err);
      const double ms = (nowUs() - t0) / 1e3;
      if (ok) {
        const GraphInfo* s = engine_.step();
        const GraphInfo* b = engine_.block();
        logf(kLogInfo, "model loaded in %.0f ms: %s; graphs %s (1 hop)%s%s%s; backend %s", ms, lc_.ctx.c_str(),
             s->name.c_str(), b ? " + " : "", b ? b->name.c_str() : "", b ? " (8 hops)" : ", no 8-hop graph",
             engine_.buildId().empty() ? "?" : engine_.buildId().c_str());
      } else {
        logf(kLogError, "model load failed after %.0f ms: %s; next attempt in %d s", ms, err.c_str(), o_.retryS);
      }
      logf(ok ? kLogInfo : kLogWarn, "%s", socIdLine().c_str());
      {
        std::lock_guard<std::mutex> lock(mu_);
        state_ = ok ? kReady : kIdle;
        if (ok) return;
      }
      std::this_thread::sleep_for(std::chrono::seconds(o_.retryS));
      std::lock_guard<std::mutex> lock(mu_);
      // A connect started an attempt meanwhile (its own thread retries from now on).
      if (state_ != kIdle || attempt_ != mine) return;
      beginAttemptLocked();
      mine = attempt_;
    }
  }

  const Options& o_;
  LoadConfig lc_;
  Dsp dsp_;
  Engine engine_;
  std::mutex mu_;
  State state_ = kIdle;
  int users_ = 0;
  double lastAttemptUs_ = -1e18;
  uint64_t attempt_ = 0;  // guarded by mu_; tells the load watchdog which attempt it watches
};

// ---------------------------------------------------------------------------------------------
// One connection.
struct ConnStats {
  uint64_t msgs = 0, hops = 0;
  double sumMs = 0, maxMs = 0, execMs = 0, waitMaxMs = 0;
};

bool sendAll(int fd, const void* p, size_t n) {
  for (;;) {
    ssize_t w = send(fd, p, n, MSG_NOSIGNAL);
    if (w < 0 && errno == EINTR) continue;
    return w == (ssize_t)n;
  }
}

void serve(Service* svc, const Options* o, int fd, uint64_t id) {
  // Same operations in the same order as dfn3_qnn -g, so that its output can be compared bit for bit.
  const float preGain = std::pow(10.f, o->preGainDb / 20.f);
  ucred cred{};
  socklen_t cl = sizeof(cred);
  if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &cl) != 0) cred.pid = -1, cred.uid = (uid_t)-1;
  timeval tv{2, 0};
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

  uint8_t hb[64];
  ssize_t n;
  do n = recv(fd, hb, sizeof(hb), MSG_TRUNC);
  while (n < 0 && errno == EINTR);
  dn::Hello hello{};
  if (n >= (ssize_t)sizeof(hello)) memcpy(&hello, hb, sizeof(hello));
  dn::HelloReply rep{dn::kMagic, dn::kVersion, dn::kOk, dn::kLatencyHops, dn::kMaxHops, 0};
  char bad[96] = "";
  if (n < 0) snprintf(bad, sizeof(bad), "no hello (%s)", strerror(errno));
  else if (n != (ssize_t)sizeof(hello)) snprintf(bad, sizeof(bad), "hello of %zd bytes", n);
  else if (hello.magic != dn::kMagic) snprintf(bad, sizeof(bad), "magic 0x%08x", hello.magic);
  else if (hello.version != dn::kVersion) snprintf(bad, sizeof(bad), "version %u", hello.version);
  else if (hello.channels < 1 || hello.channels > dn::kMaxChannels) snprintf(bad, sizeof(bad), "%u channels", hello.channels);
  else if (hello.rate != dn::kRate) snprintf(bad, sizeof(bad), "rate %u", hello.rate);
  else if (hello.hop != dn::kHop) snprintf(bad, sizeof(bad), "hop %u", hello.hop);
  if (bad[0]) {
    if (n >= 0) {
      rep.status = dn::kBadRequest;
      sendAll(fd, &rep, sizeof(rep));
    }
    logf(kLogWarn, "conn %llu (pid %d uid %d): bad hello: %s", (unsigned long long)id, cred.pid, (int)cred.uid, bad);
    close(fd);
    return;
  }
  const int nch = hello.channels;
  if (svc->conns.load() > o->maxClients) {
    rep.status = dn::kUnavailable;
    sendAll(fd, &rep, sizeof(rep));
    logf(kLogWarn, "conn %llu (pid %d uid %d): refused, %d connections open", (unsigned long long)id, cred.pid,
         (int)cred.uid, svc->conns.load() - 1);
    close(fd);
    return;
  }
  std::string why;
  if (!svc->acquire(&why)) {
    rep.status = dn::kUnavailable;
    sendAll(fd, &rep, sizeof(rep));
    logf(kLogWarn, "conn %llu (pid %d uid %d, %d ch): unavailable: %s", (unsigned long long)id, cred.pid,
         (int)cred.uid, nch, why.c_str());
    close(fd);
    return;
  }

  // Everything the message loop needs, allocated once.
  std::vector<std::unique_ptr<Channel>> chans;
  ChannelConfig cc;  // defaults: thresholds -15/35/35, 8-hop graph for full groups
  bool setupOk = true;
  for (int c = 0; c < nch && setupOk; c++) {
    chans.emplace_back(new Channel());
    std::string err;
    if (!chans.back()->init(svc->engine(), svc->dsp(), cc, &err)) {
      logf(kLogError, "conn %llu: channel setup failed: %s", (unsigned long long)id, err.c_str());
      setupOk = false;
    }
  }
  const size_t maxSamples = (size_t)dn::kMaxHops * HOP;
  std::vector<uint8_t> rx(dn::kMaxMessageBytes), tx(dn::kMaxMessageBytes);
  std::vector<float> in((size_t)nch * maxSamples), out((size_t)nch * maxSamples);
  if (!setupOk) {
    chans.clear();
    svc->release();
    rep.status = dn::kUnavailable;
    sendAll(fd, &rep, sizeof(rep));
    close(fd);
    return;
  }
  tv = {0, 0};
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  if (!sendAll(fd, &rep, sizeof(rep))) {
    logf(kLogWarn, "conn %llu: hello reply not sent: %s", (unsigned long long)id, strerror(errno));
    chans.clear();
    svc->release();
    close(fd);
    return;
  }
  logf(kLogInfo, "conn %llu open: pid %d uid %d, %d ch", (unsigned long long)id, cred.pid, (int)cred.uid, nch);

  ConnStats st;
  uint32_t expectSeq = 0;
  const char* reason = "peer closed";
  char reasonBuf[128];
  for (;;) {
    do n = recv(fd, rx.data(), rx.size(), MSG_TRUNC);
    while (n < 0 && errno == EINTR);
    if (n == 0) break;
    if (n < 0) {
      snprintf(reasonBuf, sizeof(reasonBuf), "recv: %s", strerror(errno));
      reason = reasonBuf;
      break;
    }
    dn::BlockHeader h{};
    if (n >= (ssize_t)sizeof(h)) memcpy(&h, rx.data(), sizeof(h));
    const size_t want = sizeof(h) + (size_t)h.hops * HOP * nch * 2;
    bad[0] = 0;
    if (n < (ssize_t)sizeof(h)) snprintf(bad, sizeof(bad), "message of %zd bytes", n);
    else if ((size_t)n > rx.size()) snprintf(bad, sizeof(bad), "message of %zd bytes, max %zu", n, rx.size());
    else if (h.status != dn::kOk) snprintf(bad, sizeof(bad), "request status %d", h.status);
    else if (h.hops < 1 || h.hops > dn::kMaxHops) snprintf(bad, sizeof(bad), "%u hops", h.hops);
    else if ((size_t)n != want) snprintf(bad, sizeof(bad), "%zd bytes for %u hops x %d ch", n, h.hops, nch);
    else if (h.seq != expectSeq) snprintf(bad, sizeof(bad), "seq %u, expected %u", h.seq, expectSeq);
    if (bad[0]) {
      dn::BlockHeader a{n >= (ssize_t)sizeof(h) ? h.seq : 0u, 0, dn::kBadRequest};
      sendAll(fd, &a, sizeof(a));
      snprintf(reasonBuf, sizeof(reasonBuf), "bad request: %s", bad);
      reason = reasonBuf;
      break;
    }
    const double t0 = nowUs();
    const int hops = h.hops;
    const size_t frames = (size_t)hops * HOP;
    const int16_t* pin = reinterpret_cast<const int16_t*>(rx.data() + sizeof(h));
    for (size_t f = 0; f < frames; f++)
      for (int c = 0; c < nch; c++) in[(size_t)c * maxSamples + f] = (float)pin[f * nch + c] / 32768.f * preGain;
    bool ok = true;
    double execUs = 0, waitUs = 0;
    for (int c = 0; c < nch && ok; c++) {
      ProcessInfo info;
      Qnn_ErrorHandle_t err = QNN_SUCCESS;
      ok = chans[c]->process(&in[(size_t)c * maxSamples], &out[(size_t)c * maxSamples], hops, &info, &err);
      execUs += info.execUs;
      waitUs = std::max(waitUs, info.waitUs);
      if (!ok) {
        std::string e = "graph execute: " + svc->engine()->errStr(err);
        svc->markBroken(e);
        snprintf(reasonBuf, sizeof(reasonBuf), "failed: %s", e.c_str());
        reason = reasonBuf;
      }
    }
    if (!ok) {
      dn::BlockHeader a{h.seq, 0, dn::kFailed};
      sendAll(fd, &a, sizeof(a));
      break;
    }
    int16_t* pout = reinterpret_cast<int16_t*>(tx.data() + sizeof(h));
    for (size_t f = 0; f < frames; f++)
      for (int c = 0; c < nch; c++) {
        float v = std::nearbyint(out[(size_t)c * maxSamples + f] / preGain * 32768.f);
        pout[f * nch + c] = (int16_t)std::min(32767.f, std::max(-32768.f, v));
      }
    dn::BlockHeader a{h.seq, h.hops, dn::kOk};
    memcpy(tx.data(), &a, sizeof(a));
    const double ms = (nowUs() - t0) / 1e3;
    if (!sendAll(fd, tx.data(), (size_t)n)) {
      snprintf(reasonBuf, sizeof(reasonBuf), "send: %s", strerror(errno));
      reason = reasonBuf;
      break;
    }
    st.msgs++;
    st.hops += (uint64_t)hops;
    st.sumMs += ms;
    st.maxMs = std::max(st.maxMs, ms);
    st.execMs += execUs / 1e3;
    st.waitMaxMs = std::max(st.waitMaxMs, waitUs / 1e3);
    expectSeq++;
  }
  chans.clear();
  svc->release();
  close(fd);
  const double m = st.msgs ? (double)st.msgs : 1.0;
  logf(kLogInfo,
       "conn %llu closed (%s): pid %d, %d ch, %llu msgs, %llu hops (%.1f s), per msg mean %.2f ms max %.2f ms "
       "(graph execute mean %.2f ms, longest lock wait %.2f ms)",
       (unsigned long long)id, reason, cred.pid, nch, (unsigned long long)st.msgs, (unsigned long long)st.hops,
       (double)st.hops * HOP / SR, st.sumMs / m, st.maxMs, st.execMs / m, st.waitMaxMs);
}

// ---------------------------------------------------------------------------------------------
int controlSocket(const std::string& name) {  // android_get_control_socket() without libcutils
  char env[128];
  snprintf(env, sizeof(env), "ANDROID_SOCKET_%s", name.c_str());
  const char* v = getenv(env);
  if (!v) return -1;
  char* end = nullptr;
  errno = 0;
  long fd = strtol(v, &end, 10);
  if (errno || !end || *end || fd < 0 || fd > INT_MAX) return -1;
  struct stat sb;
  if (fstat((int)fd, &sb) != 0 || !S_ISSOCK(sb.st_mode)) return -1;
  fcntl((int)fd, F_SETFD, FD_CLOEXEC);
  return (int)fd;
}

int bindSocket(const std::string& path) {
  int fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
  if (fd < 0) return -1;
  sockaddr_un a{};
  a.sun_family = AF_UNIX;
  if (path.size() >= sizeof(a.sun_path)) {
    errno = ENAMETOOLONG;
    close(fd);
    return -1;
  }
  memcpy(a.sun_path, path.c_str(), path.size() + 1);
  unlink(path.c_str());
  if (bind(fd, (sockaddr*)&a, sizeof(a)) != 0) {
    int e = errno;
    close(fd);
    errno = e;
    return -1;
  }
  chmod(path.c_str(), 0660);
  return fd;
}

int socidProbe(const char* lib) {
  void* h = dlopen(lib, RTLD_NOW | RTLD_LOCAL);
  if (!h) {
    printf("socid-probe: dlopen %s: %s\n", lib, dlerror());
    return 2;
  }
  typedef int (*ProbeFn)(char*, size_t);
  auto fn = (ProbeFn)dlsym(h, "gk3_socid_probe");
  if (!fn) {
    printf("socid-probe: no gk3_socid_probe in %s\n", lib);
    return 2;
  }
  char out[256];
  fn(out, sizeof(out));
  printf("socid-probe: a dlopen'd library reads: %s\n", out);
  printf("socid-probe: %s\n", socIdLine().c_str());
  const bool all = strstr(out, "open=415 ") && strstr(out, "openat=415 ") && strstr(out, "__open_2=415 ") &&
                   strstr(out, "__openat_2=415 ") && strstr(out, "fopen=415 ");
  printf("socid-probe: %s\n", all ? "PASS (every libc entry point sees 415)" : "FAIL");
  return all ? 0 : 1;
}

void usage() {
  printf(
      "usage: gk3denoised [options]\n"
      "  --init-socket NAME socket created by init for this service (default %s)\n"
      "  --socket PATH      instead: bind and listen on PATH (tests)\n"
      "  --ctx FILE         context binary (default %s)\n"
      "  --lib-dir DIR      QNN + FastRPC libraries, loaded by path from DIR only (default %s);\n"
      "                     '-' loads them by name through the library search path (host tests)\n"
      "  --dsp-dir DIRS     DSP-side files, ';'-separated; becomes DSP_LIBRARY_PATH and\n"
      "                     ADSP_LIBRARY_PATH (default %s); '-' leaves the environment as it is\n"
      "  --perf P           HTP vote while clients are connected: burst|high|balanced|low|none (low)\n"
      "  --pre-gain-db DB   gain in front of the network, undone on its output (18)\n"
      "  --rpc-latency US   RPC control latency vote (100)\n"
      "  --qnn-log N        QNN log level 1..5 (1 = errors)\n"
      "  --retry-s S        time between model load attempts after a failure (30)\n"
      "  --load-timeout-s S exit if a model load runs longer than this (60)\n"
      "  --max-clients N    connections served at once (8)\n"
      "  --no-socid-override  do not answer the soc_id read (test)\n"
      "  --socid-probe LIB  dlopen LIB (socid_probe.c), print what it reads from soc_id, exit\n"
      "  --stderr           also log to stderr;  -v  debug messages\n",
      kDefaultSocketName, kDefaultCtx, kDefaultLibDir, kDefaultDspDir);
}

}  // namespace

int main(int argc, char** argv) {
  Options o;
  std::string probe;
  for (int i = 1; i < argc; i++) {
    std::string a = argv[i];
    auto next = [&]() -> std::string {
      if (i + 1 >= argc) {
        usage();
        exit(2);
      }
      return argv[++i];
    };
    if (a == "--socket") o.socketPath = next();
    else if (a == "--init-socket") o.socketName = next();
    else if (a == "--ctx") o.ctx = next();
    else if (a == "--lib-dir") o.libDir = next();
    else if (a == "--dsp-dir") o.dspDir = next();
    else if (a == "--perf") o.perf = next();
    else if (a == "--pre-gain-db") o.preGainDb = (float)atof(next().c_str());
    else if (a == "--rpc-latency") o.rpcLatencyUs = atoi(next().c_str());
    else if (a == "--qnn-log") o.qnnLog = atoi(next().c_str());
    else if (a == "--retry-s") o.retryS = atoi(next().c_str());
    else if (a == "--load-timeout-s") o.loadTimeoutS = atoi(next().c_str());
    else if (a == "--max-clients") o.maxClients = atoi(next().c_str());
    else if (a == "--no-socid-override") gk3_socid_override_enable(0);
    else if (a == "--socid-probe") probe = next();
    else if (a == "--stderr") g_stderr = true;
    else if (a == "-v") o.verbose = true;
    else if (a == "--version") {
      printf("gk3denoised %s\n", GK3DENOISE_VERSION);
      return 0;
    } else {
      usage();
      return 2;
    }
  }
  setvbuf(stdout, nullptr, _IOLBF, 0);
  if (!probe.empty()) return socidProbe(probe.c_str());
  g_minPrio = o.verbose ? kLogDebug : kLogInfo;
  setLogSink(logSink);
  signal(SIGPIPE, SIG_IGN);

  // The DSP side (fastrpc_shell, skel, its libc++) is looked up by libcdsprpc's file listener in
  // these directories, read from the environment when the session opens.
  if (o.dspDir != "-") {
    setenv("DSP_LIBRARY_PATH", o.dspDir.c_str(), 1);
    setenv("ADSP_LIBRARY_PATH", o.dspDir.c_str(), 1);
  }
  LoadConfig lc;
  lc.ctx = o.ctx;
  lc.qnnLogLevel = o.qnnLog;
  if (o.libDir != "-") {
    // By path only: libQnnHtp.so finds libQnnHtpV68Stub.so and that one libcdsprpc.so through
    // their RUNPATH $ORIGIN, i.e. in the same directory; libcdsprpc.so is loaded first so that
    // the one in this directory is the one used.
    if (exists(o.libDir + "/libcdsprpc.so")) lc.preload.push_back(o.libDir + "/libcdsprpc.so");
    lc.backend = o.libDir + "/libQnnHtp.so";
    lc.system = o.libDir + "/libQnnSystem.so";
  }

  int lfd = o.socketPath.empty() ? controlSocket(o.socketName) : bindSocket(o.socketPath);
  const std::string sockDesc = o.socketPath.empty() ? "init socket " + o.socketName : o.socketPath;
  const char* dspEnv = getenv("DSP_LIBRARY_PATH");
  logf(kLogInfo, "gk3denoised %s start: pid %d uid %d context %s; socket %s; model %s; libraries %s; DSP path %s; perf %s; pre-gain %+.1f dB",
       GK3DENOISE_VERSION, getpid(), (int)getuid(), selfContext().c_str(), sockDesc.c_str(), o.ctx.c_str(),
       o.libDir == "-" ? "by name" : o.libDir.c_str(), dspEnv ? dspEnv : "-", o.perf.c_str(), o.preGainDb);
  logf(kLogInfo, "%s", socIdLine().c_str());
  if (lfd < 0) {
    logf(kLogError, "no listening socket (%s): %s", sockDesc.c_str(),
         o.socketPath.empty() ? ("ANDROID_SOCKET_" + o.socketName + " not set or not a socket").c_str()
                              : strerror(errno));
    return 1;
  }
  if (listen(lfd, 16) != 0) {
    logf(kLogError, "listen: %s", strerror(errno));
    return 1;
  }
  std::string st;
  if (!selftest(&st)) {
    logf(kLogError, "%s", st.c_str());
    return 1;
  }

  static Service svc(o);  // lives until exit; detached threads use it
  svc.configure(lc);
  svc.startInitialLoad();
  logf(kLogInfo, "listening on %s (protocol v%u, max %u hops per message, latency %u hops)", sockDesc.c_str(),
       dn::kVersion, dn::kMaxHops, dn::kLatencyHops);
  for (;;) {
    int fd = accept4(lfd, nullptr, nullptr, SOCK_CLOEXEC);
    if (fd < 0) {
      if (errno == EINTR || errno == ECONNABORTED) continue;
      logf(kLogError, "accept: %s", strerror(errno));
      if (errno == EMFILE || errno == ENFILE || errno == ENOMEM || errno == ENOBUFS) {
        usleep(100000);
        continue;
      }
      _exit(1);  // not return: svc and o are in use by detached threads
    }
    const uint64_t id = ++svc.connSeq;
    svc.conns++;
    std::thread([fd, id, &o] {
      serve(&svc, &o, fd, id);
      svc.conns--;
    }).detach();
  }
}
