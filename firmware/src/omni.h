#pragma once

#include <stddef.h>
#include <stdint.h>

#include <string>

// 阿里云百炼 Qwen-Omni 语音对话：把录音（WAV，base64）发给 OpenAI 兼容接口，
// 流式拿回语音（24kHz 16 位 PCM，base64）和文字。这里只有和硬件、网络无关的部分，方便在电脑上测试

static constexpr const char *kOmniHost = "dashscope.aliyuncs.com";
static constexpr const char *kOmniPath = "/compatible-mode/v1/chat/completions";
static constexpr int kOmniOutRate = 24000;  // 返回的语音采样率

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

// 单声道 16 位 WAV 文件头（44 字节）
void wav_header(uint8_t out[44], uint32_t samples, uint32_t rate);

// 录音音量太小时放大：峰值放大到约 -3dB，最多放大 max_gain 倍
void pcm_normalize(int16_t *pcm, size_t n, int max_gain);

// 线性插值重采样，可以分段喂（24kHz 转 16kHz 播放）
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

// 请求体：JSON 被录音分成前后两段，中间是 base64 的 WAV。录音很大，这样可以边编码边发送，不用整个拼出来
struct OmniRequest {
  std::string head;
  std::string tail;
};
OmniRequest omni_request(const char *model, const char *voice, const char *system_prompt);

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

// HTTP 分块传输解码
class ChunkedDecoder {
 public:
  // out 至少要有 n 个字节，返回解出的字节数
  size_t feed(const char *in, size_t n, char *out);
  bool done() const { return state_ == State::Done; }

 private:
  enum class State { Size, Ext, SizeLf, Data, DataCr, DataLf, Done };
  State state_ = State::Size;
  size_t left_ = 0;
  bool have_digit_ = false;
};

// 解析流式返回的 SSE：语音交给 audio 回调，文字攒在 text() 里
class OmniStream {
 public:
  using AudioFn = void (*)(const int16_t *pcm, size_t n, void *ctx);
  OmniStream(AudioFn audio, void *ctx) : audio_(audio), ctx_(ctx) {}

  // 喂解码后（去掉分块）的正文
  void feed(const char *in, size_t n);
  bool done() const { return done_; }
  // 回答的文字（UTF-8）
  const std::string &text() const { return content_.empty() ? transcript_ : content_; }
  // 服务器返回的错误信息，没有错误时为空
  const std::string &error() const { return error_; }
  size_t audio_bytes() const { return audio_bytes_; }

 private:
  void line();
  void audio(const char *b64, size_t n);
  void pcm(const uint8_t *bytes, size_t n);

  AudioFn audio_;
  void *ctx_;
  std::string line_;
  std::string content_;
  std::string transcript_;
  std::string error_;
  B64Decoder b64_;
  bool done_ = false;
  size_t audio_bytes_ = 0;
  uint8_t carry_ = 0;      // 上一段剩下的半个采样
  bool has_carry_ = false;
  std::string header_;     // 开头如果是 WAV 文件头，先攒起来跳过
  bool header_checked_ = false;
};

// 非 200 响应的正文（JSON）里取出错误说明，取不到就返回空字符串
std::string omni_error_message(int status, const std::string &body);
