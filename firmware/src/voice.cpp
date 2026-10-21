#include "voice.h"

#include <Arduino.h>

#include "audio.h"
#include "board.h"

#ifndef HAS_MIC

bool voice_available() { return false; }
void voice_begin() {}
void voice_set_options(bool, bool) {}
void voice_tap() {}
void voice_cancel() {}
void voice_pause(bool) {}
bool voice_take_wake() { return false; }
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
#include "news.h"
#include "omni.h"
#include "realtime.h"
#include "settings.h"
#include "ws.h"

#ifdef HAS_SR
#include "esp_afe_sr_models.h"
#include "esp_wn_iface.h"
#include "model_path.h"
#endif

static constexpr int kRate = 16000;
static constexpr size_t kChunk = 1600;            // 每 100ms 上传一次麦克风的声音
static constexpr uint32_t kRing = 1 << 16;        // 要上传的声音缓冲 4 秒：喊完唤醒词接着说的话，连上服务器前先存着
static constexpr uint32_t kHandshakeMs = 8000;    // 发出升级请求后等服务器答应的最长时间
static constexpr uint32_t kReadyMs = 8000;        // 连上以后等配置生效的最长时间
static constexpr uint32_t kAnswerMs = 20000;      // 说完一句后等回答的最长时间
static constexpr uint32_t kEchoMs = 300;          // 没有回声消除时，小橘说完后过这么久才开始听，免得听到自己的尾音
static constexpr int kSilenceMs = 700;            // 停顿这么久算一句话说完
static constexpr uint32_t kBargeMs = 400;         // 小橘说话时，本地听到主人连续说这么久就打断
static constexpr uint32_t kBargeGuardMs = 700;    // 小橘刚开口的这段时间不算（攒声音 300ms，回声消除还要适应一下）
static constexpr size_t kMaxMessage = 512 * 1024;

static TaskHandle_t task;
static SemaphoreHandle_t lock;
static VoiceStatus status;
static volatile bool running = false;    // 一次聊天正在进行
static volatile bool hangup = false;     // 主人要挂断
static volatile bool interrupt = false;  // 主人要打断小橘
static volatile bool woke = false;       // 刚被唤醒词叫醒，主循环要切到聊天页
static volatile bool wake_on = true;
static volatile bool barge_on = true;
static volatile bool paused = false;    // 正在升级固件：不听唤醒词、不聊天，CPU 留给写 Flash
static volatile uint32_t session_from = 0;  // 这次聊天从缓冲里第几个采样开始上传

// 要上传的声音（PSRAM）：采集任务（或回声消除后的输出）写，聊天任务读。计数只增不减，相减就是缓冲里的采样数
static int16_t *ring;
static volatile uint32_t ring_w = 0;
static uint32_t ring_r = 0;
static volatile uint8_t level = 0;

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

static void ring_push(const int16_t *pcm, size_t n) {
  uint32_t w = ring_w;
  int peak = 0;
  for (size_t i = 0; i < n; i++) {
    ring[(w + i) & (kRing - 1)] = pcm[i];
    int a = pcm[i] < 0 ? -pcm[i] : pcm[i];
    if (a > peak) peak = a;
  }
  ring_w = w + n;
  level = peak * 100 / 8000 > 100 ? 100 : peak * 100 / 8000;
}

// ---------- 本地唤醒词和回声消除（乐鑫 esp-sr 的 AFE）----------
// 麦克风和喇叭的参考信号交给 AFE：它先做回声消除，再在结果里找唤醒词。输出的干净声音拿去上传，
// 所以小橘说话时也能一直听，主人开口就能打断它。没有 esp-sr（或者内存不够建不起来）时退回直接上传麦克风的声音

#ifdef HAS_SR
static const esp_afe_sr_iface_t *afe_handle = &ESP_AFE_SR_HANDLE;
static esp_afe_sr_data_t *afe;
static int16_t *feed_buf;  // 交错的 [麦克风, 参考信号]
static int feed_chunk = 0;
static int feed_fill = 0;
static bool wakenet_on = true;
static volatile uint32_t speech_since = 0;  // 回声消除后的声音里从这时起一直有人说话，0 表示现在没人说话
#else
static void *afe = nullptr;
#endif
static Agc raw_agc(8);  // 没有 AFE 时自己放大

static void on_wake();

// 回声消除后的声音里，主人已经连续说了多久
static uint32_t local_speech_ms(uint32_t now) {
#ifdef HAS_SR
  uint32_t since = speech_since;
  return since && (int32_t)(now - since) > 0 ? now - since : 0;  // 另一个任务刚记下的时间可能比 now 晚
#else
  return 0;
#endif
}

// 采集任务里调用：每次 256 个采样
static void on_capture(const int16_t *mic, const int16_t *ref, size_t n, void *) {
  if (paused) {
#ifdef HAS_SR
    feed_fill = 0;
#endif
    return;
  }
#ifdef HAS_SR
  if (afe) {
    if (!wake_on && !running) {
      feed_fill = 0;  // 不听唤醒词也没在聊天，省点 CPU
      return;
    }
    for (size_t i = 0; i < n; i++) {
      feed_buf[2 * feed_fill] = mic[i];
      feed_buf[2 * feed_fill + 1] = ref[i];
      if (++feed_fill == feed_chunk) {
        afe_handle->feed(afe, feed_buf);
        feed_fill = 0;
      }
    }
    return;
  }
#endif
  if (!running) return;
  static int16_t buf[256];
  if (n > 256) n = 256;
  memcpy(buf, mic, n * sizeof(int16_t));
  raw_agc.process(buf, n);
  ring_push(buf, n);
}

#ifdef HAS_SR
// 取 AFE 处理后的声音：发现唤醒词就叫醒，聊天时放进上传缓冲
static void fetch_task(void *) {
  for (;;) {
    bool want_wn = wake_on;
    if (want_wn != wakenet_on) {
      wakenet_on = want_wn;
      if (want_wn) {
        afe_handle->enable_wakenet(afe);
      } else {
        afe_handle->disable_wakenet(afe);
      }
    }
    afe_fetch_result_t *res = afe_handle->fetch(afe);  // 没有喂数据时会一直等着
    if (!res || res->ret_value == ESP_FAIL) continue;
    if (res->wakeup_state == WAKENET_DETECTED) on_wake();
    if (res->vad_state != AFE_VAD_SPEECH) {
      speech_since = 0;
    } else if (!speech_since) {
      speech_since = millis() | 1;
    }
    if (running && res->data && res->data_size > 0) ring_push(res->data, res->data_size / sizeof(int16_t));
  }
}

static bool afe_begin() {
  // AFE_CONFIG_DEFAULT() 用了 C 的嵌套指定初始化，C++ 编不过，这里逐项填
  afe_config_t cfg = {};
  cfg.aec_init = true;
  cfg.voice_communication_init = false;
  cfg.voice_communication_agc_init = false;
  cfg.voice_communication_agc_gain = 15;
  cfg.vad_mode = VAD_MODE_3;
  cfg.wakenet_model_name_2 = nullptr;
  cfg.afe_ringbuf_size = 50;
  cfg.afe_linear_gain = 1.0f;
  cfg.debug_init = false;
  cfg.debug_hook[0] = {AFE_DEBUG_HOOK_MASE_TASK_IN, nullptr};
  cfg.debug_hook[1] = {AFE_DEBUG_HOOK_FETCH_TASK_IN, nullptr};
  cfg.afe_ns_mode = NS_MODE_SSP;
  cfg.afe_ns_model_name = nullptr;
  cfg.se_init = false;   // 两个麦克风已经取了平均，不做波束分离，省 CPU 和内存
  cfg.vad_init = true;  // 断句交给服务器；本地只用来在小橘说话时听出主人插话
  cfg.wakenet_init = true;
  cfg.wakenet_model_name = esp_srmodel_filter(get_static_srmodels(), ESP_WN_PREFIX, nullptr);
  cfg.wakenet_mode = DET_MODE_90;
  cfg.afe_mode = SR_MODE_LOW_COST;
  cfg.afe_perferred_core = 0;
  cfg.afe_perferred_priority = 5;
  cfg.memory_alloc_mode = AFE_MEMORY_ALLOC_MORE_PSRAM;
  cfg.agc_mode = AFE_MN_PEAK_AGC_MODE_2;  // 输出自动增益，峰值约 -6dB
  cfg.pcm_config.total_ch_num = 2;
  cfg.pcm_config.mic_num = 1;
  cfg.pcm_config.ref_num = 1;
  cfg.pcm_config.sample_rate = kRate;
  size_t before = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
  afe = afe_handle->create_from_config(&cfg);
  if (!afe) {
    log_w("voice: AFE create failed, no wake word or echo cancellation");
    return false;
  }
  feed_chunk = afe_handle->get_feed_chunksize(afe);
  feed_buf = (int16_t *)heap_caps_malloc(feed_chunk * 2 * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!feed_buf) {
    afe_handle->destroy(afe);
    afe = nullptr;
    return false;
  }
  log_i("voice: AFE ready, wake word %s, feed %d, internal RAM used %u", cfg.wakenet_model_name ? cfg.wakenet_model_name : "-",
        feed_chunk, (unsigned)(before - heap_caps_get_free_size(MALLOC_CAP_INTERNAL)));
  // 唤醒词模型推理比较吃栈
  xTaskCreatePinnedToCore(fetch_task, "voice_afe", 8192, nullptr, 5, nullptr, 0);
  return true;
}
#endif

// ---------- 和百炼的连接 ----------

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
  if (s.news_voice) {
    std::string brief = news_voice_brief();
    if (!brief.empty()) {
      p += "下面是桌面信息屏刚拉到的新闻标题，按频道分组。主人问新闻时就从这里挑着念，一次念三五条，"
           "每条用一句话说清楚，可以补一句简短的背景，但不要编造这里没有的新闻；主人没问就不要主动提：\n";
      p += brief.c_str();
    }
  }
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
  bool full_duplex = false;   // 有回声消除：小橘说话时也一直上传，主人开口就打断
  bool ready = false;
  bool closed = false;
  std::string close_reason;   // 服务器断开时说的原因
  int close_code = 0;         // 服务器断开时给的关闭码
  std::string last_error;     // 服务器最近一次报的错，断开时没给原因就显示它
  bool playing = false;       // 喇叭被这次聊天占着
  bool finishing = false;     // 回答已经收完，等喇叭放完
  bool drop_next = false;     // 还没开始的那次回答不要了（在想的时候被点了打断）
  std::string current_id;     // 正在收的回答
  std::string drop_id;        // 被打断的回答，剩下的语音和文字都不要了
  uint32_t resume_at = 0;     // 半双工时到这个时间才开始听（避开小橘自己的尾音）
  uint32_t last_activity = 0; // 最近一次有人说话或小橘说完的时间，用来判断要不要挂断
  uint32_t stopped_at = 0;    // 主人一句话说完的时间，用来算回应用了多久
  uint32_t speak_at = 0;      // 小橘这次开口（收到第一段语音）的时间
  std::string fatal;          // 非空表示要结束这次聊天

  explicit Session(RtParser::AudioFn fn) : parser(fn, this) {}

  bool dropped() const { return !drop_id.empty() && parser.response_id() == drop_id; }
};

static void to_listening(Session &s, uint32_t now, uint32_t guard_ms) {
  s.resume_at = now + guard_ms;
  s.last_activity = now;
  set_state(VoiceState::Listening);
}

static void stop_playing(Session &s) {
  s.finishing = false;
  if (!s.playing) return;
  audio_stream_end();
  s.playing = false;
}

// 不要当前这次回答了：喇叭里没放的扔掉，告诉服务器停下（回答已经收完时服务器那边没什么可停的，不发，免得它报错）
static void cut_answer(Session &s) {
  if (!s.current_id.empty()) s.drop_id = s.current_id;
  if (!s.current_id.empty() || get_state() != VoiceState::Speaking) ws_send_text(s.client, rt_event("response.cancel"));
  if (s.playing) audio_stream_flush();
  stop_playing(s);
}

// 回答的语音（24kHz），重采样成 16kHz 放进喇叭的缓冲（缓冲够大，不会卡住收数据）
static void on_audio(const int16_t *pcm, size_t n, void *ctx) {
  auto &s = *(Session *)ctx;
  if (s.dropped() || hangup) return;
  if (!s.playing) {
    s.playing = true;
    s.speak_at = millis();
    audio_stream_begin();
    uint32_t latency = s.stopped_at ? millis() - s.stopped_at : 0;
    log_i("voice: first audio %u ms after speech stopped", (unsigned)latency);
    xSemaphoreTake(lock, portMAX_DELAY);
    status.state = VoiceState::Speaking;
    if (latency) status.latency_ms = latency;
    xSemaphoreGive(lock);
  }
  int16_t out[400];
  while (n > 0) {
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
      // 有回声消除时提示音不会被听成说话，接通前存下的声音（喊完唤醒词接着说的）也照常上传
      to_listening(s, now, s.full_duplex ? 0 : 350);
      break;
    case RtEvent::SpeechStarted: {
      s.drop_next = false;  // 新的一句开始了，之后的回答都是新的
      s.last_activity = now;
      set_text(status.error, sizeof(status.error), std::string());
      VoiceState st = get_state();
      if (st == VoiceState::Speaking || st == VoiceState::Thinking) {
        log_i("voice: barge-in");
        cut_answer(s);  // 主人开口了，小橘闭嘴听
      }
      set_state(VoiceState::Hearing);
      break;
    }
    case RtEvent::SpeechStopped:
      s.stopped_at = now;
      s.last_activity = now;
      set_state(VoiceState::Thinking);
      break;
    case RtEvent::Heard:
      set_text(status.heard, sizeof(status.heard), s.parser.heard());
      break;
    case RtEvent::Response:
      s.resampler = Resampler(kRtOutRate, kRate);
      if (s.drop_next) {
        s.drop_next = false;
        s.drop_id = s.parser.response_id();
      } else {
        s.current_id = s.parser.response_id();
      }
      break;
    case RtEvent::Text:
      if (!s.dropped()) set_text(status.reply, sizeof(status.reply), s.parser.text());
      break;
    case RtEvent::Done:
      if (s.dropped() || s.parser.response_id() != s.current_id) break;  // 被打断的那次回答结束了
      s.current_id.clear();
      if (!s.parser.error().empty()) set_text(status.error, sizeof(status.error), s.parser.error());
      if (s.playing) {
        s.finishing = true;  // 等喇叭放完再回到「在听」
        audio_stream_done();
      } else if (get_state() != VoiceState::Hearing) {
        to_listening(s, now, s.full_duplex ? 0 : kEchoMs);
      }
      break;
    case RtEvent::Error:
      log_w("voice: server error %s %s", s.parser.error_code().c_str(), s.parser.error().c_str());
      s.last_error = s.parser.error();
      if (!s.ready) {
        s.fatal = s.parser.error_code() == "Arrearage" ? "百炼账户欠费了" : s.parser.error();
      } else if (s.drop_id.empty() && !s.drop_next) {
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
    if (payload.size() >= 2) s.close_code = (uint8_t)payload[0] << 8 | (uint8_t)payload[1];
    if (payload.size() > 2) s.close_reason.assign(payload, 2, 120);
    log_w("voice: server closed %d: %s", s.close_code, s.close_reason.c_str());
  } else if (opcode == kWsText) {
    // 除了语音和逐字的文字，服务器发来的事件都记到日志里，出问题时好查
    if (payload.find(".delta\"") == std::string::npos) log_i("voice: <- %.200s", payload.c_str());
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

// 把主人的话转成文字的模型，依次试：文档里写的 gummy，不行就不要文字（只是屏幕上不显示「你：…」）。
// 服务器不认的话一连上就断开（Model not exist），换下一个重连；试通的记下来，下次直接用
static const char *const kAsrModels[] = {"gummy-realtime-v1", nullptr};
static constexpr int kAsrModelCount = sizeof(kAsrModels) / sizeof(kAsrModels[0]);
static int asr_choice = 0;

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
  const char *asr = kAsrModels[asr_choice];
  log_i("voice: model %s, transcription %s", rt_model(st.ai_model.c_str()).c_str(), asr ? asr : "off");
  if (!ws_send_text(s.client, rt_session_update(st.ai_voice.c_str(), prompt.c_str(), kSilenceMs, asr))) {
    net_report(NetService::Voice, -2, s.client, "发送配置失败");
    return "网络不好，连接断了";
  }
  return "";
}

// 上传一块声音（100ms）
static bool send_chunk(Session &s) {
  static int16_t pcm[kChunk];
  static char b64[(kChunk * 2 + 2) / 3 * 4];
  for (size_t i = 0; i < kChunk; i++) pcm[i] = ring[(ring_r + i) & (kRing - 1)];
  ring_r += kChunk;
  xSemaphoreTake(lock, portMAX_DELAY);
  status.level = level;
  xSemaphoreGive(lock);
  size_t n = b64_encode((const uint8_t *)pcm, kChunk * 2, b64);
  const char *parts[] = {kRtAppendHead, b64, kRtAppendTail};
  size_t sizes[] = {strlen(kRtAppendHead), n, strlen(kRtAppendTail)};
  return ws_send(s.client, kWsText, parts, sizes, 3);
}

// 还没配置好就因为「模型不存在」被拒：换下一个识别模型，返回 true 表示该重连
static bool model_rejected(const Session &s, const std::string &reason) {
  if (s.ready || reason.find("odel") == std::string::npos || asr_choice + 1 >= kAsrModelCount) return false;
  asr_choice++;
  log_w("voice: server rejected a model (%s), retrying with transcription %s", reason.c_str(),
        kAsrModels[asr_choice] ? kAsrModels[asr_choice] : "off");
  return true;
}

// 一次聊天：连上以后一直听、一直回答，直到挂断。返回结束原因，主人自己挂断时返回空字符串；
// idle 为 true 表示是没人说话自动挂断的；retry 为 true 表示换了识别模型，该马上重连
static std::string converse(bool &idle, bool &retry) {
  auto *sp = new Session(on_audio);
  Session &s = *sp;
  s.full_duplex = afe != nullptr && barge_on;
  WsReader reader(kMaxMessage);
  std::string why = connect_session(s, reader);
  uint32_t start = millis();
  while (why.empty() && !hangup) {
    uint32_t now = millis();
    if (!s.fatal.empty()) {
      retry = model_rejected(s, s.fatal);
      why = s.fatal;
      net_report(NetService::Voice, kNetBadData, why.c_str());
      break;
    }
    if (s.closed || (!s.client.connected() && s.client.available() <= 0)) {
      retry = model_rejected(s, s.close_reason);
      // 把服务器说的原因显示出来，方便查：断开原因、断开前最后一次报错、关闭码；服务器没打招呼就是网络断了
      if (!s.close_reason.empty()) {
        why = "百炼：" + s.close_reason;
      } else if (s.closed && !s.last_error.empty()) {
        why = "百炼：" + s.last_error;
      } else if (s.closed) {
        why = "百炼断开了连接（" + std::to_string(s.close_code) + "）";
      } else {
        why = "和百炼的连接断了";
      }
      log_w("voice: disconnected %u ms after connecting, state %d: %s", (unsigned)(now - start), (int)get_state(),
            why.c_str());
      // 服务器没打招呼就断了时，附上 TLS 层的错误
      net_report(NetService::Voice, -5, s.client, s.closed ? why.c_str() : nullptr);
      break;
    }
    VoiceState st = get_state();
    if (interrupt) {
      interrupt = false;
      if (st == VoiceState::Speaking) {
        cut_answer(s);
        to_listening(s, now, s.full_duplex ? 0 : kEchoMs);
      } else if (st == VoiceState::Thinking) {
        if (s.current_id.empty()) s.drop_next = true;  // 回答还没开始，来了也不要
        cut_answer(s);
        to_listening(s, now, s.full_duplex ? 0 : kEchoMs);
      }
      st = get_state();
    }
    // 小橘说话时回声消除后主人的声音变小，服务器不一定听得出来是在插话；本地听到主人连续说话就先闭嘴，
    // 说的话照常上传，服务器会当成新的一句来回答
    if (s.full_duplex && st == VoiceState::Speaking && now - s.speak_at >= kBargeGuardMs &&
        local_speech_ms(now) >= kBargeMs) {
      log_i("voice: local barge-in");
      cut_answer(s);
      to_listening(s, now, 0);
      st = get_state();
    }
    if (s.finishing && audio_stream_pending() == 0) {
      stop_playing(s);  // 等 DMA 里最后一点放完
      if (get_state() == VoiceState::Speaking) to_listening(s, millis(), s.full_duplex ? 0 : kEchoMs);
      st = get_state();
    }

    // 上传声音。有回声消除时一直传（小橘说话时主人开口也能被听到）；没有时只在「在听」的时候传
    uint32_t w = ring_w;
    bool listening = s.ready && (s.full_duplex || ((st == VoiceState::Listening || st == VoiceState::Hearing) &&
                                                   (int32_t)(now - s.resume_at) >= 0));
    if (!s.ready) {
      // 还没连上：接通前的声音先存着（最多 4 秒），从开始聊天那一刻算起
      if (w - session_from > kRing - kChunk) session_from = w - (kRing - kChunk);
      ring_r = session_from;
    } else if (!listening) {
      ring_r = w;
    } else if (w - ring_r > kRing) {
      ring_r = w - kRing;  // 卡太久，最老的已经被覆盖了
    }
    bool sent = false;
    if (listening && w - ring_r >= kChunk) {
      if (!send_chunk(s)) {
        why = "网络不好，连接断了";
        net_report(NetService::Voice, -2, s.client, "上传声音失败");
        break;
      }
      sent = true;
    }

    size_t got = pump(s, reader, 8192);
    // 收到的事件会把 last_activity、stopped_at 设成现在，比循环开头的 now 晚，不更新的话减出来是负数（无符号就是很大），
    // 一接通就当成 20 秒没说话挂断
    now = millis();

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
  if (s.playing) audio_stream_flush();
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
    std::string why;
    for (int attempt = 0; attempt < kAsrModelCount && !hangup; attempt++) {
      bool retry = false;
      why = converse(idle, retry);
      if (!retry) break;
    }
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

// 开始一次聊天（点按钮或者喊唤醒词）。已经在聊就返回 false
static bool start_session() {
  xSemaphoreTake(lock, portMAX_DELAY);
  VoiceState st = status.state;
  bool start = (st == VoiceState::Idle || st == VoiceState::Error) && !running;
  if (start) {
    running = true;
    hangup = false;
    interrupt = false;
    session_from = ring_w;
    status.state = VoiceState::Connecting;
    status.level = 0;
    status.error[0] = 0;
    status.heard[0] = 0;
    status.reply[0] = 0;
  }
  xSemaphoreGive(lock);
  if (start) xTaskNotifyGive(task);
  return start;
}

// 听到唤醒词：没在聊就开始；小橘在想或在说就打断它
static void on_wake() {
  log_i("voice: wake word");
  if (start_session()) {
    woke = true;
    return;
  }
  VoiceState st = get_state();
  if (st == VoiceState::Thinking || st == VoiceState::Speaking) interrupt = true;
  woke = true;
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
#ifdef HAS_SR
  afe_begin();
#endif
  // TLS 和 JSON 解析比较吃栈
  xTaskCreatePinnedToCore(voice_task, "voice", 16384, nullptr, 3, &task, 1);
  audio_set_capture_sink(on_capture, nullptr);
}

void voice_set_options(bool wake, bool barge) {
  wake_on = wake;
  barge_on = barge;
}

void voice_tap() {
  if (!task) return;
  if (start_session()) return;
  VoiceState st = get_state();
  if (st == VoiceState::Thinking || st == VoiceState::Speaking) {
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

void voice_pause(bool on) {
  paused = on;
  if (on) voice_cancel();
}

bool voice_take_wake() {
  if (!woke) return false;
  woke = false;
  return true;
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
  s.wake_word = afe != nullptr && wake_on;
  s.full_duplex = afe != nullptr && barge_on;
  return s;
}

#endif
