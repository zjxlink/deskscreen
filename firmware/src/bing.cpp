#include "bing.h"

#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <esp_heap_caps.h>
#include <time.h>

#include "bing_ca.h"
#include "net.h"
#include "photos.h"
#include "settings.h"

static constexpr const char *kHost = "cn.bing.com";
static constexpr const char *kApiPath = "/HPImageArchive.aspx?format=js&idx=0&n=8&mkt=zh-CN";
static constexpr size_t kMaxJsonBytes = 64 * 1024;    // 8 张图的信息正常只有几 KB
static constexpr size_t kMaxImageBytes = 512 * 1024;  // 320x240 的图十几 KB
static constexpr uint32_t kCheckMs = 60 * 1000;       // 多久看一次要不要拉
static constexpr uint32_t kRetryMs = 60 * 60 * 1000;  // 失败后最长的重试间隔
static constexpr uint32_t kNoClockMs = 6 * 60 * 60 * 1000;  // 还没对时就每 6 小时拉一次

// 必应的网站会拦默认的 User-Agent「ESP32HTTPClient」，按浏览器的样子发
static const char kUserAgent[] =
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0 Safari/537.36";

static TaskHandle_t task;
static volatile bool kicked = false;
static bool use_http = false;  // HTTPS 连不上（比如证书校验不过）时改用普通 HTTP

// 把响应收进 PSRAM，超过 max 就写失败，HTTPClient 会报错停下
class PsramSink : public Stream {
 public:
  explicit PsramSink(size_t max) : max_(max) {}
  ~PsramSink() { free(buf_); }
  size_t write(uint8_t c) override { return write(&c, 1); }
  size_t write(const uint8_t *data, size_t n) override {
    if (len_ + n > max_) return 0;
    if (len_ + n > cap_) {
      size_t cap = cap_ ? cap_ * 2 : 16 * 1024;
      while (cap < len_ + n) cap *= 2;
      if (cap > max_) cap = max_;
      auto *p = (uint8_t *)heap_caps_realloc(buf_, cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
      if (!p) return 0;
      buf_ = p;
      cap_ = cap;
    }
    memcpy(buf_ + len_, data, n);
    len_ += n;
    return n;
  }
  int available() override { return 0; }
  int read() override { return -1; }
  int peek() override { return -1; }
  // 交出缓冲区，之后由调用方 free
  uint8_t *take(size_t &len) {
    uint8_t *p = buf_;
    len = len_;
    buf_ = nullptr;
    len_ = cap_ = 0;
    return p;
  }

 private:
  uint8_t *buf_ = nullptr;
  size_t len_ = 0;
  size_t cap_ = 0;
  size_t max_;
};

// 下载 path 的内容。返回 HTTP 状态码或负数错误码；200 时 out 是 PSRAM 里的内容，由调用方 free
static int get_once(const char *path, size_t max, uint8_t *&out, size_t &len) {
  out = nullptr;
  len = 0;
  HttpsLock https;  // 普通 HTTP 也排队，免得和其他服务同时连网
  WiFiClientSecure secure;
  WiFiClient plain;
  WiFiClient &client = use_http ? plain : secure;
  if (!use_http) {
    secure.setCACert(BING_CA);
    secure.setHandshakeTimeout(kTlsHandshakeTimeoutS);
  }
  HTTPClient http;
  http.setTimeout(15000);
  String url = String(use_http ? "http://" : "https://") + kHost + path;
  if (!http.begin(client, url)) return HTTPC_ERROR_CONNECTION_REFUSED;
  http.setUserAgent(kUserAgent);
  int status = http.GET();
  if (status == HTTP_CODE_OK) {
    PsramSink sink(max);
    int n = http.writeToStream(&sink);
    if (n < 0) {
      status = n;
    } else {
      out = sink.take(len);
      if (!out) status = kNetBadData;  // 内容是空的
    }
  }
  http.end();
  if (status != HTTP_CODE_OK) {
    if (use_http) {
      net_report(NetService::Bing, status, "HTTP");
    } else {
      net_report(NetService::Bing, status, secure);
    }
  }
  return status;
}

static int get(const char *path, size_t max, uint8_t *&out, size_t &len) {
  int status = get_once(path, max, out, len);
  if (status < 0 && status != kNetBadData) {
    // 连不上：换另一种方式再试一次，成功了以后就一直用它
    use_http = !use_http;
    log_w("bing: retrying over %s", use_http ? "http" : "https");
    status = get_once(path, max, out, len);
    if (status != HTTP_CODE_OK) use_http = !use_http;
  }
  return status;
}

// 拉一次：先拿图片列表，再下载还没有的图。插着卡时存进卡里，没卡时只把当天这一张放内存。
// card 返回这次有没有用卡。全部成功时返回 true
static bool fetch(bool &card) {
  uint8_t *body;
  size_t len;
  if (get(kApiPath, kMaxJsonBytes, body, len) != HTTP_CODE_OK) return false;
  JsonDocument filter;
  bing_build_filter(filter);
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, (const char *)body, len, DeserializationOption::Filter(filter));
  free(body);
  if (err) {
    String detail = String("JSON: ") + err.c_str() + ", " + len + " bytes";
    net_report(NetService::Bing, kNetBadData, detail.c_str());
    return false;
  }
  static BingImage imgs[kBingApiImages];  // 放在静态区，任务栈留给 TLS
  int n = bing_parse(doc, imgs, kBingApiImages);
  if (n == 0) {
    net_report(NetService::Bing, kNetBadData, "no images");
    return false;
  }

  card = photos_bing_card();
  bool ok = true;
  for (int i = 0; i < (card ? n : 1); i++) {
    const BingImage &b = imgs[i];
    if (card ? photos_bing_known(b.date) : photos_bing_kept(b.date)) continue;
    uint8_t *img;
    size_t img_len;
    if (get(b.path, kMaxImageBytes, img, img_len) != HTTP_CODE_OK) {
      ok = false;
      continue;
    }
    log_i("bing: %s, %u bytes, %s", b.date, (unsigned)img_len, b.caption);
    if (card) {
      if (!photos_bing_save(b.date, img, img_len, b.caption)) ok = false;
      free(img);
    } else {
      photos_bing_keep(b.date, img, img_len, b.caption);  // 交给 photos.cpp，不用 free
    }
  }
  if (card) photos_bing_prune(kBingMaxImages, imgs[n - 1].date);
  if (ok) net_report(NetService::Bing, HTTP_CODE_OK);
  return ok;
}

// 到下一个零点二十几分还有多少毫秒（必应北京时间零点换图，错开一会儿免得刚好没换）
static uint32_t until_next_day() {
  struct tm t;
  if (!getLocalTime(&t, 0) || t.tm_year < 2024 - 1900) return kNoClockMs;
  int32_t now = t.tm_hour * 3600 + t.tm_min * 60 + t.tm_sec;
  int32_t target = 20 * 60 + (int32_t)(esp_random() % (20 * 60));
  int32_t wait = target - now;
  if (wait < 60) wait += 24 * 3600;
  return (uint32_t)wait * 1000;
}

static void bing_task(void *) {
  bool was_wifi = false;
  bool last_card = true;
  bool fetched = false;
  int failures = 0;
  uint32_t next_at = 0;
  for (;;) {
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(kCheckMs));
    bool want = settings_get().album_bing;
    bool wifi = WiFi.isConnected();
    uint32_t now = millis();
    bool go = false;
    if (want && wifi) {
      go = !fetched || (int32_t)(now - next_at) >= 0;
      go = go || !was_wifi;  // WiFi 刚连上
      go = go || kicked;     // 设置里刚打开
      // 上次没插卡，现在插上了：把最近几天的图都补上（失败后等重试时间到了再说）
      go = go || (fetched && !last_card && failures == 0 && photos_status().card != PhotoCard::NoCard);
    }
    was_wifi = wifi;
    kicked = false;
    if (!go) continue;

    bool card = false;
    if (fetch(card)) {
      failures = 0;
      next_at = millis() + until_next_day();
    } else {
      next_at = millis() + net_retry_ms(++failures, kRetryMs);
    }
    fetched = true;
    last_card = card;
  }
}

void bing_begin() {
  if (!photos_supported()) return;  // 没有相册的板子用不上
  xTaskCreate(bing_task, "bing", 12288, nullptr, 1, &task);
}

void bing_kick() {
  kicked = true;
  if (task) xTaskNotifyGive(task);
}
