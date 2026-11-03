// SPDX-License-Identifier: Apache-2.0
// Copyright (C) 2026 Akisaira
//
// dfn3_engine: streaming DeepFilterNet3 (48 kHz, hops of 480 frames) with the network on a QNN
// backend (HTP context binary, or model libraries on the CPU backend) and every other stage on the
// CPU in float.
//
// One Engine holds the loaded backend, context and graphs; it is shared by any number of Channels.
// A Channel is one audio channel of one stream: the CPU stages' state (DfnCpu) and the recurrent
// network state (c0 history, GRU states) in its own buffers, plus its own graph input/output
// buffers. Graph executes of all channels are serialized by the engine's mutex; everything else
// runs in the caller's thread. Output hop n of a channel is input hop n - kLatencyHops, filtered.
//
// CPU stages follow libDF at DeepFilterNet d375b2d8 (MIT OR Apache-2.0):
//   libDF/src/lib.rs   frame_analysis, frame_synthesis, feat_erb (compute_band_corr, log10,
//                      band_mean_norm_erb), feat_cplx (band_unit_norm), apply_interp_band_gain,
//                      erb_fb, init_mean_norm_state / init_unit_norm_state
//   libDF/src/tract.rs DfTract::process (silence skip, rolling spectrum buffers, lookahead 2,
//                      apply_stages thresholds), df() deep filter, calc_norm_alpha
// Model, graph interface and the CPU-side math: experiments/dfn3-htp-20261103/INTERFACE.md (the
// graphs dfn3_step, 1 hop per call, and dfn3_block8, 8 hops per call, share the I/O encodings).
// This code was factored out of experiments/dfn3-htp-20261103/src/dfn3_qnn.cpp.
#pragma once

#include <complex>
#include <cstdarg>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#define POCKETFFT_NO_MULTITHREADING
#include "pocketfft_hdronly.h"

#include "HTP/QnnHtpCommon.h"
#include "HTP/QnnHtpDevice.h"
#include "HTP/QnnHtpPerfInfrastructure.h"
#include "QnnInterface.h"
#include "QnnProfile.h"
#include "System/QnnSystemInterface.h"

namespace gk3dn {

using cf = std::complex<float>;

// DeepFilterNet3 constants (config.ini of the DFN3 release).
constexpr int SR = 48000, FFT = 960, HOP = 480, NF = FFT / 2 + 1;
constexpr int NB_ERB = 32, NB_DF = 96, ORD = 5, LOOKAHEAD = 2, C0CH = 64, HID = 256, NHID = 5;
constexpr int ALIGN = FFT - HOP + LOOKAHEAD * HOP;  // 1440 samples: window overlap + lookahead
constexpr int kLatencyHops = ALIGN / HOP;           // 3
constexpr float ALPHA = 0.99f;                      // calc_norm_alpha(48000, 480, norm_tau=1)
constexpr int kMaxGraphHops = 8;                    // largest multi-hop graph the engine accepts

double nowUs();  // CLOCK_MONOTONIC

// ---------------------------------------------------------------------------------------------
// Logging: the program decides where engine messages go (stdout, logcat, ...).
enum LogPrio { kLogDebug = 3, kLogInfo = 4, kLogWarn = 5, kLogError = 6 };
using LogSink = void (*)(int prio, const char* fmt, va_list ap);
void setLogSink(LogSink sink);
void logf(int prio, const char* fmt, ...) __attribute__((format(printf, 2, 3)));

// ---------------------------------------------------------------------------------------------
// CPU stages
std::vector<int> erbWidths(int sr = SR, int fft = FFT, int nb = NB_ERB, int minNb = 2);

// Shared, read-only DSP tables; usable from several threads at once (the FFT work buffer is the
// caller's).
struct Dsp {
  pocketfft::detail::pocketfft_r<float> plan{FFT};
  float win[FFT];
  float wnorm;
  std::vector<int> widths;
  int bandOf[NF];
  Dsp();
  // buf: FFT real samples in, overwritten; out: NF bins (scaled by wnorm); scratch: FFT floats.
  void rfft(float* buf, cf* out, float* scratch) const;
  // Unnormalized inverse (realfft / FFTPACK backward); imaginary parts of DC and Nyquist ignored.
  void irfft(const cf* in, float* buf, float* scratch) const;
};

// FFT conventions and ERB layout check; returns false (and says why) if something is off.
bool selftest(std::string* report);

// Per-channel streaming state of the CPU stages. Spectra and feature rows are kept in rings indexed
// by the processed-hop counter p, so that several hops can be analysed before their network call
// (multi-hop graph) and post-processed afterwards in order.
struct DfnCpu {
  static constexpr int RING = 16;
  const Dsp* d;
  float anaMem[HOP], synMem[HOP], meanNorm[NB_ERB], unitNorm[NB_DF];
  cf spec[RING][NF];
  float erbRow[RING][NB_ERB];
  float cplxRow[RING][NB_DF * 2];  // (re, im) interleaved
  cf zero[NF];
  int64_t p = 0;  // processed (not skipped) hops so far
  int skip = 0;
  // Apply post()'s skip-counter reset at pre() time, as needed when several hops are analysed
  // before their network call. Valid when every hop gets gains (lsnr is bounded to [-15, 35], so
  // thresholds min <= -15 and max_erb >= 35 guarantee that).
  bool predictReset = false;
  float minDb = -15.f, maxErb = 35.f, maxDf = 35.f;
  alignas(64) float fbuf[FFT];
  alignas(64) float fftScratch[FFT];

  explicit DfnCpu(const Dsp* dsp) : d(dsp) { reset(); }
  void reset();
  const cf* frame(int64_t q) const { return q < 0 ? zero : spec[q % RING]; }
  // Analysis + features of one hop. Returns false if the hop is skipped as silence (libDF
  // skip_counter); otherwise the hop gets index p - 1.
  bool pre(const float* x);
  // Graph feature input for hops q0 .. q0+T-1: rows q0-2 .. q0+T-1 (zero before the first hop).
  void window(int64_t q0, int T, float* erb, float* cplx) const;
  // Mask + deep filter for hop q (on frame q-2), synthesis. gains [32], coefs [96][5][2]. Hops
  // must be post-processed in order.
  void post(int64_t q, const float* gains, const float* coefs, float lsnr, float* out);
};

// ---------------------------------------------------------------------------------------------
// QNN side

// Saturation counters. Inputs: values outside the encoding range before quantizing. Outputs:
// codes at the ends of the range (for ReLU/sigmoid outputs code 0 is normal).
struct SatCount {
  uint64_t elems = 0, lo = 0, hi = 0, callsLo = 0, callsHi = 0;
  void count(size_t cnt, size_t l, size_t h) {
    elems += cnt, lo += l, hi += h;
    callsLo += l > 0, callsHi += h > 0;
  }
  void add(const SatCount& o) {
    elems += o.elems, lo += o.lo, hi += o.hi, callsLo += o.callsLo, callsHi += o.callsHi;
  }
};

// A graph input/output as the context describes it; the data lives in buffers of the Channels.
// Fields common to Qnn_TensorV1_t/V2_t are accessed through v1 (V2 starts with the V1 layout).
struct TensorInfo {
  std::string name;
  std::vector<uint32_t> dims;
  Qnn_Tensor_t tmpl;  // name/dims point into this object; clientBuf is set per Channel
  size_t n = 1, esz = 4;
  Qnn_DataType_t dt = QNN_DATATYPE_FLOAT_32;
  float scale = 1.f, inv = 1.f;
  int32_t offset = 0;
  int64_t qmin = 0, qmax = 0;

  bool init(const Qnn_Tensor_t& src);
  size_t bytes() const { return n * esz; }
  bool sameQuant(const TensorInfo& o) const { return dt == o.dt && scale == o.scale && offset == o.offset; }
  std::string desc() const;
  // float <-> tensor element conversion on buffer 'buf', element range [i0, i0 + cnt)
  void put(uint8_t* buf, const float* x, size_t i0, size_t cnt, SatCount* sat) const;
  void get(const uint8_t* buf, float* y, size_t i0, size_t cnt) const;
  void scan(const uint8_t* buf, SatCount* sat) const;  // output codes at qmin / qmax
};

// One graph of the context: T hops per call (T = feat_erb rows - 2).
struct GraphInfo {
  std::string name;
  Qnn_GraphHandle_t h = nullptr;
  std::vector<TensorInfo> ins, outs;
  int T = 0;
  int iErb = -1, iSpec = -1, iC0h = -1, iHin = -1;
  int oGains = -1, oCoefs = -1, oLsnr = -1, oC0 = -1, oH = -1;
  bool c0Full = false;  // c0 output is the whole history (multi-hop graph), not one frame
  bool bind();          // finds the tensors of the dfn3_net.py interface
  size_t inBytes() const;
  size_t outBytes() const;
};

struct LoadConfig {
  std::string backend = "libQnnHtp.so";  // name or path
  std::string system = "libQnnSystem.so";
  std::string ctx;                        // context binary, or
  std::vector<std::string> modelLibs;     // model libraries (e.g. float graphs on libQnnCpu.so)
  std::vector<std::string> preload;       // libraries dlopen'd first, by path (e.g. libcdsprpc.so)
  int qnnLogLevel = QNN_LOG_LEVEL_WARN;
  uint32_t socModel = 0;  // HTP device config SoC model, 0 = let QNN detect
  bool platformInfo = false;
  bool profile = false;   // basic profiling per execute
  int blockHops = 8;      // hops per call of the multi-hop graph to use
};

struct ExecTiming {
  double startUs = 0, endUs = 0, waitUs = 0;  // waitUs: time spent waiting for the mutex
};

class Engine {
 public:
  Engine() = default;
  ~Engine();
  Engine(const Engine&) = delete;
  Engine& operator=(const Engine&) = delete;

  // Loads backend, device, context and graphs. Must not run while Channels of this engine exist.
  bool load(const LoadConfig& cfg, std::string* err);
  // Frees context, device and backend (the libraries stay loaded). Same restriction as load().
  void unload();
  bool loaded() const { return step_ != nullptr; }

  const GraphInfo* step() const { return step_; }    // 1 hop per call
  const GraphInfo* block() const { return block_; }  // blockHops per call, or null
  std::vector<GraphInfo>& graphs() { return graphs_; }
  const std::string& buildId() const { return buildId_; }
  bool isHtp() const { return isHtp_; }
  Qnn_ProfileHandle_t profile() const { return prof_; }
  double loadMs() const { return loadMs_; }

  // HTP power vote (DCVS v3 presets burst|high|balanced|low, modeled on qnn-net-run's profiles,
  // plus the RPC control latency). perfRelease() destroys the power config id, which removes all
  // votes made through it (QNN HTP backend docs, "destroyPowerConfigId").
  bool perfVote(const std::string& preset, int rpcLatencyUs, std::string* msg);
  bool perfRelease(std::string* msg);
  bool perfActive() const { return powerId_ != 0; }

  // Serialized graphExecute. 'after' (optional) runs inside the lock right after the execute.
  struct AfterExec {
    virtual ~AfterExec() = default;
    virtual void afterExecute(const GraphInfo& g, Qnn_ErrorHandle_t e, const ExecTiming& t) = 0;
  };
  Qnn_ErrorHandle_t execute(const GraphInfo& g, Qnn_Tensor_t* in, uint32_t nIn, Qnn_Tensor_t* out,
                            uint32_t nOut, ExecTiming* t, AfterExec* after = nullptr);
  std::string errStr(Qnn_ErrorHandle_t e) const;
  const QNN_INTERFACE_VER_TYPE* qnn() const { return qnn_; }

 private:
  bool loadBackend(const LoadConfig& cfg, std::string* err);
  bool createDevice(const LoadConfig& cfg, std::string* err);
  bool loadContext(const LoadConfig& cfg, std::string* err);
  bool loadModelLibs(const LoadConfig& cfg, std::string* err);
  void printPlatformInfo();

  std::mutex execMu_;
  void* lib_ = nullptr;
  void* sysLib_ = nullptr;
  std::vector<void*> preloaded_, modelLibs_;
  const QNN_INTERFACE_VER_TYPE* qnn_ = nullptr;
  Qnn_LogHandle_t log_ = nullptr;
  Qnn_BackendHandle_t backend_ = nullptr;
  Qnn_DeviceHandle_t device_ = nullptr;
  Qnn_ContextHandle_t ctx_ = nullptr;
  Qnn_ProfileHandle_t prof_ = nullptr;
  uint32_t powerId_ = 0;
  QnnHtpDevice_PerfInfrastructure_t* perf_ = nullptr;
  std::vector<GraphInfo> graphs_;  // never resized after load: tensors point into it
  GraphInfo* step_ = nullptr;
  GraphInfo* block_ = nullptr;
  bool isHtp_ = false;
  std::string buildId_;
  double loadMs_ = 0;
};

struct ChannelConfig {
  float minDb = -15.f, maxErb = 35.f, maxDf = 35.f;  // lsnr thresholds (apply_stages)
  bool predictReset = true;     // see DfnCpu::predictReset; needed with the multi-hop graph
  bool useBlock = true;         // use the multi-hop graph for full groups of its hop count
  bool countSaturation = false;
};

// Per-call information for tools (timing, the network outputs).
struct ChannelHooks {
  virtual ~ChannelHooks() = default;
  // Inside the engine lock, right after the execute (e.g. to read the shared profile handle).
  virtual void afterExecute(const GraphInfo& g, const ExecTiming& t) {}
  // After the outputs are dequantized: T rows of gains [32], coefs [960], lsnr [1].
  virtual void outputs(const GraphInfo& g, int T, const float* gains, const float* coefs, const float* lsnr) {}
};

struct ProcessInfo {
  int skipped = 0;     // hops skipped as silence (output zeros)
  int calls1 = 0, callsB = 0;
  double execUs = 0;   // sum of graphExecute times (without mutex waits)
  double waitUs = 0;   // sum of mutex waits
  double firstExecStartUs = 0, lastExecEndUs = 0;
};

class Channel {
 public:
  Channel() = default;
  Channel(const Channel&) = delete;
  Channel& operator=(const Channel&) = delete;
  // Allocates all buffers; the per-hop path allocates nothing afterwards.
  bool init(Engine* e, const Dsp* dsp, const ChannelConfig& cfg, std::string* err);
  void reset();  // filter state back to the start (zero windows, history, GRU states)
  // nHops hops of HOP samples in -> nHops hops out (output hop n = input hop n - 3, filtered).
  // Full groups of the multi-hop graph's hop count go through it, the rest through the 1-hop graph.
  // On a failed execute returns false with *err set; the channel state is then undefined.
  bool process(const float* in, float* out, int nHops, ProcessInfo* info, Qnn_ErrorHandle_t* err,
               ChannelHooks* hooks = nullptr);
  // Saturation counters per graph and tensor (countSaturation).
  const std::vector<SatCount>& inSat(int graph) const { return io_[graph].inSat; }
  const std::vector<SatCount>& outSat(int graph) const { return io_[graph].outSat; }
  const GraphInfo* graph(int i) const { return io_[i].g; }
  int graphCount() const { return nGraphs_; }

 private:
  struct GraphIo {
    const GraphInfo* g = nullptr;
    std::vector<std::vector<uint8_t>> inBuf, outBuf;
    std::vector<Qnn_Tensor_t> inT, outT;
    bool c0Alias = false, hAlias = false;
    std::vector<SatCount> inSat, outSat;
  };
  bool initIo(GraphIo& io, const GraphInfo* g, std::string* err);
  bool runGraph(GraphIo& io, int64_t q0, ProcessInfo* info, Qnn_ErrorHandle_t* err, ChannelHooks* hooks);

  Engine* e_ = nullptr;
  ChannelConfig cfg_;
  std::unique_ptr<DfnCpu> cpu_;
  const GraphInfo* ref_ = nullptr;  // the 1-hop graph: its c0_hist / h_in encodings hold the state
  std::vector<uint8_t> c0State_, hState_;
  GraphIo io_[2];  // [0] 1-hop graph, [1] multi-hop graph (if used)
  int nGraphs_ = 0;
  std::vector<float> erbTmp_, specTmp_, c0Tmp_, hTmp_, gains_, coefs_, lsnr_;
  struct Pend {
    int64_t q;
    int hop;
  };
  Pend pend_[kMaxGraphHops];
};

}  // namespace gk3dn
