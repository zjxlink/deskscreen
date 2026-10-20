#include "omni.h"

#include <ArduinoJson.h>
#include <string.h>

static const char kB64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

size_t b64_encode(const uint8_t *in, size_t n, char *out) {
  size_t o = 0;
  size_t i = 0;
  for (; i + 3 <= n; i += 3) {
    uint32_t v = (uint32_t)in[i] << 16 | (uint32_t)in[i + 1] << 8 | in[i + 2];
    out[o++] = kB64[v >> 18 & 63];
    out[o++] = kB64[v >> 12 & 63];
    out[o++] = kB64[v >> 6 & 63];
    out[o++] = kB64[v & 63];
  }
  if (i < n) {
    uint32_t v = (uint32_t)in[i] << 16 | (i + 1 < n ? (uint32_t)in[i + 1] << 8 : 0);
    out[o++] = kB64[v >> 18 & 63];
    out[o++] = kB64[v >> 12 & 63];
    out[o++] = i + 1 < n ? kB64[v >> 6 & 63] : '=';
    out[o++] = '=';
  }
  return o;
}

static int b64_value(char c) {
  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= 'a' && c <= 'z') return c - 'a' + 26;
  if (c >= '0' && c <= '9') return c - '0' + 52;
  if (c == '+' || c == '-') return 62;
  if (c == '/' || c == '_') return 63;
  return -1;
}

size_t B64Decoder::feed(const char *in, size_t n, uint8_t *out) {
  size_t o = 0;
  for (size_t i = 0; i < n; i++) {
    if (in[i] == '=') {
      reset();  // 这一段结束，剩下不满一个字节的位丢掉
      continue;
    }
    int v = b64_value(in[i]);
    if (v < 0) continue;  // 换行、反斜杠转义之类的跳过
    acc_ = acc_ << 6 | v;
    bits_ += 6;
    if (bits_ >= 8) {
      bits_ -= 8;
      out[o++] = (uint8_t)(acc_ >> bits_);
    }
  }
  acc_ &= (1u << bits_) - 1;
  return o;
}

void B64Decoder::reset() {
  acc_ = 0;
  bits_ = 0;
}

static void put32(uint8_t *p, uint32_t v) {
  p[0] = v;
  p[1] = v >> 8;
  p[2] = v >> 16;
  p[3] = v >> 24;
}

void wav_header(uint8_t out[44], uint32_t samples, uint32_t rate) {
  uint32_t data = samples * 2;
  memcpy(out, "RIFF", 4);
  put32(out + 4, 36 + data);
  memcpy(out + 8, "WAVEfmt ", 8);
  put32(out + 16, 16);
  out[20] = 1;  // PCM
  out[21] = 0;
  out[22] = 1;  // 单声道
  out[23] = 0;
  put32(out + 24, rate);
  put32(out + 28, rate * 2);
  out[32] = 2;  // 每帧字节数
  out[33] = 0;
  out[34] = 16;
  out[35] = 0;
  memcpy(out + 36, "data", 4);
  put32(out + 40, data);
}

void pcm_normalize(int16_t *pcm, size_t n, int max_gain) {
  int peak = 0;
  for (size_t i = 0; i < n; i++) {
    int a = pcm[i] < 0 ? -pcm[i] : pcm[i];
    if (a > peak) peak = a;
  }
  if (peak == 0) return;
  // 目标峰值 23000（约 -3dB），用 1/256 的定点倍数
  int gain = 23000 * 256 / peak;
  if (gain > max_gain * 256) gain = max_gain * 256;
  if (gain <= 256) return;  // 只放大不缩小
  for (size_t i = 0; i < n; i++) {
    int v = pcm[i] * gain / 256;
    pcm[i] = v > 32767 ? 32767 : v < -32768 ? -32768 : v;
  }
}

Resampler::Resampler(int in_rate, int out_rate) : step_((uint32_t)(((uint64_t)in_rate << 16) / out_rate)) {}

size_t Resampler::feed(const int16_t *in, size_t n, int16_t *out) {
  size_t o = 0;
  for (size_t i = 0; i < n; i++) {
    int32_t s = in[i];
    while (pos_ < 0x10000) {
      out[o++] = (int16_t)(prev_ + (int32_t)(((int64_t)(s - prev_) * pos_) >> 16));
      pos_ += step_;
    }
    pos_ -= 0x10000;
    prev_ = (int16_t)s;
  }
  return o;
}

OmniRequest omni_request(const char *model, const char *voice, const char *system_prompt) {
  static const char kMark[] = "@AUDIO@";
  JsonDocument doc;
  doc["model"] = model;
  doc["stream"] = true;
  doc["stream_options"]["include_usage"] = true;
  doc["modalities"].add("text");
  doc["modalities"].add("audio");
  doc["audio"]["voice"] = voice;
  doc["audio"]["format"] = "wav";
  JsonObject sys = doc["messages"].add<JsonObject>();
  sys["role"] = "system";
  sys["content"] = system_prompt;
  JsonObject user = doc["messages"].add<JsonObject>();
  user["role"] = "user";
  JsonObject part = user["content"].add<JsonObject>();
  part["type"] = "input_audio";
  part["input_audio"]["data"] = "data:;base64,@AUDIO@";
  part["input_audio"]["format"] = "wav";
  std::string json;
  serializeJson(doc, json);
  size_t at = json.find(kMark);
  OmniRequest r;
  r.head = json.substr(0, at);
  r.tail = json.substr(at + sizeof(kMark) - 1);
  return r;
}

bool HttpHead::feed(const char *in, size_t n, size_t &consumed) {
  consumed = 0;
  while (consumed < n && !done_) {
    char c = in[consumed++];
    if (c == '\n') {
      if (!line_.empty() && line_.back() == '\r') line_.pop_back();
      if (line_.empty()) {
        done_ = true;
      } else {
        line();
      }
      line_.clear();
    } else if (line_.size() < 512) {
      line_ += c;
    }
  }
  return done_;
}

static bool starts_with_nocase(const std::string &s, const char *prefix) {
  size_t n = strlen(prefix);
  if (s.size() < n) return false;
  for (size_t i = 0; i < n; i++) {
    char a = s[i], b = prefix[i];
    if (a >= 'A' && a <= 'Z') a += 32;
    if (b >= 'A' && b <= 'Z') b += 32;
    if (a != b) return false;
  }
  return true;
}

void HttpHead::line() {
  if (first_) {
    first_ = false;
    size_t sp = line_.find(' ');
    if (sp != std::string::npos) status_ = atoi(line_.c_str() + sp + 1);
    return;
  }
  if (starts_with_nocase(line_, "transfer-encoding:") && line_.find("chunked") != std::string::npos) chunked_ = true;
}

static int hex_value(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

size_t ChunkedDecoder::feed(const char *in, size_t n, char *out) {
  size_t o = 0;
  size_t i = 0;
  while (i < n && state_ != State::Done) {
    switch (state_) {
      case State::Size: {
        char c = in[i++];
        int h = hex_value(c);
        if (h >= 0) {
          left_ = left_ * 16 + h;
          have_digit_ = true;
        } else if (c == ';' || c == ' ') {
          state_ = State::Ext;
        } else if (c == '\r') {
          state_ = State::SizeLf;
        } else if (c == '\n') {
          state_ = have_digit_ && left_ == 0 ? State::Done : State::Data;
        }
        break;
      }
      case State::Ext:
        if (in[i] == '\r') state_ = State::SizeLf;
        if (in[i] == '\n') state_ = left_ == 0 ? State::Done : State::Data;
        i++;
        break;
      case State::SizeLf:
        if (in[i++] == '\n') state_ = left_ == 0 ? State::Done : State::Data;
        break;
      case State::Data: {
        size_t take = n - i < left_ ? n - i : left_;
        memcpy(out + o, in + i, take);
        o += take;
        i += take;
        left_ -= take;
        if (left_ == 0) state_ = State::DataCr;
        break;
      }
      case State::DataCr:
        state_ = in[i] == '\r' ? State::DataLf : State::Size;
        if (state_ == State::DataLf) i++;
        if (state_ == State::Size && in[i] == '\n') i++;
        have_digit_ = false;
        break;
      case State::DataLf:
        if (in[i] == '\n') i++;
        state_ = State::Size;
        break;
      case State::Done:
        break;
    }
  }
  return o;
}

void OmniStream::feed(const char *in, size_t n) {
  for (size_t i = 0; i < n; i++) {
    if (in[i] == '\n') {
      if (!line_.empty() && line_.back() == '\r') line_.pop_back();
      line();
      line_.clear();
    } else {
      line_ += in[i];
    }
  }
}

void OmniStream::line() {
  if (line_.compare(0, 5, "data:") != 0) return;  // 空行、event:、id: 之类
  size_t start = 5;
  while (start < line_.size() && line_[start] == ' ') start++;
  if (line_.compare(start, std::string::npos, "[DONE]") == 0) {
    done_ = true;
    return;
  }
  JsonDocument doc;
  // 原地解析（不复制字符串），语音数据可能有几十 KB
  if (deserializeJson(doc, &line_[start], line_.size() - start)) return;
  JsonVariant err = doc["error"];
  if (!err.isNull()) {
    const char *msg = err["message"] | (const char *)nullptr;
    error_ = msg ? msg : "服务器返回错误";
    done_ = true;
    return;
  }
  JsonVariant delta = doc["choices"][0]["delta"];
  if (delta.isNull()) return;
  if (const char *c = delta["content"]) content_ += c;
  if (const char *t = delta["audio"]["transcript"]) transcript_ += t;
  if (const char *d = delta["audio"]["data"]) audio(d, strlen(d));
}

void OmniStream::audio(const char *b64, size_t n) {
  // 一次解 1KB，免得在栈上开大数组
  uint8_t buf[768 + 4];
  while (n > 0) {
    size_t take = n < 1024 ? n : 1024;
    size_t got = b64_.feed(b64, take, buf);
    b64 += take;
    n -= take;
    pcm(buf, got);
  }
}

void OmniStream::pcm(const uint8_t *bytes, size_t n) {
  if (!header_checked_) {
    // 有的模型在第一块前面带了 44 字节的 WAV 头，要跳过，否则开头会有一声"咔"
    header_.append((const char *)bytes, n);
    if (header_.size() < 4) return;
    if (header_.compare(0, 4, "RIFF") != 0) {
      header_checked_ = true;
    } else {
      size_t at = header_.find("data", 12);
      if (at == std::string::npos || header_.size() < at + 8) {
        if (header_.size() < 512) return;
        at = 36;  // 找不到 data 段，按标准 44 字节头处理
      }
      header_checked_ = true;
      header_.erase(0, at + 8);
    }
    std::string rest;
    rest.swap(header_);
    if (!rest.empty()) pcm((const uint8_t *)rest.data(), rest.size());
    return;
  }
  audio_bytes_ += n;
  int16_t samples[400];
  size_t count = 0;
  size_t i = 0;
  if (has_carry_ && n > 0) {
    samples[count++] = (int16_t)(carry_ | bytes[0] << 8);
    has_carry_ = false;
    i = 1;
  }
  for (; i + 1 < n; i += 2) {
    samples[count++] = (int16_t)(bytes[i] | bytes[i + 1] << 8);
    if (count == sizeof(samples) / sizeof(samples[0])) {
      if (audio_) audio_(samples, count, ctx_);
      count = 0;
    }
  }
  if (i < n) {
    carry_ = bytes[i];
    has_carry_ = true;
  }
  if (count && audio_) audio_(samples, count, ctx_);
}

std::string omni_error_message(int status, const std::string &body) {
  std::string server;
  JsonDocument doc;
  if (!deserializeJson(doc, body)) {
    const char *m = doc["error"]["message"] | doc["message"] | (const char *)nullptr;
    if (m) server = m;
  }
  if (server.size() > 120) server = server.substr(0, 120) + "…";
  const char *hint = nullptr;
  switch (status) {
    case 401: hint = "API Key 不对或已失效"; break;
    case 403: hint = "这个 Key 没有权限用该模型"; break;
    case 404: hint = "找不到模型，检查模型名称"; break;
    case 429: hint = "请求太频繁或免费额度用完了"; break;
    default: break;
  }
  std::string s;
  if (hint) s = hint;
  if (!server.empty()) s += s.empty() ? server : "（" + server + "）";
  if (s.empty()) s = "服务器返回 " + std::to_string(status);
  return s;
}
