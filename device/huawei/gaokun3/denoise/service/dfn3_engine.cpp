// SPDX-License-Identifier: Apache-2.0
// Copyright (C) 2026 Akisaira
//
// dfn3_engine: see dfn3_engine.h.
#include "dfn3_engine.h"

#include <dlfcn.h>
#include <time.h>

#include <algorithm>
#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace gk3dn {

double nowUs() {
  timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec * 1e6 + ts.tv_nsec * 1e-3;
}

// ---------------------------------------------------------------------------------------------
// Logging
namespace {

void stderrSink(int prio, const char* fmt, va_list ap) {
  vfprintf(stderr, fmt, ap);
  fputc('\n', stderr);
}

LogSink g_sink = stderrSink;

}  // namespace

void setLogSink(LogSink sink) { g_sink = sink ? sink : stderrSink; }

void logf(int prio, const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  g_sink(prio, fmt, ap);
  va_end(ap);
}

// ---------------------------------------------------------------------------------------------
// CPU stages
namespace {

float freq2erb(float f) { return 9.265f * std::log1p(f / (24.7f * 9.265f)); }
float erb2freq(float n) { return 24.7f * 9.265f * (std::exp(n / 9.265f) - 1.f); }

}  // namespace

// libDF erb_fb()
std::vector<int> erbWidths(int sr, int fft, int nb, int minNb) {
  float fw = (float)sr / (float)fft, lo = freq2erb(0.f), hi = freq2erb((float)(sr / 2));
  float step = (hi - lo) / (float)nb;
  std::vector<int> erb(nb);
  int prev = 0, over = 0;
  for (int i = 1; i <= nb; i++) {
    float f = erb2freq(lo + (float)i * step);
    int fb = (int)std::round(f / fw);
    int n = fb - prev - over;
    if (n < minNb) {
      over = minNb - n;
      n = minNb;
    } else {
      over = 0;
    }
    erb[i - 1] = n;
    prev = fb;
  }
  erb[nb - 1] += 1;
  int sum = 0;
  for (int v : erb) sum += v;
  if (sum > fft / 2 + 1) erb[nb - 1] -= sum - (fft / 2 + 1);
  return erb;
}

Dsp::Dsp() {
  for (int i = 0; i < FFT; i++) {
    double s = std::sin(0.5 * M_PI * (i + 0.5) / (FFT / 2));
    win[i] = (float)std::sin(0.5 * M_PI * s * s);
  }
  wnorm = 1.f / ((float)(FFT * FFT) / (float)(2 * HOP));
  widths = erbWidths();
  int k = 0;
  for (int b = 0; b < NB_ERB; b++)
    for (int j = 0; j < widths[b]; j++) bandOf[k++] = b;
  if (!plan.has_packplan()) abort();  // FFT 960 = 2^6 * 3 * 5 always gets the packed plan
}

void Dsp::rfft(float* buf, cf* out, float* scratch) const {
  plan.exec_scratch(buf, 1.f, true, scratch);  // FFTPACK half-complex: r0, r1, i1, ..., r479, i479, r480
  out[0] = cf(buf[0] * wnorm, 0.f);
  for (int k = 1; k < NF - 1; k++) out[k] = cf(buf[2 * k - 1] * wnorm, buf[2 * k] * wnorm);
  out[NF - 1] = cf(buf[FFT - 1] * wnorm, 0.f);
}

void Dsp::irfft(const cf* in, float* buf, float* scratch) const {
  buf[0] = in[0].real();
  for (int k = 1; k < NF - 1; k++) {
    buf[2 * k - 1] = in[k].real();
    buf[2 * k] = in[k].imag();
  }
  buf[FFT - 1] = in[NF - 1].real();
  plan.exec_scratch(buf, 1.f, false, scratch);
}

bool selftest(std::string* report) {
  Dsp d;
  float buf[FFT], scratch[FFT];
  cf spec[NF];
  for (int i = 0; i < FFT; i++) buf[i] = std::sin(2 * M_PI * 7 * i / FFT) + 0.5f * std::cos(2 * M_PI * 5 * i / FFT);
  d.rfft(buf, spec, scratch);
  // numpy: rfft(sin(2 pi 7 n / N))[7] = -i N/2; rfft(cos)[5] = N/2 (times wnorm here)
  bool ok = std::fabs(spec[7].imag() / d.wnorm + FFT / 2) < 1e-2 && std::fabs(spec[5].real() / d.wnorm - FFT / 4) < 1e-2;
  float in[FFT];
  for (int i = 0; i < FFT; i++) in[i] = std::sin(0.01f * i * i) + 0.1f;
  memcpy(buf, in, sizeof(in));
  d.rfft(buf, spec, scratch);
  for (auto& c : spec) c /= d.wnorm;
  d.irfft(spec, buf, scratch);
  double err = 0;
  for (int i = 0; i < FFT; i++) err = std::max(err, (double)std::fabs(buf[i] / FFT - in[i]));
  ok = ok && err < 1e-4;
  std::vector<int> w = erbWidths();
  const int ref[NB_ERB] = {2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 5, 5, 7, 7, 8, 10, 12, 13, 15, 18, 20, 24, 28, 31, 37, 42, 50, 56, 67};
  bool wok = std::equal(w.begin(), w.end(), ref);
  if (report) {
    char b[160];
    snprintf(b, sizeof(b), "selftest: fft sign %s, roundtrip err %.2e, erb widths %s", ok ? "ok" : "BAD", err,
             wok ? "ok" : "BAD");
    *report = b;
  }
  return ok && wok;
}

void DfnCpu::reset() {
  memset(anaMem, 0, sizeof(anaMem));
  memset(synMem, 0, sizeof(synMem));
  for (int i = 0; i < NB_ERB; i++) meanNorm[i] = -60.f + (float)i * ((-90.f + 60.f) / (NB_ERB - 1));
  for (int i = 0; i < NB_DF; i++) unitNorm[i] = 0.001f + (float)i * ((0.0001f - 0.001f) / (NB_DF - 1));
  for (auto& r : spec) std::fill(r, r + NF, cf(0.f, 0.f));
  std::fill(zero, zero + NF, cf(0.f, 0.f));
  memset(erbRow, 0, sizeof(erbRow));
  memset(cplxRow, 0, sizeof(cplxRow));
  p = 0;
  skip = 0;
}

bool DfnCpu::pre(const float* x) {
  float ms = 0.f;
  for (int i = 0; i < HOP; i++) ms += x[i] * x[i];
  ms /= (float)HOP;
  skip = ms < 1e-7f ? skip + 1 : 0;
  if (skip > 5) return false;
  const int slot = (int)(p % RING);
  cf* sp = spec[slot];
  for (int i = 0; i < HOP; i++) {
    fbuf[i] = anaMem[i] * d->win[i];
    fbuf[HOP + i] = x[i] * d->win[HOP + i];
  }
  memcpy(anaMem, x, sizeof(anaMem));
  d->rfft(fbuf, sp, fftScratch);
  float* e = erbRow[slot];
  int bc = 0;
  for (int b = 0; b < NB_ERB; b++) {
    int w = d->widths[b];
    float k = 1.f / (float)w, acc = 0.f;
    for (int j = 0; j < w; j++) {
      const cf& v = sp[bc + j];
      acc += (v.real() * v.real() + v.imag() * v.imag()) * k;
    }
    bc += w;
    float db = std::log10(acc + 1e-10f) * 10.f;
    meanNorm[b] = db * (1.f - ALPHA) + meanNorm[b] * ALPHA;
    e[b] = (db - meanNorm[b]) / 40.f;
  }
  float* c = cplxRow[slot];
  for (int f = 0; f < NB_DF; f++) {
    unitNorm[f] = std::hypot(sp[f].real(), sp[f].imag()) * (1.f - ALPHA) + unitNorm[f] * ALPHA;
    float s = std::sqrt(unitNorm[f]);
    c[2 * f] = sp[f].real() / s;
    c[2 * f + 1] = sp[f].imag() / s;
  }
  p++;
  if (predictReset) skip = 0;
  return true;
}

void DfnCpu::window(int64_t q0, int T, float* erb, float* cplx) const {
  for (int r = 0; r < T + 2; r++) {
    int64_t q = q0 - 2 + r;
    if (q < 0) {
      memset(erb + r * NB_ERB, 0, NB_ERB * sizeof(float));
      memset(cplx + r * NB_DF * 2, 0, NB_DF * 2 * sizeof(float));
    } else {
      memcpy(erb + r * NB_ERB, erbRow[q % RING], NB_ERB * sizeof(float));
      memcpy(cplx + r * NB_DF * 2, cplxRow[q % RING], NB_DF * 2 * sizeof(float));
    }
  }
}

void DfnCpu::post(int64_t q, const float* gains, const float* coefs, float lsnr, float* out) {
  bool applyGains, gainZeros, applyDf;
  if (lsnr < minDb) applyGains = false, gainZeros = true, applyDf = false;
  else if (lsnr > maxErb) applyGains = false, gainZeros = false, applyDf = false;
  else if (lsnr > maxDf) applyGains = true, gainZeros = false, applyDf = false;
  else applyGains = true, gainZeros = false, applyDf = true;
  cf y[NF];
  const cf* s2 = frame(q - LOOKAHEAD);
  if (applyGains) {
    for (int i = 0; i < NF; i++) y[i] = s2[i] * gains[d->bandOf[i]];
    if (!predictReset) skip = 0;
  } else if (gainZeros) {
    for (int i = 0; i < NF; i++) y[i] = 0.f;
    if (!predictReset) skip = 0;
  } else {
    for (int i = 0; i < NF; i++) y[i] = s2[i];
    if (!predictReset) skip++;
  }
  if (applyDf) {
    const cf* fr[ORD];
    for (int k = 0; k < ORD; k++) fr[k] = frame(q - (ORD - 1) + k);
    for (int f = 0; f < NB_DF; f++) {
      cf acc = 0.f;
      for (int k = 0; k < ORD; k++) acc += fr[k][f] * cf(coefs[(f * ORD + k) * 2], coefs[(f * ORD + k) * 2 + 1]);
      y[f] = acc;
    }
  }
  d->irfft(y, fbuf, fftScratch);
  for (int i = 0; i < HOP; i++) out[i] = fbuf[i] * d->win[i] + synMem[i];
  for (int i = 0; i < HOP; i++) synMem[i] = fbuf[HOP + i] * d->win[HOP + i];
}

// ---------------------------------------------------------------------------------------------
// Tensors and graphs
namespace {

const char* dtName(Qnn_DataType_t t) {
  switch (t) {
    case QNN_DATATYPE_FLOAT_32: return "f32";
    case QNN_DATATYPE_FLOAT_16: return "f16";
    case QNN_DATATYPE_UFIXED_POINT_16: return "ufxp16";
    case QNN_DATATYPE_UFIXED_POINT_8: return "ufxp8";
    case QNN_DATATYPE_SFIXED_POINT_16: return "sfxp16";
    case QNN_DATATYPE_SFIXED_POINT_8: return "sfxp8";
    default: return "?";
  }
}

void qnnLogCallback(const char* fmt, QnnLog_Level_t level, uint64_t ts, va_list args) {
  static const char* names[] = {"?", "E", "W", "I", "V", "D"};
  const int prio = level == QNN_LOG_LEVEL_ERROR ? kLogError : level == QNN_LOG_LEVEL_WARN ? kLogWarn
                   : level == QNN_LOG_LEVEL_INFO ? kLogInfo : kLogDebug;
  char msg[1024];
  vsnprintf(msg, sizeof(msg), fmt, args);
  size_t len = strlen(msg);
  while (len && msg[len - 1] == '\n') msg[--len] = 0;
  logf(prio, "[qnn %s %" PRIu64 "] %s", (level >= 1 && level <= 5) ? names[level] : "?", ts, msg);
}

// Model library ABI (share/QNN/converter/jni/QnnWrapperUtils.hpp, QnnModel.cpp)
struct ModelGraphInfo {
  Qnn_GraphHandle_t graph;
  char* graphName;
  Qnn_Tensor_t* inputTensors;
  uint32_t numInputTensors;
  Qnn_Tensor_t* outputTensors;
  uint32_t numOutputTensors;
};
struct ModelGraphConfigInfo {
  char* graphName;
  const QnnGraph_Config_t** graphConfigs;
};
typedef int (*ComposeGraphsFn)(Qnn_BackendHandle_t, QNN_INTERFACE_VER_TYPE, Qnn_ContextHandle_t,
                               const ModelGraphConfigInfo**, const uint32_t, ModelGraphInfo***, uint32_t*, bool,
                               QnnLog_Callback_t, QnnLog_Level_t);

bool readFile(const std::string& path, std::vector<uint8_t>& b) {
  FILE* f = fopen(path.c_str(), "rb");
  if (!f) return false;
  fseek(f, 0, SEEK_END);
  long n = ftell(f);
  fseek(f, 0, SEEK_SET);
  if (n <= 0) {
    fclose(f);
    return false;
  }
  b.resize((size_t)n);
  bool ok = fread(b.data(), 1, b.size(), f) == b.size();
  fclose(f);
  return ok;
}

bool initGraphTensors(GraphInfo& g, uint32_t ni, const Qnn_Tensor_t* ins, uint32_t no, const Qnn_Tensor_t* outs) {
  g.ins.resize(ni);
  g.outs.resize(no);
  for (uint32_t i = 0; i < ni; i++)
    if (!g.ins[i].init(ins[i])) return false;
  for (uint32_t i = 0; i < no; i++)
    if (!g.outs[i].init(outs[i])) return false;
  return true;
}

int findT(const std::vector<TensorInfo>& v, const char* name) {
  for (size_t i = 0; i < v.size(); i++)
    if (v[i].name == name) return (int)i;
  return -1;
}

}  // namespace

bool TensorInfo::init(const Qnn_Tensor_t& src) {
  tmpl = src;
  name = src.v1.name ? src.v1.name : "";
  dims.assign(src.v1.dimensions, src.v1.dimensions + src.v1.rank);
  tmpl.v1.name = name.c_str();
  tmpl.v1.dimensions = dims.data();
  if (tmpl.version == QNN_TENSOR_VERSION_2) {
    tmpl.v2.isDynamicDimensions = nullptr;
    memset(&tmpl.v2.sparseParams, 0, sizeof(tmpl.v2.sparseParams));
  }
  dt = src.v1.dataType;
  n = 1;
  for (auto v : dims) n *= v;
  switch (dt) {
    case QNN_DATATYPE_FLOAT_32: esz = 4; break;
    case QNN_DATATYPE_FLOAT_16: esz = 2; break;
    case QNN_DATATYPE_UFIXED_POINT_16: esz = 2, qmin = 0, qmax = 65535; break;
    case QNN_DATATYPE_SFIXED_POINT_16: esz = 2, qmin = -32768, qmax = 32767; break;
    case QNN_DATATYPE_UFIXED_POINT_8: esz = 1, qmin = 0, qmax = 255; break;
    case QNN_DATATYPE_SFIXED_POINT_8: esz = 1, qmin = -128, qmax = 127; break;
    default: logf(kLogError, "tensor %s: unsupported data type 0x%x", name.c_str(), dt); return false;
  }
  if (qmax) {
    const Qnn_QuantizeParams_t& q = src.v1.quantizeParams;
    if (q.encodingDefinition != QNN_DEFINITION_DEFINED ||
        q.quantizationEncoding != QNN_QUANTIZATION_ENCODING_SCALE_OFFSET) {
      logf(kLogError, "tensor %s: needs a per-tensor scale/offset encoding", name.c_str());
      return false;
    }
    scale = q.scaleOffsetEncoding.scale;
    offset = q.scaleOffsetEncoding.offset;
    inv = 1.f / scale;
  }
  tmpl.v1.memType = QNN_TENSORMEMTYPE_RAW;
  tmpl.v1.clientBuf.data = nullptr;
  tmpl.v1.clientBuf.dataSize = (uint32_t)bytes();
  return true;
}

std::string TensorInfo::desc() const {
  char b[256];
  std::string d;
  for (size_t i = 0; i < dims.size(); i++) d += (i ? "x" : "") + std::to_string(dims[i]);
  if (qmax)
    snprintf(b, sizeof(b), "%-10s [%s] %s %zu B scale=%.6g offset=%d range=[%.4g, %.4g]", name.c_str(), d.c_str(),
             dtName(dt), n * esz, scale, offset, scale * (double)(qmin + offset), scale * (double)(qmax + offset));
  else
    snprintf(b, sizeof(b), "%-10s [%s] %s %zu B", name.c_str(), d.c_str(), dtName(dt), n * esz);
  return b;
}

void TensorInfo::scan(const uint8_t* buf, SatCount* sat) const {
  if (!qmax || !sat) return;
  size_t l = 0, h = 0;
  if (esz == 2) {
    for (size_t i = 0; i < n; i++) {
      int64_t v = dt == QNN_DATATYPE_SFIXED_POINT_16 ? (int64_t)((const int16_t*)buf)[i]
                                                      : (int64_t)((const uint16_t*)buf)[i];
      l += v == qmin, h += v == qmax;
    }
  } else {
    for (size_t i = 0; i < n; i++) {
      int64_t v = dt == QNN_DATATYPE_SFIXED_POINT_8 ? (int64_t)((const int8_t*)buf)[i] : (int64_t)buf[i];
      l += v == qmin, h += v == qmax;
    }
  }
  sat->count(n, l, h);
}

void TensorInfo::put(uint8_t* buf, const float* x, size_t i0, size_t cnt, SatCount* sat) const {
  if (qmax && sat) {
    const float rlo = scale * (float)(qmin + offset), rhi = scale * (float)(qmax + offset);
    size_t l = 0, h = 0;
    for (size_t i = 0; i < cnt; i++) l += x[i] < rlo, h += x[i] > rhi;
    sat->count(cnt, l, h);
  }
  switch (dt) {
    case QNN_DATATYPE_FLOAT_32: memcpy(buf + i0 * 4, x, cnt * 4); break;
    case QNN_DATATYPE_UFIXED_POINT_16: {
      uint16_t* p = (uint16_t*)buf + i0;
      for (size_t i = 0; i < cnt; i++) {
        long v = lrintf(x[i] * inv) - offset;
        p[i] = (uint16_t)(v < 0 ? 0 : v > 65535 ? 65535 : v);
      }
      break;
    }
    default: {
      for (size_t i = 0; i < cnt; i++) {
        int64_t v = (int64_t)lrintf(x[i] * inv) - offset;
        v = std::min(std::max(v, qmin), qmax);
        if (dt == QNN_DATATYPE_SFIXED_POINT_16) ((int16_t*)buf)[i0 + i] = (int16_t)v;
        else if (dt == QNN_DATATYPE_UFIXED_POINT_8) buf[i0 + i] = (uint8_t)v;
        else if (dt == QNN_DATATYPE_SFIXED_POINT_8) ((int8_t*)buf)[i0 + i] = (int8_t)v;
        else if (dt == QNN_DATATYPE_FLOAT_16) ((_Float16*)buf)[i0 + i] = (_Float16)x[i];
      }
    }
  }
}

void TensorInfo::get(const uint8_t* buf, float* y, size_t i0, size_t cnt) const {
  switch (dt) {
    case QNN_DATATYPE_FLOAT_32: memcpy(y, buf + i0 * 4, cnt * 4); break;
    case QNN_DATATYPE_UFIXED_POINT_16: {
      const uint16_t* p = (const uint16_t*)buf + i0;
      for (size_t i = 0; i < cnt; i++) y[i] = scale * (float)((int32_t)p[i] + offset);
      break;
    }
    case QNN_DATATYPE_SFIXED_POINT_16:
      for (size_t i = 0; i < cnt; i++) y[i] = scale * (float)(((const int16_t*)buf)[i0 + i] + offset);
      break;
    case QNN_DATATYPE_UFIXED_POINT_8:
      for (size_t i = 0; i < cnt; i++) y[i] = scale * (float)(buf[i0 + i] + offset);
      break;
    case QNN_DATATYPE_SFIXED_POINT_8:
      for (size_t i = 0; i < cnt; i++) y[i] = scale * (float)(((const int8_t*)buf)[i0 + i] + offset);
      break;
    case QNN_DATATYPE_FLOAT_16:
      for (size_t i = 0; i < cnt; i++) y[i] = (float)((const _Float16*)buf)[i0 + i];
      break;
    default: break;
  }
}

bool GraphInfo::bind() {
  iErb = findT(ins, "feat_erb");
  iSpec = findT(ins, "feat_spec");
  iC0h = findT(ins, "c0_hist");
  iHin = findT(ins, "h_in");
  oGains = findT(outs, "gains");
  oCoefs = findT(outs, "coefs");
  oLsnr = findT(outs, "lsnr");
  oC0 = findT(outs, "c0_new");
  if (oC0 < 0) oC0 = findT(outs, "c0_out");
  oH = findT(outs, "h_out");
  if (iErb < 0 || iSpec < 0 || iC0h < 0 || iHin < 0 || oGains < 0 || oCoefs < 0 || oLsnr < 0 || oC0 < 0 || oH < 0)
    return false;
  T = (int)(ins[iErb].n / NB_ERB) - 2;
  const size_t t = (size_t)T, fr = (size_t)NB_DF * C0CH;
  c0Full = outs[oC0].n == 4 * fr;
  return T >= 1 && ins[iErb].n == (t + 2) * NB_ERB && ins[iSpec].n == (t + 2) * NB_DF * 2 &&
         ins[iC0h].n == 4 * fr && ins[iHin].n == (size_t)NHID * HID && outs[oGains].n == t * NB_ERB &&
         outs[oCoefs].n == t * NB_DF * ORD * 2 && outs[oLsnr].n == t && (outs[oC0].n == fr || c0Full) &&
         outs[oH].n == (size_t)NHID * HID;
}

size_t GraphInfo::inBytes() const {
  size_t b = 0;
  for (auto& t : ins) b += t.bytes();
  return b;
}

size_t GraphInfo::outBytes() const {
  size_t b = 0;
  for (auto& t : outs) b += t.bytes();
  return b;
}

// ---------------------------------------------------------------------------------------------
// Engine
Engine::~Engine() { unload(); }

std::string Engine::errStr(Qnn_ErrorHandle_t e) const {
  const char* msg = nullptr;
  char buf[256];
  if (qnn_ && qnn_->errorGetMessage && qnn_->errorGetMessage(e, &msg) == QNN_SUCCESS && msg)
    snprintf(buf, sizeof(buf), "%" PRIu64 " (%s)", (uint64_t)e, msg);
  else
    snprintf(buf, sizeof(buf), "%" PRIu64, (uint64_t)e);
  return buf;
}

bool Engine::loadBackend(const LoadConfig& cfg, std::string* err) {
  if (!lib_) lib_ = dlopen(cfg.backend.c_str(), RTLD_NOW | RTLD_LOCAL);
  if (!lib_) {
    *err = "dlopen " + cfg.backend + ": " + dlerror();
    return false;
  }
  typedef Qnn_ErrorHandle_t (*GetProvidersFn)(const QnnInterface_t***, uint32_t*);
  auto gp = (GetProvidersFn)dlsym(lib_, "QnnInterface_getProviders");
  const QnnInterface_t** prov = nullptr;
  uint32_t np = 0;
  if (!gp || gp(&prov, &np) != QNN_SUCCESS || !np) {
    *err = "QnnInterface_getProviders";
    return false;
  }
  qnn_ = nullptr;
  for (uint32_t i = 0; i < np; i++) {
    if (prov[i]->apiVersion.coreApiVersion.major == QNN_API_VERSION_MAJOR) {
      qnn_ = &prov[i]->QNN_INTERFACE_VER_NAME;
      isHtp_ = prov[i]->backendId == QNN_BACKEND_ID_HTP;
      logf(kLogInfo, "backend %s id=%u core API %u.%u.%u backend API %u.%u.%u",
           prov[i]->providerName ? prov[i]->providerName : "?", prov[i]->backendId,
           prov[i]->apiVersion.coreApiVersion.major, prov[i]->apiVersion.coreApiVersion.minor,
           prov[i]->apiVersion.coreApiVersion.patch, prov[i]->apiVersion.backendApiVersion.major,
           prov[i]->apiVersion.backendApiVersion.minor, prov[i]->apiVersion.backendApiVersion.patch);
      break;
    }
  }
  if (!qnn_) {
    *err = "no provider with core API major " + std::to_string(QNN_API_VERSION_MAJOR);
    return false;
  }
  const char* bid = nullptr;
  buildId_.clear();
  if (qnn_->backendGetBuildId && qnn_->backendGetBuildId(&bid) == QNN_SUCCESS && bid) {
    buildId_ = bid;
    logf(kLogInfo, "backend build id: %s", bid);
  }
  if (qnn_->logCreate) qnn_->logCreate(qnnLogCallback, (QnnLog_Level_t)cfg.qnnLogLevel, &log_);
  Qnn_ErrorHandle_t e = qnn_->backendCreate(log_, nullptr, &backend_);
  if (e != QNN_SUCCESS) {
    backend_ = nullptr;
    *err = "backendCreate: " + errStr(e);
    return false;
  }
  return true;
}

void Engine::printPlatformInfo() {
  const QnnDevice_PlatformInfo_t* pi = nullptr;
  if (!qnn_->deviceGetPlatformInfo || qnn_->deviceGetPlatformInfo(log_, &pi) != QNN_SUCCESS || !pi) return;
  if (pi->version == QNN_DEVICE_PLATFORM_INFO_VERSION_1) {
    for (uint32_t i = 0; i < pi->v1.numHwDevices; i++) {
      const QnnDevice_HardwareDeviceInfo_t& d = pi->v1.hwDevices[i];
      if (d.version != QNN_DEVICE_HARDWARE_DEVICE_INFO_VERSION_1) continue;
      auto* ext = reinterpret_cast<const QnnHtpDevice_DeviceInfoExtension_t*>(d.v1.deviceInfoExtension);
      if (ext && ext->devType == QNN_HTP_DEVICE_TYPE_ON_CHIP)
        logf(kLogInfo, "htp device %u: vtcmMB=%zu socModel=%u arch=v%d cores=%u", d.v1.deviceId,
             ext->onChipDevice.vtcmSize, ext->onChipDevice.socModel, (int)ext->onChipDevice.arch, d.v1.numCores);
    }
  }
  if (qnn_->deviceFreePlatformInfo) qnn_->deviceFreePlatformInfo(log_, pi);
}

bool Engine::createDevice(const LoadConfig& cfg, std::string* err) {
  if (!isHtp_ || !qnn_->deviceCreate) return true;
  if (cfg.platformInfo) printPlatformInfo();
  QnnHtpDevice_CustomConfig_t socCfg;
  memset(&socCfg, 0, sizeof(socCfg));
  QnnDevice_Config_t dc;
  const QnnDevice_Config_t* dcp[2] = {nullptr, nullptr};
  if (cfg.socModel) {
    socCfg.option = QNN_HTP_DEVICE_CONFIG_OPTION_SOC;
    socCfg.socModel = cfg.socModel;
    dc.option = QNN_DEVICE_CONFIG_OPTION_CUSTOM;
    dc.customConfig = &socCfg;
    dcp[0] = &dc;
  }
  Qnn_ErrorHandle_t e = qnn_->deviceCreate(log_, cfg.socModel ? dcp : nullptr, &device_);
  if (e != QNN_SUCCESS) {
    device_ = nullptr;
    *err = "deviceCreate: " + errStr(e);
    return false;
  }
  return true;
}

bool Engine::loadContext(const LoadConfig& cfg, std::string* err) {
  std::vector<uint8_t> blob;
  if (!readFile(cfg.ctx, blob)) {
    *err = "read " + cfg.ctx;
    return false;
  }
  if (!sysLib_) sysLib_ = dlopen(cfg.system.c_str(), RTLD_NOW | RTLD_LOCAL);
  if (!sysLib_) {
    *err = "dlopen " + cfg.system + ": " + dlerror();
    return false;
  }
  typedef Qnn_ErrorHandle_t (*GetSysProvidersFn)(const QnnSystemInterface_t***, uint32_t*);
  auto gp = (GetSysProvidersFn)dlsym(sysLib_, "QnnSystemInterface_getProviders");
  const QnnSystemInterface_t** prov = nullptr;
  uint32_t np = 0;
  if (!gp || gp(&prov, &np) != QNN_SUCCESS || !np) {
    *err = "QnnSystemInterface_getProviders";
    return false;
  }
  const auto& S = prov[0]->QNN_SYSTEM_INTERFACE_VER_NAME;
  QnnSystemContext_Handle_t sc = nullptr;
  if (S.systemContextCreate(&sc) != QNN_SUCCESS) {
    *err = "systemContextCreate";
    return false;
  }
  bool ok = false;
  do {
    const QnnSystemContext_BinaryInfo_t* bi = nullptr;
    Qnn_ContextBinarySize_t biSize = 0;
    if (S.systemContextGetBinaryInfo(sc, blob.data(), blob.size(), &bi, &biSize) != QNN_SUCCESS || !bi) {
      *err = "systemContextGetBinaryInfo";
      break;
    }
    uint32_t ng = 0;
    QnnSystemContext_GraphInfo_t* gs = nullptr;
    if (bi->version == QNN_SYSTEM_CONTEXT_BINARY_INFO_VERSION_1) ng = bi->contextBinaryInfoV1.numGraphs, gs = bi->contextBinaryInfoV1.graphs;
    else if (bi->version == QNN_SYSTEM_CONTEXT_BINARY_INFO_VERSION_2) ng = bi->contextBinaryInfoV2.numGraphs, gs = bi->contextBinaryInfoV2.graphs;
    else if (bi->version == QNN_SYSTEM_CONTEXT_BINARY_INFO_VERSION_3) {
      ng = bi->contextBinaryInfoV3.numGraphs, gs = bi->contextBinaryInfoV3.graphs;
      logf(kLogInfo, "context binary: socModel=%u", bi->contextBinaryInfoV3.socModel);
    }
    if (ng < 1) {
      *err = "no graph in context binary";
      break;
    }
    logf(kLogInfo, "context binary: %s, %zu bytes, info v%d, %u graph(s)", cfg.ctx.c_str(), blob.size(),
         (int)bi->version, ng);
    graphs_.clear();
    graphs_.reserve(ng);
    bool tensorsOk = true;
    for (uint32_t i = 0; i < ng && tensorsOk; i++) {
      // GraphInfo V1/V2/V3 start with the same five fields.
      const QnnSystemContext_GraphInfoV1_t* g1 = &gs[i].graphInfoV1;
      graphs_.emplace_back();
      GraphInfo& g = graphs_.back();
      g.name = g1->graphName;
      tensorsOk = initGraphTensors(g, g1->numGraphInputs, g1->graphInputs, g1->numGraphOutputs, g1->graphOutputs);
    }
    if (!tensorsOk) {
      *err = "unsupported graph tensors";
      break;
    }
    Qnn_ErrorHandle_t e = qnn_->contextCreateFromBinary(backend_, device_, nullptr, blob.data(), blob.size(), &ctx_, prof_);
    if (e != QNN_SUCCESS) {
      ctx_ = nullptr;
      *err = "contextCreateFromBinary: " + errStr(e);
      break;
    }
    ok = true;
    for (GraphInfo& g : graphs_) {
      e = qnn_->graphRetrieve(ctx_, g.name.c_str(), &g.h);
      if (e != QNN_SUCCESS) {
        *err = "graphRetrieve(" + g.name + "): " + errStr(e);
        ok = false;
        break;
      }
      logf(kLogInfo, "graph %s retrieved", g.name.c_str());
    }
  } while (false);
  S.systemContextFree(sc);
  return ok;
}

bool Engine::loadModelLibs(const LoadConfig& cfg, std::string* err) {
  Qnn_ErrorHandle_t e = qnn_->contextCreate(backend_, device_, nullptr, &ctx_);
  if (e != QNN_SUCCESS) {
    ctx_ = nullptr;
    *err = "contextCreate: " + errStr(e);
    return false;
  }
  graphs_.clear();
  graphs_.reserve(cfg.modelLibs.size() * 2);
  for (const std::string& path : cfg.modelLibs) {
    void* lib = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!lib) {
      *err = "dlopen " + path + ": " + dlerror();
      return false;
    }
    modelLibs_.push_back(lib);
    auto compose = (ComposeGraphsFn)dlsym(lib, "QnnModel_composeGraphs");
    if (!compose) {
      *err = "no QnnModel_composeGraphs in " + path;
      return false;
    }
    ModelGraphInfo** gi = nullptr;
    uint32_t ng = 0;
    if (compose(backend_, *qnn_, ctx_, nullptr, 0, &gi, &ng, false, qnnLogCallback,
                (QnnLog_Level_t)cfg.qnnLogLevel) != 0 || ng < 1) {
      *err = "QnnModel_composeGraphs " + path;
      return false;
    }
    if (graphs_.size() + ng > graphs_.capacity()) {
      *err = "too many graphs in " + path;
      return false;
    }
    for (uint32_t i = 0; i < ng; i++) {
      graphs_.emplace_back();
      GraphInfo& g = graphs_.back();
      g.name = gi[i]->graphName;
      g.h = gi[i]->graph;
      if (!initGraphTensors(g, gi[i]->numInputTensors, gi[i]->inputTensors, gi[i]->numOutputTensors,
                            gi[i]->outputTensors)) {
        *err = "unsupported graph tensors in " + path;
        return false;
      }
      e = qnn_->graphFinalize(g.h, prof_, nullptr);
      if (e != QNN_SUCCESS) {
        *err = "graphFinalize " + g.name + ": " + errStr(e);
        return false;
      }
      logf(kLogInfo, "model library %s: graph %s composed and finalized", path.c_str(), g.name.c_str());
    }
  }
  return true;
}

bool Engine::load(const LoadConfig& cfg, std::string* err) {
  std::string dummy;
  if (!err) err = &dummy;
  unload();
  const double t0 = nowUs();
  for (const std::string& p : cfg.preload) {
    void* h = dlopen(p.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!h) {
      *err = "dlopen " + p + ": " + dlerror();
      return false;
    }
    preloaded_.push_back(h);  // kept for the life of the process
  }
  bool ok = loadBackend(cfg, err) && createDevice(cfg, err);
  if (ok && cfg.profile && qnn_->profileCreate(backend_, QNN_PROFILE_LEVEL_BASIC, &prof_) != QNN_SUCCESS) {
    logf(kLogWarn, "profileCreate failed, continuing without");
    prof_ = nullptr;
  }
  ok = ok && (cfg.ctx.empty() ? loadModelLibs(cfg, err) : loadContext(cfg, err));
  GraphInfo* g1 = nullptr;
  GraphInfo* gB = nullptr;
  for (size_t i = 0; ok && i < graphs_.size(); i++) {
    GraphInfo& g = graphs_[i];
    if (!g.bind()) {
      *err = "graph " + g.name + " does not match the dfn3_net.py interface";
      ok = false;
      break;
    }
    logf(kLogInfo, "graph %s: %d hop(s) per call, I/O per call %zu B in + %zu B out", g.name.c_str(), g.T,
         g.inBytes(), g.outBytes());
    for (auto& t : g.ins) logf(kLogDebug, "  in  %s", t.desc().c_str());
    for (auto& t : g.outs) logf(kLogDebug, "  out %s", t.desc().c_str());
    if (g.T == 1 && !g1) g1 = &g;
    if (g.T == cfg.blockHops && g.T > 1 && g.T <= kMaxGraphHops && !gB) gB = &g;
  }
  if (ok && !g1) {
    *err = "no 1-hop graph";
    ok = false;
  }
  if (!ok) {
    unload();
    return false;
  }
  step_ = g1;
  block_ = gB;
  // The state lives in the 1-hop graph's c0_hist / h_in encodings; other graphs read it in place
  // when their encodings are the same, else through float.
  for (GraphInfo& g : graphs_) {
    const bool c0Alias = &g == g1 || g.ins[g.iC0h].sameQuant(g1->ins[g1->iC0h]);
    const bool hAlias = &g == g1 || g.ins[g.iHin].sameQuant(g1->ins[g1->iHin]);
    logf(kLogInfo, "state %s: c0 in %s, out %s; h in %s, out %s", g.name.c_str(), c0Alias ? "shared" : "converted",
         g.outs[g.oC0].sameQuant(g1->ins[g1->iC0h]) ? "raw copy" : "requantized", hAlias ? "shared" : "converted",
         g.outs[g.oH].sameQuant(g1->ins[g1->iHin]) ? "raw copy" : "requantized");
  }
  loadMs_ = (nowUs() - t0) / 1e3;
  return true;
}

void Engine::unload() {
  std::string msg;
  if (powerId_) perfRelease(&msg);
  if (qnn_) {
    if (prof_) qnn_->profileFree(prof_);
    if (ctx_) qnn_->contextFree(ctx_, nullptr);
    if (device_ && qnn_->deviceFree) qnn_->deviceFree(device_);
    if (backend_) qnn_->backendFree(backend_);
    if (log_ && qnn_->logFree) qnn_->logFree(log_);
  }
  prof_ = nullptr;
  ctx_ = nullptr;
  device_ = nullptr;
  backend_ = nullptr;
  log_ = nullptr;
  perf_ = nullptr;
  graphs_.clear();
  step_ = block_ = nullptr;
}

bool Engine::perfVote(const std::string& preset, int rpcLatencyUs, std::string* msg) {
  std::string dummy;
  if (!msg) msg = &dummy;
  if (!isHtp_ || preset == "none") {
    *msg = "perf: no vote (" + std::string(isHtp_ ? "preset none" : "not HTP") + ")";
    return true;
  }
  if (!perf_) {
    QnnDevice_Infrastructure_t infra = nullptr;
    if (!qnn_->deviceGetInfrastructure || qnn_->deviceGetInfrastructure(&infra) != QNN_SUCCESS || !infra) {
      *msg = "perf: no device infrastructure, skipping power vote";
      return true;
    }
    auto* hi = reinterpret_cast<QnnHtpDevice_Infrastructure_t*>(infra);
    if (hi->infraType != QNN_HTP_DEVICE_INFRASTRUCTURE_TYPE_PERF) {
      *msg = "perf: no perf infrastructure, skipping power vote";
      return true;
    }
    perf_ = &hi->perfInfra;
  }
  if (!powerId_ && perf_->createPowerConfigId(0, 0, &powerId_) != QNN_SUCCESS) {
    powerId_ = 0;
    *msg = "perf: createPowerConfigId failed";
    return false;
  }
  QnnHtpPerfInfrastructure_PowerConfig_t p;
  memset(&p, 0, sizeof(p));
  p.option = QNN_HTP_PERF_INFRASTRUCTURE_POWER_CONFIGOPTION_DCVS_V3;
  auto& d = p.dcvsV3Config;
  d.contextId = powerId_;
  d.setDcvsEnable = 1;
  d.setSleepLatency = 1;
  d.setSleepDisable = 1;
  d.setBusParams = 1;
  d.setCoreParams = 1;
  QnnHtpPerfInfrastructure_VoltageCorner_t mn, tg, mx;
  if (preset == "burst") {
    d.dcvsEnable = 0, d.powerMode = QNN_HTP_PERF_INFRASTRUCTURE_POWERMODE_PERFORMANCE_MODE;
    d.sleepLatency = 40, d.sleepDisable = 1;
    mn = tg = mx = DCVS_VOLTAGE_VCORNER_MAX_VOLTAGE_CORNER;
  } else if (preset == "high") {
    d.dcvsEnable = 0, d.powerMode = QNN_HTP_PERF_INFRASTRUCTURE_POWERMODE_PERFORMANCE_MODE;
    d.sleepLatency = 100, d.sleepDisable = 0;
    mn = tg = mx = DCVS_VOLTAGE_VCORNER_TURBO;
  } else if (preset == "balanced") {
    d.dcvsEnable = 1, d.powerMode = QNN_HTP_PERF_INFRASTRUCTURE_POWERMODE_ADJUST_UP_DOWN;
    d.sleepLatency = 1000, d.sleepDisable = 0;
    mn = DCVS_VOLTAGE_VCORNER_SVS, tg = DCVS_VOLTAGE_VCORNER_NOM_PLUS, mx = DCVS_VOLTAGE_VCORNER_TURBO;
  } else if (preset == "low") {
    d.dcvsEnable = 1, d.powerMode = QNN_HTP_PERF_INFRASTRUCTURE_POWERMODE_POWER_SAVER_MODE;
    d.sleepLatency = 1000, d.sleepDisable = 0;
    mn = DCVS_VOLTAGE_VCORNER_SVS2, tg = DCVS_VOLTAGE_VCORNER_SVS, mx = DCVS_VOLTAGE_VCORNER_SVS_PLUS;
  } else {
    *msg = "perf: unknown preset " + preset;
    return false;
  }
  d.busVoltageCornerMin = d.coreVoltageCornerMin = mn;
  d.busVoltageCornerTarget = d.coreVoltageCornerTarget = tg;
  d.busVoltageCornerMax = d.coreVoltageCornerMax = mx;
  QnnHtpPerfInfrastructure_PowerConfig_t lat;
  memset(&lat, 0, sizeof(lat));
  lat.option = QNN_HTP_PERF_INFRASTRUCTURE_POWER_CONFIGOPTION_RPC_CONTROL_LATENCY;
  lat.rpcControlLatencyConfig = (uint32_t)rpcLatencyUs;
  const QnnHtpPerfInfrastructure_PowerConfig_t* cfgs[] = {&p, &lat, nullptr};
  Qnn_ErrorHandle_t e = perf_->setPowerConfig(powerId_, cfgs);
  *msg = "perf: preset " + preset + " rpc_control_latency=" + std::to_string(rpcLatencyUs) + "us -> " +
         (e == QNN_SUCCESS ? std::string("ok") : errStr(e));
  return e == QNN_SUCCESS;
}

bool Engine::perfRelease(std::string* msg) {
  std::string dummy;
  if (!msg) msg = &dummy;
  if (!powerId_ || !perf_) {
    powerId_ = 0;
    *msg = "perf: no vote to release";
    return true;
  }
  Qnn_ErrorHandle_t e = perf_->destroyPowerConfigId(powerId_);
  *msg = "perf: vote released (destroyPowerConfigId " + std::to_string(powerId_) + ") -> " +
         (e == QNN_SUCCESS ? std::string("ok") : errStr(e));
  powerId_ = 0;
  return e == QNN_SUCCESS;
}

Qnn_ErrorHandle_t Engine::execute(const GraphInfo& g, Qnn_Tensor_t* in, uint32_t nIn, Qnn_Tensor_t* out,
                                  uint32_t nOut, ExecTiming* t, AfterExec* after) {
  const double w0 = nowUs();
  std::lock_guard<std::mutex> lock(execMu_);
  ExecTiming tt;
  tt.startUs = nowUs();
  tt.waitUs = tt.startUs - w0;
  Qnn_ErrorHandle_t e = qnn_->graphExecute(g.h, in, nIn, out, nOut, prof_, nullptr);
  tt.endUs = nowUs();
  if (t) *t = tt;
  if (after) after->afterExecute(g, e, tt);
  return e;
}

// ---------------------------------------------------------------------------------------------
// Channel
bool Channel::initIo(GraphIo& io, const GraphInfo* g, std::string* err) {
  io.g = g;
  io.inBuf.assign(g->ins.size(), {});
  io.outBuf.assign(g->outs.size(), {});
  io.inT.resize(g->ins.size());
  io.outT.resize(g->outs.size());
  io.inSat.assign(g->ins.size(), SatCount());
  io.outSat.assign(g->outs.size(), SatCount());
  io.c0Alias = g == ref_ || g->ins[g->iC0h].sameQuant(ref_->ins[ref_->iC0h]);
  io.hAlias = g == ref_ || g->ins[g->iHin].sameQuant(ref_->ins[ref_->iHin]);
  for (size_t i = 0; i < g->ins.size(); i++) {
    io.inT[i] = g->ins[i].tmpl;
    uint8_t* data;
    if ((int)i == g->iC0h && io.c0Alias) data = c0State_.data();
    else if ((int)i == g->iHin && io.hAlias) data = hState_.data();
    else {
      io.inBuf[i].assign(g->ins[i].bytes(), 0);
      data = io.inBuf[i].data();
    }
    io.inT[i].v1.clientBuf.data = data;
    io.inT[i].v1.clientBuf.dataSize = (uint32_t)g->ins[i].bytes();
  }
  for (size_t i = 0; i < g->outs.size(); i++) {
    io.outT[i] = g->outs[i].tmpl;
    io.outBuf[i].assign(g->outs[i].bytes(), 0);
    io.outT[i].v1.clientBuf.data = io.outBuf[i].data();
    io.outT[i].v1.clientBuf.dataSize = (uint32_t)g->outs[i].bytes();
  }
  if (g->T > kMaxGraphHops || g->T + ORD + LOOKAHEAD > DfnCpu::RING) {
    *err = "graph " + g->name + ": too many hops per call";
    return false;
  }
  return true;
}

bool Channel::init(Engine* e, const Dsp* dsp, const ChannelConfig& cfg, std::string* err) {
  std::string dummy;
  if (!err) err = &dummy;
  if (!e || !e->loaded()) {
    *err = "engine not loaded";
    return false;
  }
  e_ = e;
  cfg_ = cfg;
  cpu_.reset(new DfnCpu(dsp));
  cpu_->minDb = cfg.minDb, cpu_->maxErb = cfg.maxErb, cpu_->maxDf = cfg.maxDf;
  cpu_->predictReset = cfg.predictReset;
  ref_ = e->step();
  c0State_.assign(ref_->ins[ref_->iC0h].bytes(), 0);
  hState_.assign(ref_->ins[ref_->iHin].bytes(), 0);
  nGraphs_ = 1;
  if (!initIo(io_[0], ref_, err)) return false;
  if (cfg.useBlock && e->block()) {
    if (!cfg.predictReset) {
      *err = "the multi-hop graph needs predictReset";
      return false;
    }
    if (!initIo(io_[1], e->block(), err)) return false;
    nGraphs_ = 2;
  }
  const int tMax = nGraphs_ == 2 ? std::max(1, io_[1].g->T) : 1;
  erbTmp_.assign((size_t)(tMax + 2) * NB_ERB, 0.f);
  specTmp_.assign((size_t)(tMax + 2) * NB_DF * 2, 0.f);
  c0Tmp_.assign((size_t)4 * NB_DF * C0CH, 0.f);
  hTmp_.assign((size_t)NHID * HID, 0.f);
  gains_.assign((size_t)tMax * NB_ERB, 0.f);
  coefs_.assign((size_t)tMax * NB_DF * ORD * 2, 0.f);
  lsnr_.assign((size_t)tMax, 0.f);
  reset();
  return true;
}

void Channel::reset() {
  cpu_->reset();
  std::fill(c0Tmp_.begin(), c0Tmp_.end(), 0.f);
  std::fill(hTmp_.begin(), hTmp_.end(), 0.f);
  ref_->ins[ref_->iC0h].put(c0State_.data(), c0Tmp_.data(), 0, c0Tmp_.size(), nullptr);
  ref_->ins[ref_->iHin].put(hState_.data(), hTmp_.data(), 0, hTmp_.size(), nullptr);
}

bool Channel::runGraph(GraphIo& io, int64_t q0, ProcessInfo* info, Qnn_ErrorHandle_t* err, ChannelHooks* hooks) {
  const GraphInfo& g = *io.g;
  const size_t T = (size_t)g.T, c0Frame = (size_t)NB_DF * C0CH;
  const bool sat = cfg_.countSaturation;
  const TensorInfo& rc0 = ref_->ins[ref_->iC0h];
  const TensorInfo& rh = ref_->ins[ref_->iHin];
  cpu_->window(q0, g.T, erbTmp_.data(), specTmp_.data());
  g.ins[g.iErb].put(io.inBuf[g.iErb].data(), erbTmp_.data(), 0, g.ins[g.iErb].n, sat ? &io.inSat[g.iErb] : nullptr);
  g.ins[g.iSpec].put(io.inBuf[g.iSpec].data(), specTmp_.data(), 0, g.ins[g.iSpec].n,
                     sat ? &io.inSat[g.iSpec] : nullptr);
  if (!io.c0Alias) {
    rc0.get(c0State_.data(), c0Tmp_.data(), 0, 4 * c0Frame);
    g.ins[g.iC0h].put(io.inBuf[g.iC0h].data(), c0Tmp_.data(), 0, 4 * c0Frame, sat ? &io.inSat[g.iC0h] : nullptr);
  }
  if (!io.hAlias) {
    rh.get(hState_.data(), hTmp_.data(), 0, hTmp_.size());
    g.ins[g.iHin].put(io.inBuf[g.iHin].data(), hTmp_.data(), 0, hTmp_.size(), sat ? &io.inSat[g.iHin] : nullptr);
  }
  struct After : Engine::AfterExec {
    ChannelHooks* h;
    void afterExecute(const GraphInfo& gr, Qnn_ErrorHandle_t e, const ExecTiming& t) override {
      if (e == QNN_SUCCESS) h->afterExecute(gr, t);
    }
  } after;
  after.h = hooks;
  ExecTiming t;
  Qnn_ErrorHandle_t e = e_->execute(g, io.inT.data(), (uint32_t)io.inT.size(), io.outT.data(),
                                    (uint32_t)io.outT.size(), &t, hooks ? &after : nullptr);
  if (info->calls1 + info->callsB == 0) info->firstExecStartUs = t.startUs;
  info->lastExecEndUs = t.endUs;
  info->execUs += t.endUs - t.startUs;
  info->waitUs += t.waitUs;
  (g.T == 1 ? info->calls1 : info->callsB)++;
  if (e != QNN_SUCCESS) {
    if (err) *err = e;
    return false;
  }
  if (sat)
    for (size_t j = 0; j < g.outs.size(); j++) g.outs[j].scan(io.outBuf[j].data(), &io.outSat[j]);
  g.outs[g.oGains].get(io.outBuf[g.oGains].data(), gains_.data(), 0, T * NB_ERB);
  g.outs[g.oCoefs].get(io.outBuf[g.oCoefs].data(), coefs_.data(), 0, T * NB_DF * ORD * 2);
  g.outs[g.oLsnr].get(io.outBuf[g.oLsnr].data(), lsnr_.data(), 0, T);
  uint8_t* hb = c0State_.data();
  const size_t esz = rc0.esz;
  const TensorInfo& c0o = g.outs[g.oC0];
  const uint8_t* c0b = io.outBuf[g.oC0].data();
  if (g.c0Full) {
    if (c0o.sameQuant(rc0)) memcpy(hb, c0b, 4 * c0Frame * esz);
    else {
      c0o.get(c0b, c0Tmp_.data(), 0, 4 * c0Frame);
      rc0.put(hb, c0Tmp_.data(), 0, 4 * c0Frame, nullptr);
    }
  } else {
    memmove(hb, hb + c0Frame * esz, 3 * c0Frame * esz);
    if (c0o.sameQuant(rc0)) memcpy(hb + 3 * c0Frame * esz, c0b, c0Frame * esz);
    else {
      c0o.get(c0b, c0Tmp_.data(), 0, c0Frame);
      rc0.put(hb, c0Tmp_.data(), 3 * c0Frame, c0Frame, nullptr);
    }
  }
  const TensorInfo& ho = g.outs[g.oH];
  if (ho.sameQuant(rh)) memcpy(hState_.data(), io.outBuf[g.oH].data(), hState_.size());
  else {
    ho.get(io.outBuf[g.oH].data(), hTmp_.data(), 0, hTmp_.size());
    rh.put(hState_.data(), hTmp_.data(), 0, hTmp_.size(), nullptr);
  }
  if (hooks) hooks->outputs(g, g.T, gains_.data(), coefs_.data(), lsnr_.data());
  return true;
}

bool Channel::process(const float* in, float* out, int nHops, ProcessInfo* info, Qnn_ErrorHandle_t* err,
                      ChannelHooks* hooks) {
  ProcessInfo local;
  if (!info) info = &local;
  *info = ProcessInfo();
  const int TB = nGraphs_ == 2 ? io_[1].g->T : 0;
  int np = 0;
  // Analysed hops not yet through a graph go through the 1-hop graph, in order.
  auto flush1 = [&]() -> bool {
    for (int k = 0; k < np; k++) {
      if (!runGraph(io_[0], pend_[k].q, info, err, hooks)) return false;
      cpu_->post(pend_[k].q, gains_.data(), coefs_.data(), lsnr_[0], out + (size_t)pend_[k].hop * HOP);
    }
    np = 0;
    return true;
  };
  for (int k = 0; k < nHops; k++) {
    if (cpu_->pre(in + (size_t)k * HOP)) {
      pend_[np++] = {cpu_->p - 1, k};
      if (TB == 0) {
        if (!flush1()) return false;
      } else if (np == TB) {
        if (!runGraph(io_[1], pend_[0].q, info, err, hooks)) return false;
        for (int j = 0; j < TB; j++)
          cpu_->post(pend_[j].q, gains_.data() + (size_t)j * NB_ERB, coefs_.data() + (size_t)j * NB_DF * ORD * 2,
                     lsnr_[j], out + (size_t)pend_[j].hop * HOP);
        np = 0;
      }
    } else {  // silence skip (cannot happen with predictReset): keep the output order
      if (!flush1()) return false;
      info->skipped++;
      memset(out + (size_t)k * HOP, 0, HOP * sizeof(float));
    }
  }
  return flush1();
}

}  // namespace gk3dn
