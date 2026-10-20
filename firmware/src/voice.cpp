#include "voice.h"

#include <Arduino.h>

#include "audio.h"
#include "board.h"

#ifndef HAS_MIC

bool voice_available() { return false; }
void voice_begin() {}
void voice_tap() {}
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
#include <esp_random.h>
#include <string.h>
#include <time.h>

#include "dashscope_ca.h"
#include "net.h"
#include "omni.h"
#include "realtime.h"
#include "settings.h"
#include "ws.h"

static constexpr int kRate = 16000;
static constexpr size_t kChunk = 1600;            // 每 100ms 上传一次麦克风的声音
static constexpr size_t kRing = 16000;            // 麦克风缓冲 1 秒，联网卡一下也不丢声音
static constexpr uint32_t kHandshakeMs = 8000;    // 发出升级请求后等服务器答应的最长时间
static constexpr uint32_t kReadyMs = 8000;        // 连上以后等配置生效的最长时间
static constexpr uint32_t kAnswerMs = 20000;      // 说完一句后等回答的最长时间
static constexpr uint32_t kEchoMs = 300;          // 小橘说完后过这么久才开始听，免得听到自己的尾音
static constexpr uint32_t kCueMs = 350;           // 开始提示音的长度，期间不听
static constexpr int kSilenceMs = 700;            // 停顿这么久算一句话说完
static constexpr size_t kMaxMessage = 512 * 1024;

static TaskHandle_t task;
static TaskHandle_t mic_task_handle;
static SemaphoreHandle_t lock;
static VoiceStatus status;
static volatile bool running = false;    // 一次聊天正在进行
static volatile bool hangup = false;     // 主人要挂断
static volatile bool interrupt = false;  // 主人要打断小橘
static volatile bool mic_want = false;

// 麦克风环形缓冲（PSRAM）：录音任务写，聊天任务读。两个计数只增不减，相减就是缓冲里的采样数
static int16_t *ring;
static volatile uint32_t ring_w = 0;
static uint32_t ring_r = 0;

// 截断时不能把一个汉字切成两半。屏幕字库里没有全角波浪号，换成普通的
static void copy_utf8(char *dst, size_t size, std::string src) {
  for (size_t at; (at = src.find("\xEF\xBD\x9E")) != std::string::npos;) src.replace(at, 3, "~");
  size_t n = src.size() < size - 1 ? src.size() : size - 1;
  while (n > 0 && n < src.size() && ((uint8_t)src[n] & 0xC0) == 0x80) n--;
  memcpy(dst, src.data(), n);
  dst[n] = 0;
}

static void set_state(VoiceState st) {
  xSemaphoreTake(lock, portMAX_DELAY);
  status.state = st;
  xSemaphoreGive(lock);
}

static VoiceState get_state() {
  xSemaphoreTake(lock, portMAX_DELAY);
  VoiceState st = status.state;
  xSemaphoreGive(lock);
  return st;
}

static void set_text(char *dst, size_t size, const std::string &text) {
  xSemaphoreTake(lock, portMAX_DELAY);
  copy_utf8(dst, size, text);
  xSemaphoreGive(lock);
}

// 一直读麦克风放进环形缓冲。i2s 的 DMA 缓冲只有 96ms，聊天任务在收发网络数据时顾不上读
static void mic_task(void *) {
  static int16_t buf[256];
  bool on = false;
  for (;;) {
    if (mic_want != on) {
      on = mic_want;
      if (on) {
        audio_mic_begin();
      } else {
        audio_mic_end();
      }
    }
    if (!on) {
      ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(200));
      continue;
    }
    size_t got = audio_mic_read(buf, 256);
    uint32_t w = ring_w;
    for (size_t i = 0; i < got; i++) ring[(w + i) % kRing] = buf[i];
    ring_w = w + got;
  }
}

static String system_prompt() {
  Settings s = settings_get();
  String p =
      "你是小橘，一只住在桌面信息屏里的像素橘猫，性格活泼、有点调皮，说话亲切。"
      "你在和主人连续语音聊天，像打电话一样一来一回：用口语回答，简短自然，一般不超过 60 个字，"
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

static bool write_all(WiFiClientSecure &c, const uint8_t *data, size_t n) {
  while (n > 0) {
    size_t w = c.write(data, n);
    if (w == 0) return false;
    data += w;
    n -= w;
  }
  return true;
}

// 发一帧 WebSocket 消息，负载由几段拼成（省得为了拼接再复制一遍几 KB 的音频）
static bool ws_send(WiFiClientSecure &c, uint8_t opcode, const char *const parts[], const size_t sizes[], int count) {
  uint64_t len = 0;
  for (int i = 0; i < count; i++) len += sizes[i];
  uint8_t mask[4];
  uint32_t r = esp_random();
  memcpy(mask, &r, 4);
  uint8_t head[14];
  size_t hn = ws_frame_header(opcode, len, mask, head);
  if (!write_all(c, head, hn)) return false;
  static uint8_t buf[1460];
  uint64_t off = 0;
  for (int i = 0; i < count; i++) {
    for (size_t at = 0; at < sizes[i];) {
      size_t take = sizes[i] - at < sizeof(buf) ? sizes[i] - at : sizeof(buf);
      memcpy(buf, parts[i] + at, take);
      ws_mask(buf, take, mask, off);
      if (!write_all(c, buf, take)) return false;
      at += take;
      off += take;
    }
  }
  return true;
}

static bool ws_send_text(WiFiClientSecure &c, const std::string &s) {
  const char *parts[] = {s.data()};
  size_t sizes[] = {s.size()};
  return ws_send(c, kWsText, parts, sizes, 1);
}

struct Session {
  WiFiClientSecure client;
  RtParser parser;
  Resampler resampler{kRtOutRate, kRate};
  bool ready = false;
  bool closed = false;
  bool playing = false;
  bool dropping = false;      // 被打断的那次回答，剩下的语音和文字都不要了
  uint32_t resume_at = 0;     // 到这个时间才开始听（避开提示音和小橘自己的尾音）
  uint32_t last_activity = 0; // 最近一次有人说话或小橘说完的时间，用来判断要不要挂断
  uint32_t stopped_at = 0;    // 主人一句话说完的时间，用来算回应用了多久
  std::string fatal;          // 非空表示要结束这次聊天

  explicit Session(RtParser::AudioFn fn) : parser(fn, this) {}
};

static void to_listening(Session &s, uint32_t now, uint32_t guard_ms) {
  s.resume_at = now + guard_ms;
  s.last_activity = now;
  set_state(VoiceState::Listening);
}

static void stop_playing(Session &s) {
  if (!s.playing) return;
  audio_stream_end();
  s.playing = false;
}

// 回答的语音（24kHz），重采样成 16kHz 边收边播。播放会阻塞到声音送进 I2S 为止
static void on_audio(const int16_t *pcm, size_t n, void *ctx) {
  auto &s = *(Session *)ctx;
  if (s.dropping || hangup || interrupt) return;
  if (!s.playing) {
    s.playing = true;
    audio_stream_begin();
    uint32_t latency = s.stopped_at ? millis() - s.stopped_at : 0;
    log_i("voice: first audio %u ms after speech stopped", (unsigned)latency);
    xSemaphoreTake(lock, portMAX_DELAY);
    status.state = VoiceState::Speaking;
    if (latency) status.latency_ms = latency;
    xSemaphoreGive(lock);
  }
  int16_t out[400];
  while (n > 0 && !hangup && !interrupt) {
    size_t take = n < 480 ? n : 480;  // 480 个 24kHz 采样变成 320 个 16kHz 采样
    size_t got = s.resampler.feed(pcm, take, out);
    audio_stream_write(out, got);
    pcm += take;
    n -= take;
  }
}

static void on_event(Session &s, RtEvent ev) {
  uint32_t now = millis();
  switch (ev) {
    case RtEvent::Ready:
      if (s.ready) break;
      s.ready = true;
      audio_play(SoundId::ChatOn);
      to_listening(s, now, kCueMs);
      break;
    case RtEvent::SpeechStarted:
      s.dropping = false;  // 新的一句开始了，之后的回答都是新的
      s.last_activity = now;
      set_text(status.error, sizeof(status.error), std::string());
      set_state(VoiceState::Hearing);
      break;
    case RtEvent::SpeechStopped:
      s.stopped_at = now;
      s.last_activity = now;
      set_state(VoiceState::Thinking);
      break;
    case RtEvent::Heard:
      set_text(status.heard, sizeof(status.heard), s.parser.heard());
      break;
    case RtEvent::Text:
      if (!s.dropping) set_text(status.reply, sizeof(status.reply), s.parser.text());
      break;
    case RtEvent::Done:
      stop_playing(s);
      if (s.dropping) {
        s.dropping = false;  // 被打断的回答到此结束，已经在听了
        break;
      }
      if (!s.parser.error().empty()) set_text(status.error, sizeof(status.error), s.parser.error());
      to_listening(s, now, kEchoMs);
      break;
    case RtEvent::Error:
      log_w("voice: server error %s %s", s.parser.error_code().c_str(), s.parser.error().c_str());
      if (!s.ready) {
        s.fatal = s.parser.error_code() == "Arrearage" ? "百炼账户欠费了" : s.parser.error();
      } else if (!s.dropping) {
        // 连上以后的错误（比如打断时已经没有回答可取消）不挂断，严重的错误服务器会自己断开连接
        set_text(status.error, sizeof(status.error), s.parser.error());
      }
      break;
    default:
      break;
  }
}

static void on_message(uint8_t opcode, std::string &payload, void *ctx) {
  auto &s = *(Session *)ctx;
  if (opcode == kWsPing) {
    const char *parts[] = {payload.data()};
    size_t sizes[] = {payload.size()};
    ws_send(s.client, kWsPong, parts, sizes, 1);
  } else if (opcode == kWsClose) {
    s.closed = true;
    if (payload.size() > 2) log_w("voice: server closed: %s", payload.c_str() + 2);
  } else if (opcode == kWsText) {
    on_event(s, s.parser.feed(payload));
  }
}

// 读收到的数据，一次最多处理 limit 字节，免得长时间顾不上发麦克风的声音
static size_t pump(Session &s, WsReader &reader, size_t limit) {
  static uint8_t buf[2048];
  size_t total = 0;
  while (total < limit && !s.fatal.size()) {
    int avail = s.client.available();
    if (avail <= 0) break;
    int n = s.client.read(buf, avail < (int)sizeof(buf) ? avail : sizeof(buf));
    if (n <= 0) break;
    total += n;
    if (!reader.feed(buf, n, on_message, &s)) s.fatal = "服务器返回的数据不对";
  }
  return total;
}

// 连上百炼并完成 WebSocket 握手。返回出错原因，成功返回空字符串
static std::string connect_session(Session &s, WsReader &reader) {
  Settings st = settings_get();
  if (st.ai_key.length() == 0) return "先在设置网页填上阿里云百炼的 API Key";
  if (!WiFi.isConnected()) return "还没连上 WiFi";

  uint32_t t0 = millis();
  {
    HttpsLock https;  // 只在握手时排队，握手最占内存；连上以后其他服务照常联网
    s.client.setCACert(DASHSCOPE_CA);
    s.client.setHandshakeTimeout(kTlsHandshakeTimeoutS);
    bool ok = s.client.connect(kOmniHost, 443);
    if (!ok && !hangup) {
      // 偶尔第一次握手失败（网络抖一下），马上再试一次
      s.client.stop();
      delay(300);
      ok = s.client.connect(kOmniHost, 443);
    }
    if (!ok) {
      char err[96] = "";
      s.client.lastError(err, sizeof(err));
      net_report(NetService::Voice, -1, s.client);
      if (strstr(err, "X509") || strstr(err, "erif")) return "证书校验失败，连不上百炼服务器";
      return "连不上百炼服务器";
    }
  }
  uint32_t t1 = millis();

  uint8_t raw[16];
  for (int i = 0; i < 16; i += 4) {
    uint32_t r = esp_random();
    memcpy(raw + i, &r, 4);
  }
  char key[32];
  key[b64_encode(raw, 16, key)] = 0;
  std::string path = std::string(kRtPath) + rt_model(st.ai_model.c_str());
  std::string extra = std::string("Authorization: Bearer ") + st.ai_key.c_str() +
                      "\r\nUser-Agent: DeskScreen/" FW_VERSION "\r\n";
  std::string req = ws_handshake(kOmniHost, path.c_str(), key, extra);
  if (!write_all(s.client, (const uint8_t *)req.data(), req.size())) {
    net_report(NetService::Voice, -2, s.client, "发送握手失败");
    return "连不上百炼服务器";
  }

  HttpHead http;
  std::string body;
  static char buf[1024];
  uint32_t last = millis();
  while (!hangup) {
    int avail = s.client.available();
    if (avail <= 0) {
      if (!s.client.connected()) break;
      if (http.done() && http.status() != 101 && millis() - last > 1000) break;  // 错误说明读得差不多了
      if (millis() - last > kHandshakeMs) {
        net_report(NetService::Voice, -3, "等握手超时");
        return "百炼服务器没有回应";
      }
      delay(5);
      continue;
    }
    int n = s.client.read((uint8_t *)buf, avail < (int)sizeof(buf) ? avail : sizeof(buf));
    if (n <= 0) continue;
    last = millis();
    size_t pos = 0;
    if (!http.done()) {
      size_t used = 0;
      if (!http.feed(buf, n, used)) continue;
      pos = used;
    }
    if (http.status() == 101) {
      // 响应头后面紧跟着的可能已经是第一条消息
      if (pos < (size_t)n && !reader.feed((const uint8_t *)buf + pos, n - pos, on_message, &s)) {
        return "服务器返回的数据不对";
      }
      break;
    }
    if (body.size() < 2048) body.append(buf + pos, n - pos);
  }
  if (hangup) return "";
  if (http.status() != 101) {
    std::string msg = http.status() ? omni_error_message(http.status(), body) : "百炼服务器没有回应";
    net_report(NetService::Voice, http.status() ? http.status() : -4, msg.c_str());
    return msg;
  }
  log_i("voice: tls %u ms, websocket %u ms", (unsigned)(t1 - t0), (unsigned)(millis() - t1));

  String prompt = system_prompt();
  if (!ws_send_text(s.client, rt_session_update(st.ai_voice.c_str(), prompt.c_str(), kSilenceMs))) {
    net_report(NetService::Voice, -2, s.client, "发送配置失败");
    return "网络不好，连接断了";
  }
  return "";
}

// 把一块麦克风的声音（100ms）放大后上传
static bool send_chunk(Session &s, Agc &agc) {
  static int16_t pcm[kChunk];
  static char b64[(kChunk * 2 + 2) / 3 * 4];
  for (size_t i = 0; i < kChunk; i++) pcm[i] = ring[(ring_r + i) % kRing];
  ring_r += kChunk;
  int peak = agc.process(pcm, kChunk);
  xSemaphoreTake(lock, portMAX_DELAY);
  status.level = peak * 100 / 8000 > 100 ? 100 : peak * 100 / 8000;
  xSemaphoreGive(lock);
  size_t n = b64_encode((const uint8_t *)pcm, kChunk * 2, b64);
  const char *parts[] = {kRtAppendHead, b64, kRtAppendTail};
  size_t sizes[] = {strlen(kRtAppendHead), n, strlen(kRtAppendTail)};
  return ws_send(s.client, kWsText, parts, sizes, 3);
}

// 一次聊天：连上以后一直听、一直回答，直到挂断。返回结束原因，主人自己挂断时返回空字符串；
// idle 为 true 表示是没人说话自动挂断的
static std::string converse(bool &idle) {
  auto *sp = new Session(on_audio);
  Session &s = *sp;
  WsReader reader(kMaxMessage);
  Agc agc(8);
  std::string why = connect_session(s, reader);
  uint32_t start = millis();
  if (why.empty() && !hangup) {
    ring_r = ring_w;
    mic_want = true;
    xTaskNotifyGive(mic_task_handle);
  }
  while (why.empty() && !hangup) {
    uint32_t now = millis();
    if (!s.fatal.empty()) {
      why = s.fatal;
      net_report(NetService::Voice, kNetBadData, why.c_str());
      break;
    }
    if (s.closed || (!s.client.connected() && s.client.available() <= 0)) {
      why = "和百炼的连接断了";
      net_report(NetService::Voice, -5, why.c_str());
      break;
    }
    VoiceState st = get_state();
    if (interrupt) {
      interrupt = false;
      if (st == VoiceState::Thinking || st == VoiceState::Speaking) {
        ws_send_text(s.client, rt_event("response.cancel"));
        s.dropping = true;
        stop_playing(s);
        to_listening(s, now, kEchoMs);
        st = VoiceState::Listening;
      }
    }

    // 上传麦克风的声音；没在听的时候缓冲里的都扔掉
    bool listening = s.ready && (st == VoiceState::Listening || st == VoiceState::Hearing) &&
                     (int32_t)(now - s.resume_at) >= 0;
    uint32_t w = ring_w;
    if (!listening) {
      ring_r = w;
    } else if (w - ring_r > kRing) {
      ring_r = w - kRing;  // 卡太久，最老的已经被覆盖了
    }
    bool sent = false;
    if (listening && w - ring_r >= kChunk) {
      if (!send_chunk(s, agc)) {
        why = "网络不好，连接断了";
        net_report(NetService::Voice, -2, s.client, "上传声音失败");
        break;
      }
      sent = true;
    }

    size_t got = pump(s, reader, 8192);

    if (!s.ready && now - start > kReadyMs) {
      why = "百炼服务器没有回应";
      net_report(NetService::Voice, -3, "等配置生效超时");
      break;
    }
    st = get_state();
    if (st == VoiceState::Listening) {
      uint32_t quiet = now - s.last_activity;
      xSemaphoreTake(lock, portMAX_DELAY);
      status.idle_left_ms = quiet < kVoiceIdleHangupMs ? kVoiceIdleHangupMs - quiet : 0;
      xSemaphoreGive(lock);
      if (quiet > kVoiceIdleHangupMs) {
        idle = true;
        why = "好久没听到声音，先挂了喵";
        break;
      }
    } else if (st == VoiceState::Thinking && now - s.stopped_at > kAnswerMs) {
      set_text(status.error, sizeof(status.error), std::string("这句没等到回答，再说一遍吧"));
      net_report(NetService::Voice, -3, "等回答超时");
      to_listening(s, now, 0);
    }
    if (!sent && got == 0) delay(5);
  }
  mic_want = false;
  stop_playing(s);
  if (s.client.connected()) {
    static const char kBye[] = "\x03\xE8";  // 1000 正常关闭
    const char *parts[] = {kBye};
    size_t sizes[] = {2};
    ws_send(s.client, kWsClose, parts, sizes, 1);
  }
  s.client.stop();
  delete sp;
  if (why.empty() || idle) net_report(NetService::Voice, 200);
  return why;
}

static void voice_task(void *) {
  for (;;) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    bool idle = false;
    std::string why = hangup ? "" : converse(idle);
    xSemaphoreTake(lock, portMAX_DELAY);
    if (why.empty() || idle) {
      status.state = VoiceState::Idle;
      copy_utf8(status.error, sizeof(status.error), why);
    } else {
      status.state = VoiceState::Error;
      copy_utf8(status.error, sizeof(status.error), why);
    }
    status.level = 0;
    status.idle_left_ms = 0;
    xSemaphoreGive(lock);
    if (why.empty() || idle) audio_play(SoundId::ChatOff);
    running = false;
  }
}

bool voice_available() { return task != nullptr; }

void voice_begin() {
  if (!audio_available() || !audio_mic_available()) return;
  ring = (int16_t *)heap_caps_malloc(kRing * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!ring) {
    log_w("voice: no memory for microphone buffer");
    return;
  }
  lock = xSemaphoreCreateMutex();
  status.state = VoiceState::Idle;
  xTaskCreatePinnedToCore(mic_task, "voice_mic", 3072, nullptr, 4, &mic_task_handle, 1);
  // TLS 和 JSON 解析比较吃栈
  xTaskCreatePinnedToCore(voice_task, "voice", 16384, nullptr, 3, &task, 1);
}

void voice_tap() {
  if (!task) return;
  xSemaphoreTake(lock, portMAX_DELAY);
  VoiceState st = status.state;
  bool start = (st == VoiceState::Idle || st == VoiceState::Error) && !running;
  if (start) {
    running = true;
    hangup = false;
    interrupt = false;
    status.state = VoiceState::Connecting;
    status.level = 0;
    status.error[0] = 0;
    status.heard[0] = 0;
    status.reply[0] = 0;
  }
  xSemaphoreGive(lock);
  if (start) {
    xTaskNotifyGive(task);
  } else if (st == VoiceState::Thinking || st == VoiceState::Speaking) {
    interrupt = true;
  } else if (st == VoiceState::Connecting || st == VoiceState::Listening || st == VoiceState::Hearing) {
    hangup = true;
  }
}

void voice_cancel() {
  if (!task) return;
  xSemaphoreTake(lock, portMAX_DELAY);
  if (status.state == VoiceState::Error) {
    status.state = VoiceState::Idle;
    status.error[0] = 0;
  }
  xSemaphoreGive(lock);
  if (running) hangup = true;
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
