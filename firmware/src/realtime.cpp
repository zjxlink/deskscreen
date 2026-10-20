#include "realtime.h"

#include <ArduinoJson.h>
#include <string.h>

const char kRtAppendHead[] = "{\"type\":\"input_audio_buffer.append\",\"audio\":\"";
const char kRtAppendTail[] = "\"}";

std::string rt_model(const char *model) {
  std::string m = model && model[0] ? model : "qwen3-omni-flash";
  if (m.find("realtime") == std::string::npos) m += "-realtime";
  return m;
}

std::string rt_session_update(const char *voice, const char *instructions, int silence_ms) {
  JsonDocument doc;
  doc["type"] = "session.update";
  JsonObject s = doc["session"].to<JsonObject>();
  s["modalities"].add("text");
  s["modalities"].add("audio");
  s["voice"] = voice;
  s["instructions"] = instructions;
  s["input_audio_format"] = "pcm16";
  s["output_audio_format"] = "pcm24";
  s["input_audio_transcription"]["model"] = nullptr;  // 服务器默认的识别模型，用来显示主人说了什么
  JsonObject td = s["turn_detection"].to<JsonObject>();
  td["type"] = "server_vad";
  td["threshold"] = 0.2;
  td["prefix_padding_ms"] = 300;
  td["silence_duration_ms"] = silence_ms;
  std::string out;
  serializeJson(doc, out);
  return out;
}

std::string rt_event(const char *type) {
  JsonDocument doc;
  doc["type"] = type;
  std::string out;
  serializeJson(doc, out);
  return out;
}

// 消息里某个字符串字段的值（不处理转义），找不到返回 false
static bool find_string(const std::string &msg, const char *key, size_t &start, size_t &end) {
  std::string pat = std::string("\"") + key + "\"";
  size_t at = msg.find(pat);
  if (at == std::string::npos) return false;
  at = msg.find('"', msg.find(':', at + pat.size()) + 1);
  if (at == std::string::npos) return false;
  start = at + 1;
  end = msg.find('"', start);
  return end != std::string::npos;
}

RtEvent RtParser::feed(std::string &msg) {
  // 语音数据最大，几十 KB，不走 JSON 解析，直接在原文里解 base64
  size_t ts, te;
  if (find_string(msg, "type", ts, te) && msg.compare(ts, te - ts, "response.audio.delta") == 0) {
    size_t ds, de;
    if (find_string(msg, "response_id", ds, de)) {
      response_id_.assign(msg, ds, de - ds);
    } else {
      response_id_.clear();
    }
    if (!find_string(msg, "delta", ds, de)) return RtEvent::None;
    b64_.reset();
    audio(msg.data() + ds, de - ds);
    return RtEvent::Audio;
  }

  JsonDocument doc;
  if (deserializeJson(doc, msg)) return RtEvent::None;
  std::string type = doc["type"] | "";
  const char *rid = doc["response_id"] | (const char *)nullptr;
  if (!rid) rid = doc["response"]["id"] | "";
  response_id_ = rid;
  if (type == "session.updated") return RtEvent::Ready;
  if (type == "input_audio_buffer.speech_started") return RtEvent::SpeechStarted;
  if (type == "input_audio_buffer.speech_stopped") return RtEvent::SpeechStopped;
  if (type == "response.created") {
    text_.clear();
    error_.clear();
    error_code_.clear();
    has_carry_ = false;
    return RtEvent::Response;
  }
  if (type == "response.audio_transcript.delta" || type == "response.text.delta") {
    const char *d = doc["delta"] | "";
    if (!d[0]) return RtEvent::None;
    text_ += d;
    return RtEvent::Text;
  }
  if (type == "conversation.item.input_audio_transcription.completed") {
    heard_ = doc["transcript"] | "";
    return RtEvent::Heard;
  }
  if (type == "response.done") {
    // 被取消、或者服务器出错时 status 不是 completed
    const char *st = doc["response"]["status"] | "completed";
    if (strcmp(st, "failed") == 0) {
      JsonVariant e = doc["response"]["status_details"]["error"];
      error_ = e["message"] | "回答失败了";
      error_code_ = e["code"] | "";
    }
    return RtEvent::Done;
  }
  if (type == "error") {
    error_ = doc["error"]["message"] | "服务器返回错误";
    error_code_ = doc["error"]["code"] | "";
    return RtEvent::Error;
  }
  return RtEvent::None;
}

void RtParser::audio(const char *b64, size_t n) {
  uint8_t buf[768 + 4];
  while (n > 0) {
    size_t take = n < 1024 ? n : 1024;
    size_t got = b64_.feed(b64, take, buf);
    b64 += take;
    n -= take;
    pcm(buf, got);
  }
}

void RtParser::pcm(const uint8_t *bytes, size_t n) {
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

int Agc::process(int16_t *pcm, size_t n) {
  int peak = 0;
  for (size_t i = 0; i < n; i++) {
    int a = pcm[i] < 0 ? -pcm[i] : pcm[i];
    if (a > peak) peak = a;
  }
  // 包络：变响马上跟上，变小慢慢回落（每块 100ms 约 2%）。下限让安静时最多放大到 max_gain 倍
  int floor = 20000 / max_gain_;
  env_ = peak > env_ ? peak : env_ - env_ / 50;
  int env = env_ > floor ? env_ : floor;
  int target = 20000 * 256 / env;
  if (target < 256) target = 256;
  // 往小调要快（免得削顶），往大调要慢（免得把说话间隙的底噪突然放大）
  if (target < gain_) {
    gain_ = target;
  } else if (target > gain_) {
    int step = (target - gain_) / 8;
    gain_ += step > 0 ? step : 1;
  }
  for (size_t i = 0; i < n; i++) {
    int v = pcm[i] * gain_ / 256;
    pcm[i] = v > 32767 ? 32767 : v < -32768 ? -32768 : v;
  }
  return peak;
}
