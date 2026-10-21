#pragma once

#include <stddef.h>
#include <stdint.h>

#include <string>

// 阿里云百炼语音对话用到的小工具：base64、重采样、HTTP 响应头和错误说明。
// 和硬件、网络无关，方便在电脑上测试。实时对话的事件见 realtime.h

static constexpr const char *kOmniHost = "dashscope.aliyuncs.com";

// base64 编码 n 个字节，写出 4 * ceil(n / 3) 个字符（不含结尾的 0），返回写出的字符数
size_t b64_encode(const uint8_t *in, size_t n, char *out);

// 流式 base64 解码：可以分段喂，遇到 '=' 就结束当前这一段（服务器每一块都单独编码，带各自的 '='）
class B64Decoder {
 public:
  // out 至少要有 n * 3 / 4 + 3 个字节，返回解出的字节数
  size_t feed(const char *in, size_t n, uint8_t *out);
  void reset();

 private:
  uint32_t acc_ = 0;
  int bits_ = 0;
};

// 线性插值重采样，可以分段喂（回答是 24kHz，转成 16kHz 播放）
class Resampler {
 public:
  Resampler(int in_rate, int out_rate);
  // out 至少要有 n * out_rate / in_rate + 2 个位置，返回写出的采样数
  size_t feed(const int16_t *in, size_t n, int16_t *out);

 private:
  uint32_t step_;      // 每个输出采样前进多少个输入采样，16.16 定点
  uint32_t pos_ = 0;   // 下一个输出采样在 prev_ 和下一个输入采样之间的位置，16.16 定点
  int16_t prev_ = 0;
};

// HTTP 响应头：一个字节一个字节地喂，读到空行为止
class HttpHead {
 public:
  // 返回 true 表示响应头读完了；consumed 为这次用掉的字节数，剩下的是正文
  bool feed(const char *in, size_t n, size_t &consumed);
  bool done() const { return done_; }
  int status() const { return status_; }
  bool chunked() const { return chunked_; }

 private:
  void line();
  std::string line_;
  bool first_ = true;
  bool done_ = false;
  int status_ = 0;
  bool chunked_ = false;
};

// 非 200 响应的正文（JSON）里取出错误说明，取不到就返回空字符串
std::string omni_error_message(int status, const std::string &body);
