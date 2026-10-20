#pragma once

#include <stddef.h>
#include <stdint.h>

#include <string>

#include "omni.h"

// 阿里云百炼 Qwen-Omni-Realtime 实时语音对话（WebSocket）：边说边把麦克风的声音传上去，
// 服务器自己判断一句话说完了（server VAD）就开始回答，语音和文字流式传回来，上下文在一次连接里一直保留。
// 这里是事件的拼装和解析，和硬件、网络无关，可以在电脑上测试

static constexpr const char *kRtPath = "/api-ws/v1/realtime?model=";
static constexpr int kRtInRate = 16000;   // 上传 16kHz 16 位单声道 PCM
static constexpr int kRtOutRate = 24000;  // 回答是 24kHz 16 位单声道 PCM

// 设置里填的是普通模型名（qwen3-omni-flash），实时接口用带 -realtime 的版本
std::string rt_model(const char *model);

// 连上以后第一条：音色、提示词、输入输出格式、服务器断句
std::string rt_session_update(const char *voice, const char *instructions, int silence_ms);

// 只有类型的事件，比如 response.cancel
std::string rt_event(const char *type);

// input_audio_buffer.append：JSON 被 base64 音频分成前后两段，边编码边发
extern const char kRtAppendHead[];
extern const char kRtAppendTail[];

enum class RtEvent : uint8_t {
  None,           // 不关心的事件
  Ready,          // session.updated：配置生效，可以开始说了
  SpeechStarted,  // 服务器听到有人开始说话
  SpeechStopped,  // 一句话说完了，服务器开始想
  Heard,          // 识别出主人说的话，见 heard()
  Response,       // 开始新的回答
  Text,           // 回答的文字多了一段，见 text()
  Audio,          // 回答的语音，已经交给回调
  Done,           // 这次回答结束（包括被打断的）
  Error,          // 出错了，见 error()
};

class RtParser {
 public:
  using AudioFn = void (*)(const int16_t *pcm, size_t n, void *ctx);
  RtParser(AudioFn audio, void *ctx) : audio_(audio), ctx_(ctx) {}

  // 喂一整条服务器消息（JSON 文本）
  RtEvent feed(std::string &msg);

  const std::string &text() const { return text_; }     // 当前回答的文字
  const std::string &heard() const { return heard_; }   // 主人最近说的话
  const std::string &error() const { return error_; }   // 最近一次错误（开始新的回答时清空）
  const std::string &error_code() const { return error_code_; }
  // 刚喂的这条消息属于哪次回答（response.created、各种 delta、response.done 都带），没有时为空
  const std::string &response_id() const { return response_id_; }

 private:
  void audio(const char *b64, size_t n);
  void pcm(const uint8_t *bytes, size_t n);

  AudioFn audio_;
  void *ctx_;
  std::string text_;
  std::string heard_;
  std::string error_;
  std::string error_code_;
  std::string response_id_;
  B64Decoder b64_;
  uint8_t carry_ = 0;
  bool has_carry_ = false;
};

// 自动增益：ES7210 录出来的声音偏小，服务器断句不灵。按最近的峰值慢慢调整放大倍数（1~max_gain 倍），
// 突然变响时马上降下来，避免削顶
class Agc {
 public:
  // 一开始就放大到上限的一半，免得第一句话太小声
  explicit Agc(int max_gain) : max_gain_(max_gain), gain_(max_gain * 128) {}
  // 原地放大，返回放大前的峰值（画音量条用）
  int process(int16_t *pcm, size_t n);
  int gain_x256() const { return gain_; }

 private:
  int max_gain_;
  int env_ = 0;      // 峰值包络
  int gain_;         // 当前倍数，1/256 定点
};
