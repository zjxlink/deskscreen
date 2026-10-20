#include "voice.h"

#include <Arduino.h>

#include "audio.h"
#include "board.h"

#ifndef HAS_MIC

bool voice_available() { return false; }
void voice_begin() {}
void voice_press() {}
void voice_release() {}
void voice_cancel() {}
VoiceStatus voice_status() {
  VoiceStatus s = {};
  s.state = VoiceState::Off;
  return s;
}

#else

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <esp_heap_caps.h>
#include <string.h>
#include <time.h>

#include "dashscope_ca.h"
#include "net.h"
#include "omni.h"
#include "settings.h"

static constexpr int kRate = 16000;
static constexpr size_t kMaxSamples = kRate * kVoiceMaxRecordMs / 1000;
static constexpr uint32_t kFirstByteMs = 30000;  // 发完录音后等回答的最长时间
static constexpr uint32_t kIdleMs = 15000;       // 回答中途断流多久算失败

static TaskHandle_t task;
static SemaphoreHandle_t lock;
static VoiceStatus status;
static uint8_t *wav;  // 44 字节 WAV 头 + 录音，放在 PSRAM
static volatile bool releasing = false;
static volatile bool cancelling = false;

static void set_state(VoiceState st, const char *error = nullptr) {
  xSemaphoreTake(lock, portMAX_DELAY);
  status.state = st;
  if (error) strlcpy(status.error, error, sizeof(status.error));
  xSemaphoreGive(lock);
}

// 截断时不能把一个汉字切成两半。屏幕字库里没有全角波浪号，换成普通的
static void copy_utf8(char *dst, size_t size, std::string src) {
  for (size_t at; (at = src.find("\xEF\xBD\x9E")) != std::string::npos;) src.replace(at, 3, "~");
  size_t n = src.size() < size - 1 ? src.size() : size - 1;
  while (n > 0 && n < src.size() && ((uint8_t)src[n] & 0xC0) == 0x80) n--;
  memcpy(dst, src.data(), n);
  dst[n] = 0;
}

static size_t record() {
  int16_t *pcm = (int16_t *)(wav + 44);
  size_t n = 0;
  uint32_t start = millis();
  audio_mic_begin();
  while (!releasing && !cancelling && n < kMaxSamples) {
    size_t want = kMaxSamples - n < 256 ? kMaxSamples - n : 256;
    size_t got = audio_mic_read(pcm + n, want);
    int peak = 0;
    for (size_t i = 0; i < got; i++) {
      int a = abs(pcm[n + i]);
      if (a > peak) peak = a;
    }
    n += got;
    xSemaphoreTake(lock, portMAX_DELAY);
    status.level = peak * 100 / 8000 > 100 ? 100 : peak * 100 / 8000;
    status.rec_ms = millis() - start;
    xSemaphoreGive(lock);
  }
  audio_mic_end();
  return n;
}

static String system_prompt() {
  Settings s = settings_get();
  String p =
      "你是小橘，一只住在桌面信息屏里的像素橘猫，性格活泼、有点调皮，说话亲切。"
      "你在和主人语音聊天：用口语回答，简短自然，一般不超过 60 个字，"
      "不要用列表、表情符号、括号或星号描述动作，只说要念出来的话。主人用什么语言就用什么语言回答。";
  struct tm t;
  if (getLocalTime(&t, 0) && t.tm_year >= 2024 - 1900) {
    static const char *const days[] = {"日", "一", "二", "三", "四", "五", "六"};
    char buf[96];
    snprintf(buf, sizeof(buf), "现在是 %d年%d月%d日 星期%s %02d:%02d（北京时间）。", t.tm_year + 1900, t.tm_mon + 1,
             t.tm_mday, days[t.tm_wday], t.tm_hour, t.tm_min);
    p += buf;
  }
  if (s.city.length()) p += "主人在" + s.city + "。";
  return p;
}

struct Playback {
  Resampler resampler{kOmniOutRate, kRate};
  bool started = false;
};

static void on_audio(const int16_t *pcm, size_t n, void *ctx) {
  auto *pb = (Playback *)ctx;
  if (cancelling) return;
  if (!pb->started) {
    pb->started = true;
    audio_stream_begin();
    set_state(VoiceState::Speaking);
  }
  int16_t out[400];
  while (n > 0) {
    size_t take = n < 480 ? n : 480;  // 480 个 24kHz 采样变成 320 个 16kHz 采样
    size_t got = pb->resampler.feed(pcm, take, out);
    audio_stream_write(out, got);
    pcm += take;
    n -= take;
  }
}

static bool write_all(WiFiClientSecure &c, const uint8_t *data, size_t n) {
  while (n > 0) {
    if (cancelling) return false;
    size_t w = c.write(data, n);
    if (w == 0) return false;
    data += w;
    n -= w;
  }
  return true;
}

// 把录音发给百炼，边收边播放回答。返回出错原因，成功返回空字符串
static std::string converse(size_t samples, std::string &reply) {
  Settings s = settings_get();
  if (s.ai_key.length() == 0) return "先在设置网页填上阿里云百炼的 API Key";
  if (!WiFi.isConnected()) return "还没连上 WiFi";

  wav_header(wav, samples, kRate);
  pcm_normalize((int16_t *)(wav + 44), samples, 8);
  size_t wav_bytes = 44 + samples * 2;
  OmniRequest req = omni_request(s.ai_model.c_str(), s.ai_voice.c_str(), system_prompt().c_str());
  size_t length = req.head.size() + (wav_bytes + 2) / 3 * 4 + req.tail.size();

  HttpsLock https;
  WiFiClientSecure client;
  client.setCACert(DASHSCOPE_CA);
  client.setHandshakeTimeout(kTlsHandshakeTimeoutS);
  if (!client.connect(kOmniHost, 443)) {
    char err[96] = "";
    client.lastError(err, sizeof(err));
    net_report(NetService::Voice, -1, client);
    if (strstr(err, "X509") || strstr(err, "erif")) return "证书校验失败，连不上百炼服务器";
    return "连不上百炼服务器";
  }

  String head = String("POST ") + kOmniPath + " HTTP/1.1\r\nHost: " + kOmniHost + "\r\nAuthorization: Bearer " + s.ai_key +
                "\r\nContent-Type: application/json\r\nAccept: text/event-stream\r\nUser-Agent: DeskScreen/" FW_VERSION
                "\r\nContent-Length: " + String((unsigned long)length) + "\r\nConnection: close\r\n\r\n";
  bool ok = write_all(client, (const uint8_t *)head.c_str(), head.length()) &&
            write_all(client, (const uint8_t *)req.head.data(), req.head.size());
  static char b64[2048];
  for (size_t off = 0; ok && off < wav_bytes; off += 1536) {
    size_t take = wav_bytes - off < 1536 ? wav_bytes - off : 1536;
    size_t n = b64_encode(wav + off, take, b64);
    ok = write_all(client, (const uint8_t *)b64, n);
  }
  ok = ok && write_all(client, (const uint8_t *)req.tail.data(), req.tail.size());
  if (cancelling) return "";
  if (!ok) {
    net_report(NetService::Voice, -2, client, "发送录音失败");
    return "发送录音失败，网络不好";
  }

  HttpHead http;
  ChunkedDecoder chunked;
  Playback pb;
  OmniStream stream(on_audio, &pb);
  std::string error_body;
  static char buf[1024];
  static char body[1024];
  uint32_t last = millis();
  bool got_any = false;
  while (!cancelling && !stream.done() && !chunked.done()) {
    int avail = client.available();
    if (avail <= 0) {
      if (!client.connected()) break;
      if (millis() - last > (got_any ? kIdleMs : kFirstByteMs)) {
        if (pb.started) audio_stream_end();
        net_report(NetService::Voice, -3, "等回答超时");
        return "等了太久没有回答";
      }
      delay(5);
      continue;
    }
    int n = client.read((uint8_t *)buf, avail < (int)sizeof(buf) ? avail : sizeof(buf));
    if (n <= 0) continue;
    last = millis();
    got_any = true;
    size_t pos = 0;
    if (!http.done()) {
      size_t used = 0;
      if (!http.feed(buf, n, used)) continue;
      pos = used;
    }
    const char *data = buf + pos;
    size_t len = n - pos;
    if (http.chunked()) {
      len = chunked.feed(data, len, body);
      data = body;
    }
    if (http.status() == 200) {
      stream.feed(data, len);
    } else if (error_body.size() < 2048) {
      error_body.append(data, len);
    }
  }
  if (pb.started) audio_stream_end();
  client.stop();
  if (cancelling) return "";

  if (http.status() != 200) {
    std::string msg = http.status() ? omni_error_message(http.status(), error_body) : "百炼服务器没有回应";
    net_report(NetService::Voice, http.status() ? http.status() : -4, msg.c_str());
    return msg;
  }
  if (!stream.error().empty()) {
    net_report(NetService::Voice, kNetBadData, stream.error().c_str());
    return stream.error() == "Arrearage" ? "百炼账户欠费了" : stream.error();
  }
  reply = stream.text();
  if (reply.empty() && stream.audio_bytes() == 0) {
    net_report(NetService::Voice, kNetBadData, "没有收到回答");
    return "没有收到回答";
  }
  net_report(NetService::Voice, 200);
  return "";
}

static void voice_task(void *) {
  for (;;) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    if (cancelling) {
      set_state(VoiceState::Idle);
      continue;
    }
    size_t samples = record();
    if (cancelling) {
      set_state(VoiceState::Idle);
      continue;
    }
    if (samples < (size_t)kRate * kVoiceMinRecordMs / 1000) {
      set_state(VoiceState::Error, "按住说话，说完再松手");
      continue;
    }
    set_state(VoiceState::Thinking);
    std::string reply;
    std::string error = converse(samples, reply);
    xSemaphoreTake(lock, portMAX_DELAY);
    if (cancelling) {
      status.state = VoiceState::Idle;
    } else if (!error.empty()) {
      status.state = VoiceState::Error;
      copy_utf8(status.error, sizeof(status.error), error);
    } else {
      status.state = VoiceState::Idle;
      copy_utf8(status.reply, sizeof(status.reply), reply);
    }
    xSemaphoreGive(lock);
  }
}

bool voice_available() { return task != nullptr; }

void voice_begin() {
  if (!audio_available() || !audio_mic_available()) return;
  wav = (uint8_t *)heap_caps_malloc(44 + kMaxSamples * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!wav) {
    log_w("voice: no memory for recording");
    return;
  }
  lock = xSemaphoreCreateMutex();
  status.state = VoiceState::Idle;
  // TLS 和 JSON 解析比较吃栈
  xTaskCreatePinnedToCore(voice_task, "voice", 16384, nullptr, 3, &task, 1);
}

void voice_press() {
  if (!task) return;
  xSemaphoreTake(lock, portMAX_DELAY);
  VoiceState st = status.state;
  xSemaphoreGive(lock);
  if (st == VoiceState::Thinking || st == VoiceState::Speaking) {
    cancelling = true;  // 任务结束这一轮后回到 Idle
    return;
  }
  if (st == VoiceState::Recording) return;
  releasing = false;
  cancelling = false;
  xSemaphoreTake(lock, portMAX_DELAY);
  status.state = VoiceState::Recording;
  status.level = 0;
  status.rec_ms = 0;
  status.error[0] = 0;
  xSemaphoreGive(lock);
  xTaskNotifyGive(task);
}

void voice_release() {
  if (task) releasing = true;
}

void voice_cancel() {
  if (!task) return;
  xSemaphoreTake(lock, portMAX_DELAY);
  VoiceState st = status.state;
  if (st == VoiceState::Error) status.state = VoiceState::Idle;
  xSemaphoreGive(lock);
  if (st == VoiceState::Recording || st == VoiceState::Thinking || st == VoiceState::Speaking) cancelling = true;
}

VoiceStatus voice_status() {
  VoiceStatus s;
  if (!task) {
    s = {};
    s.state = VoiceState::Off;
    return s;
  }
  xSemaphoreTake(lock, portMAX_DELAY);
  s = status;
  xSemaphoreGive(lock);
  return s;
}

#endif
