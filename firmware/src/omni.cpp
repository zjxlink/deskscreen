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

std::string omni_error_message(int status, const std::string &body) {
  std::string server;
  JsonDocument doc;
  if (!deserializeJson(doc, body)) {
    const char *m = doc["error"]["message"] | doc["message"] | (const char *)nullptr;
    if (m) server = m;
  }
  if (server.size() > 120) {
    size_t n = 120;
    while (n > 0 && ((uint8_t)server[n] & 0xC0) == 0x80) n--;  // 别切出半个汉字
    server = server.substr(0, n) + "…";
  }
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
