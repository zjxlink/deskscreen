#include "friends.h"

#include <Arduino.h>
#include <HTTPClient.h>
#include <LovyanGFX.hpp>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <esp_heap_caps.h>

#include "net.h"
#include "root_ca.h"
#include "settings.h"

static constexpr uint32_t kFastMs = 3000;
static constexpr uint32_t kNormalMs = 6000;
static constexpr uint32_t kSlowMs = 30000;
static constexpr uint32_t kRetryMaxMs = 5 * 60 * 1000;
static constexpr uint32_t kInviteShowMs = 10 * 60 * 1000;
static constexpr size_t kPhotoMax = 400 * 1024;
static constexpr int kThumbSlots = 4;
static constexpr int kMaxActions = 8;
static constexpr int kInboxMax = 16;

enum class ActKind : uint8_t { Poke, Text, Read, Invite };
struct Action {
  ActKind kind;
  char circle[kFrIdLen];
  char to[kFrIdLen];
  char text[kFrTextLen];
};

static int job = -1;
static SemaphoreHandle_t lock;
static volatile FrPace pace = FrPace::Normal;
static volatile bool here = false;
static FrStatus *status;  // PSRAM
static uint32_t status_ver = 0, status_seen = 0;
static FrEvent *inbox;    // 收到还没交给主循环的消息，PSRAM
static int inbox_n = 0;
static Action actions[kMaxActions];
static int action_n = 0;
static FrSync *sync_buf;  // PSRAM
static uint16_t *thumbs[kThumbSlots];
static uint32_t thumb_gens[kThumbSlots];
static int thumb_next = 0;
static uint32_t thumb_gen = 0;

// 同步用的状态，只在联网任务里用
static uint32_t seq = 0;          // 已经收到的最后一条
static bool have_seq = false;     // 开机后第一次同步只拿序号，不把以前的消息再弹一遍
static int failures = 0;
static String token;
static String base;               // 服务器地址，去掉了结尾的 /
static String synced_name;        // 服务器上的名字，和设置里的不一样时改名

// 连接复用：板子每几秒同步一次，每次都 TLS 握手太费时间（服务器和反向代理都支持 keep-alive）
static WiFiClientSecure tls;
static WiFiClient plain;
static HTTPClient http;
static String conn_base;

struct PsramAllocator : ArduinoJson::Allocator {
  void *allocate(size_t n) override { return heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT); }
  void deallocate(void *p) override { heap_caps_free(p); }
  void *reallocate(void *p, size_t n) override { return heap_caps_realloc(p, n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT); }
};
static PsramAllocator psram_alloc;

static void *psram(size_t n) { return heap_caps_calloc(1, n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT); }

static void set_error(const char *msg) {
  xSemaphoreTake(lock, portMAX_DELAY);
  if (strcmp(status->error, msg ? msg : "") != 0 || status->synced != !msg) {
    friends_copy(status->error, sizeof(status->error), msg ? msg : "");
    status->synced = !msg;
    status_ver++;
  }
  xSemaphoreGive(lock);
}

static void set_note(const char *msg) {
  xSemaphoreTake(lock, portMAX_DELAY);
  friends_copy(status->note, sizeof(status->note), msg);
  status->note_seq++;
  status_ver++;
  xSemaphoreGive(lock);
}

// 服务器回的 {"error": "..."}，没有就用状态码
static void error_text(int code, const String &body, char *out, size_t size) {
  JsonDocument doc;
  if (code > 0 && !deserializeJson(doc, body) && doc["error"].is<const char *>()) {
    friends_copy(out, size, doc["error"]);
  } else if (code < 0) {
    snprintf(out, size, "连不上服务器（%s）", HTTPClient::errorToString(code).c_str());
  } else {
    snprintf(out, size, "服务器返回 %d", code);
  }
}

// 发一个请求，返回 HTTP 状态码或 HTTPClient 的负数错误码。body 为 nullptr 时用 GET
static int request(const String &path, const char *body, String &resp) {
  HttpsLock https;
  bool secure = base.startsWith("https://");
  if (conn_base != base) {
    http.end();
    tls.stop();
    plain.stop();
    conn_base = base;
  }
  String url = base + path;
  bool ok = secure ? http.begin(tls, url) : http.begin(plain, url);
  if (!ok) return HTTPC_ERROR_CONNECTION_REFUSED;
  http.setReuse(true);
  http.setTimeout(8000);
  if (token.length()) http.addHeader("Authorization", "Bearer " + token);
  int code;
  if (body) {
    http.addHeader("Content-Type", "application/json");
    code = http.POST((uint8_t *)body, strlen(body));
  } else {
    code = http.GET();
  }
  resp = code > 0 ? http.getString() : String();
  if (code < 0) {
    // 连接坏了（服务器重启、反向代理断开空闲连接）：下次重新连
    http.end();
    tls.stop();
    plain.stop();
  } else {
    http.end();  // setReuse 时不断开连接
  }
  if (secure) net_report(NetService::Friends, code, tls);
  else net_report(NetService::Friends, code);
  return code;
}

static int post_json(const char *path, JsonDocument &doc, String &resp) {
  String body;
  serializeJson(doc, body);
  return request(path, body.c_str(), resp);
}

// 第一次连上服务器：注册，拿到 token 存起来
static bool do_register(const Settings &s) {
  JsonDocument doc;
  doc["name"] = s.fr_name;
  doc["kind"] = "board";
  String resp;
  token = "";
  int code = post_json("/api/register", doc, resp);
  char err[96];
  if (code != 200) {
    error_text(code, resp, err, sizeof(err));
    set_error(err);
    return false;
  }
  JsonDocument r;
  if (deserializeJson(r, resp) || !r["token"].is<const char *>()) {
    set_error("服务器返回的内容不对，地址是不是填错了？");
    return false;
  }
  token = r["token"].as<const char *>();
  settings_save_fr_token(base, token);
  synced_name = s.fr_name;
  have_seq = false;
  seq = 0;
  log_i("friends: registered as %s", r["id"].as<const char *>());
  return true;
}

// 下载留言里的照片，缩成缩略图放进一格。失败返回 -1（留言照样显示，只是没有照片）
static int fetch_thumb(const char *path, uint32_t &gen) {
  HttpsLock https;
  bool secure = base.startsWith("https://");
  String url = base + path;
  if (!(secure ? http.begin(tls, url) : http.begin(plain, url))) return -1;
  http.setReuse(true);
  http.setTimeout(10000);
  int code = http.GET();
  int len = http.getSize();
  uint8_t *jpg = nullptr;
  size_t got = 0;
  if (code == 200 && len > 0 && (size_t)len <= kPhotoMax) {
    jpg = (uint8_t *)psram(len);
    WiFiClient *stream = http.getStreamPtr();
    uint32_t start = millis();
    while (jpg && got < (size_t)len && millis() - start < 10000) {
      int n = stream->read(jpg + got, len - got);
      if (n > 0) got += n;
      else delay(5);
    }
  }
  http.end();
  if (!jpg || got != (size_t)len) {
    free(jpg);
    if (code < 0) {
      tls.stop();
      plain.stop();
    }
    return -1;
  }
  int slot = thumb_next;
  if (!thumbs[slot]) thumbs[slot] = (uint16_t *)psram(kFrThumbW * kFrThumbH * 2);
  bool ok = false;
  if (thumbs[slot]) {
    LGFX_Sprite spr;
    spr.setPsram(true);
    spr.setColorDepth(16);
    if (spr.createSprite(kFrThumbW, kFrThumbH)) {
      spr.fillScreen(TFT_BLACK);
      ok = spr.drawJpg(jpg, got, 0, 0, kFrThumbW, kFrThumbH, 0, 0, 0.0f, 0.0f, lgfx::datum_t::middle_center);
      // 精灵里是大端的 RGB565，LVGL 要小端
      const uint16_t *src = (const uint16_t *)spr.getBuffer();
      for (int i = 0; ok && i < kFrThumbW * kFrThumbH; i++) thumbs[slot][i] = (src[i] >> 8) | (src[i] << 8);
      spr.deleteSprite();
    }
  }
  free(jpg);
  if (!ok) return -1;
  thumb_next = (thumb_next + 1) % kThumbSlots;
  gen = ++thumb_gen;
  thumb_gens[slot] = gen;
  return slot;
}

// 发出排队的操作。失败的不重试，在好友页底部说一声
static void run_actions() {
  for (;;) {
    xSemaphoreTake(lock, portMAX_DELAY);
    if (action_n == 0) {
      xSemaphoreGive(lock);
      return;
    }
    Action a = actions[0];
    memmove(actions, actions + 1, sizeof(Action) * (action_n - 1));
    action_n--;
    xSemaphoreGive(lock);

    JsonDocument doc;
    const char *path = "/api/send";
    switch (a.kind) {
      case ActKind::Poke:
        doc["circle"] = a.circle;
        doc["to"] = a.to;
        doc["kind"] = "poke";
        break;
      case ActKind::Text:
        doc["circle"] = a.circle;
        doc["to"] = a.to;
        doc["kind"] = "text";
        doc["text"] = a.text;
        break;
      case ActKind::Read:
        path = "/api/read";
        doc["ids"].add(a.text);
        break;
      case ActKind::Invite:
        path = "/api/invite";
        doc["circle"] = a.circle;
        break;
    }
    String resp;
    int code = post_json(path, doc, resp);
    char msg[96];
    if (code != 200) {
      if (a.kind == ActKind::Read) continue;  // 已读没发出去无所谓
      char err[80];
      error_text(code, resp, err, sizeof(err));
      snprintf(msg, sizeof(msg), "没发出去：%s", err);
      set_note(msg);
      continue;
    }
    if (a.kind == ActKind::Invite) {
      JsonDocument r;
      if (deserializeJson(r, resp) || !r["code"].is<const char *>()) {
        set_note("没拿到邀请码，再试一次");
        continue;
      }
      xSemaphoreTake(lock, portMAX_DELAY);
      friends_copy(status->invite_code, sizeof(status->invite_code), r["code"]);
      friends_copy(status->invite_circle, sizeof(status->invite_circle), r["circle"] | "");
      status->invite_until = millis() + kInviteShowMs;
      status_ver++;
      xSemaphoreGive(lock);
    } else if (a.kind != ActKind::Read) {
      set_note(a.kind == ActKind::Poke ? "戳过去了" : "发出去了");
    }
  }
}

static void sync_once() {
  char path[64];
  snprintf(path, sizeof(path), "/api/sync?since=%lu&here=%d", have_seq ? (unsigned long)seq : 0UL, here ? 1 : 0);
  String resp;
  int code = request(path, nullptr, resp);
  char err[96];
  if (code == 401) {
    // 服务器上没有这个设备了（数据被清掉、换了服务器）：下一轮重新注册
    token = "";
    settings_save_fr_token("", "");
    set_error("服务器不认这台设备了，正在重新注册");
    return;
  }
  if (code != 200) {
    error_text(code, resp, err, sizeof(err));
    set_error(err);
    failures++;
    return;
  }
  JsonDocument doc(&psram_alloc);
  if (deserializeJson(doc, resp) || !friends_parse_sync(doc, *sync_buf)) {
    net_report(NetService::Friends, kNetBadData, "sync");
    set_error("服务器返回的内容不对");
    failures++;
    return;
  }
  failures = 0;
  FrSync &s = *sync_buf;
  bool first = !have_seq;
  have_seq = true;
  if (s.seq < seq) seq = 0;  // 服务器的数据被重置过
  bool got = false;
  // 收到的消息：照片先下载好再交给主循环
  for (int i = 0; i < s.nevents && !first; i++) {
    FrEvent &e = s.events[i];
    if (e.seq <= seq || e.mine) continue;
    if (e.type == FrEventType::Other) continue;
    if (e.type == FrEventType::Text && e.photo[0]) e.thumb = fetch_thumb(e.photo, e.thumb_gen);
    xSemaphoreTake(lock, portMAX_DELAY);
    if (inbox_n == kInboxMax) {
      memmove(inbox, inbox + 1, sizeof(FrEvent) * (kInboxMax - 1));
      inbox_n--;
    }
    inbox[inbox_n++] = e;
    xSemaphoreGive(lock);
    got = true;
  }
  seq = s.seq;
  // 序号是整个服务器共用的，别人的消息也会让它变；只在收到自己的消息时记下来，免得老写闪存
  if (got || first) settings_save_fr_seq(seq);
  synced_name = s.me_name;

  xSemaphoreTake(lock, portMAX_DELAY);
  bool changed = status->ncircles != s.ncircles ||
                 memcmp(status->circles, s.circles, sizeof(FrCircle) * s.ncircles) != 0 ||
                 strcmp(status->me_name, s.me_name) != 0 || !status->synced || status->error[0];
  if (changed) {
    status->ncircles = s.ncircles;
    memcpy(status->circles, s.circles, sizeof(s.circles));
    friends_copy(status->me_name, sizeof(status->me_name), s.me_name);
    status->synced = true;
    status->error[0] = '\0';
    status_ver++;
  }
  if (status->invite_code[0] && (int32_t)(millis() - status->invite_until) >= 0) {
    status->invite_code[0] = '\0';
    status_ver++;
  }
  xSemaphoreGive(lock);
}

static uint32_t friends_step() {
  Settings s = settings_get();
  char url[160];
  friends_clean_url(s.fr_url.c_str(), url, sizeof(url));
  base = url;
  xSemaphoreTake(lock, portMAX_DELAY);
  bool was = status->configured;
  status->configured = base.length() > 0;
  if (status->configured != was) status_ver++;
  if (!status->configured) {
    status->ncircles = 0;
    status->registered = false;
    action_n = 0;
  }
  xSemaphoreGive(lock);
  if (!base.length()) return kNetJobIdle;
  if (!WiFi.isConnected()) return 5000;

  // token 是哪个服务器给的，换了服务器要重新注册
  String tok_base;
  token = settings_load_fr_token(tok_base);
  if (tok_base != base) token = "";
  static bool loaded = false;
  if (!loaded && token.length()) {
    loaded = true;
    seq = settings_load_fr_seq();  // 关机期间收到的消息，开机后照样弹出来
    have_seq = seq > 0;
  }
  if (!token.length() && !do_register(s)) {
    failures++;
    return net_retry_ms(failures, kRetryMaxMs);
  }
  xSemaphoreTake(lock, portMAX_DELAY);
  if (!status->registered) {
    status->registered = true;
    status_ver++;
  }
  xSemaphoreGive(lock);

  if (synced_name.length() && s.fr_name.length() && synced_name != s.fr_name) {
    JsonDocument doc;
    doc["name"] = s.fr_name;
    String resp;
    if (post_json("/api/rename", doc, resp) == 200) synced_name = s.fr_name;
  }
  run_actions();
  sync_once();
  if (failures) return net_retry_ms(failures, kRetryMaxMs);
  switch (pace) {
    case FrPace::Fast: return kFastMs;
    case FrPace::Slow: return kSlowMs;
    default: return kNormalMs;
  }
}

void friends_begin() {
  lock = xSemaphoreCreateMutex();
  status = (FrStatus *)psram(sizeof(FrStatus));
  inbox = (FrEvent *)psram(sizeof(FrEvent) * kInboxMax);
  sync_buf = (FrSync *)psram(sizeof(FrSync));
  if (!status || !inbox || !sync_buf) return;
  tls.setCACert(ROOT_CA_BUNDLE);
  tls.setHandshakeTimeout(kTlsHandshakeTimeoutS);
  job = net_job_add("friends", friends_step, 8000);
}

void friends_kick() {
  // 改了名字或服务器：下一轮按新设置来
  net_job_kick(job);
}

void friends_set_pace(FrPace p) {
  if (p == pace) return;
  bool faster = p == FrPace::Fast && pace != FrPace::Fast;
  pace = p;
  if (faster) net_job_kick(job);  // 刚打开好友页，马上刷新一下谁在桌前
}

void friends_set_here(bool h) {
  if (h == here) return;
  here = h;
  net_job_kick(job);  // 别人的屏幕马上更新
}

bool friends_take_status(FrStatus &out) {
  if (!status) return false;
  xSemaphoreTake(lock, portMAX_DELAY);
  bool changed = status_ver != status_seen;
  if (changed) {
    out = *status;
    status_seen = status_ver;
  }
  xSemaphoreGive(lock);
  return changed;
}

int friends_take_events(FrEvent *out, int max) {
  if (!inbox) return 0;
  xSemaphoreTake(lock, portMAX_DELAY);
  int n = inbox_n < max ? inbox_n : max;
  memcpy(out, inbox, sizeof(FrEvent) * n);
  memmove(inbox, inbox + n, sizeof(FrEvent) * (inbox_n - n));
  inbox_n -= n;
  xSemaphoreGive(lock);
  return n;
}

const uint16_t *friends_thumb(int slot, uint32_t gen) {
  if (slot < 0 || slot >= kThumbSlots || thumb_gens[slot] != gen) return nullptr;
  return thumbs[slot];
}

static void queue(ActKind kind, const char *circle, const char *to, const char *text) {
  if (!status) return;
  xSemaphoreTake(lock, portMAX_DELAY);
  if (status->configured && action_n < kMaxActions) {
    Action &a = actions[action_n++];
    a.kind = kind;
    friends_copy(a.circle, sizeof(a.circle), circle);
    friends_copy(a.to, sizeof(a.to), to);
    friends_copy(a.text, sizeof(a.text), text);
  }
  xSemaphoreGive(lock);
  net_job_kick(job);
}

void friends_poke(const char *circle, const char *to) { queue(ActKind::Poke, circle, to, ""); }
void friends_send_text(const char *circle, const char *to, const char *text) { queue(ActKind::Text, circle, to, text); }
void friends_mark_read(const char *msg_id) { queue(ActKind::Read, "", "", msg_id); }
void friends_invite(const char *circle) { queue(ActKind::Invite, circle, "", ""); }

std::string friends_manage_url() {
  Settings s = settings_get();
  char url[160];
  friends_clean_url(s.fr_url.c_str(), url, sizeof(url));
  String tok_base;
  String tok = settings_load_fr_token(tok_base);
  if (!url[0] || !tok.length() || tok_base != url) return "";
  return std::string(url) + "/#t=" + tok.c_str();
}

std::string friends_brief() {
  if (!status) return "未开启";
  xSemaphoreTake(lock, portMAX_DELAY);
  char out[128];
  if (!status->configured) snprintf(out, sizeof(out), "未开启");
  else if (status->error[0]) snprintf(out, sizeof(out), "连不上：%s", status->error);
  else if (!status->registered || !status->synced) snprintf(out, sizeof(out), "正在连接…");
  else snprintf(out, sizeof(out), "● 已连接 · %d 个圈子", status->ncircles);
  xSemaphoreGive(lock);
  return out;
}
