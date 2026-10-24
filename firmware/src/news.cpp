#include "news.h"

#include <Arduino.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <esp_heap_caps.h>

#include "net.h"
#include "settings.h"

static constexpr uint32_t kRefreshMs = 30 * 60 * 1000;
static constexpr uint32_t kRetryMs = 5 * 60 * 1000;  // 一轮里有频道失败，过 5 分钟再拉失败的那些
static constexpr uint32_t kGapMs = 1500;             // 一轮里每个频道之间歇一下，让别的服务插进来
static constexpr uint32_t kNightCheckMs = 10 * 60 * 1000;

static int job = -1;
static SemaphoreHandle_t lock;
static NewsData *latest;            // PSRAM
static NewsChannel *scratch;        // 正在拉的频道，PSRAM
static String urls[kNewsMaxChannels];  // 每个频道的地址，换设置时按它把旧新闻挪过去
static NewsKind kinds[kNewsMaxChannels];
static bool fresh = false;
static volatile bool quiet = false;
static bool reconfigure = true;
static int next_ch = 0;         // 这一轮拉到第几个频道
static bool round_failed = false;
static String cfg_key;          // 上次排频道时的来源设置，没变就不用重排
static uint32_t due_at = 0;     // 按计划下一次该跑的时间（别的设置改了也会叫醒这个任务）

// ArduinoJson 的内存也放 PSRAM：知乎热榜过滤后还有四十多 KB
struct PsramAllocator : ArduinoJson::Allocator {
  void *allocate(size_t n) override { return heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT); }
  void deallocate(void *p) override { heap_caps_free(p); }
  void *reallocate(void *p, size_t n) override { return heap_caps_realloc(p, n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT); }
};
static PsramAllocator psram_alloc;

// 按设置重新排频道：内置来源按 kNewsSources 的顺序，然后是自定义 RSS。地址没变的频道保留已经拉到的新闻
static void rebuild(NewsData &d) {
  Settings s = settings_get();
  static NewsData *old = nullptr;
  static String old_urls[kNewsMaxChannels];
  if (!old) old = (NewsData *)heap_caps_malloc(sizeof(NewsData), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  // 分不到内存就不保留旧新闻，各频道重新拉
  if (old) memcpy(old, &d, sizeof(NewsData));
  for (int i = 0; i < kNewsMaxChannels; i++) old_urls[i] = urls[i];

  memset(&d, 0, sizeof(NewsData));
  d.updated = old ? old->updated : 0;
  auto add = [&](const char *name, NewsKind kind, const String &url) {
    if (d.nch >= kNewsMaxChannels) return;
    int i = d.nch++;
    urls[i] = url;
    kinds[i] = kind;
    for (int j = 0; old && j < old->nch; j++) {
      if (old_urls[j] == url) {
        d.ch[i] = old->ch[j];
        break;
      }
    }
    if (name) snprintf(d.ch[i].name, sizeof(d.ch[i].name), "%s", name);
  };
  for (int i = 0; i < kNewsSourceCount; i++) {
    const NewsSource &src = kNewsSources[i];
    if (news_has_source(s.news_src.c_str(), src.id)) add(src.name, src.kind, src.url);
  }
  std::string rss[kNewsMaxRss];
  int n = news_split_urls(s.news_rss.c_str(), rss, kNewsMaxRss);
  for (int i = 0; i < n; i++) {
    int before = d.nch;
    add(nullptr, NewsKind::Rss, String(rss[i].c_str()));
    // 还没拉到过的 RSS 先叫「订阅 1」，拉到后换成它自己的名字
    if (d.nch > before && !d.ch[before].name[0]) snprintf(d.ch[before].name, sizeof(d.ch[before].name), "订阅 %d", i + 1);
  }
  for (int i = d.nch; i < kNewsMaxChannels; i++) urls[i] = "";
}

// 把 RSS 一段一段喂给解析器，够数了就让 HTTPClient 停止下载（写不进去它就不收了）。
// writeToStream 要的是 Stream，只用到写的那一半
class RssSink : public Stream {
 public:
  explicit RssSink(RssParser &p) : p_(p) {}
  int available() override { return 0; }
  int read() override { return -1; }
  int peek() override { return -1; }
  size_t write(uint8_t c) override { return write(&c, 1); }
  size_t write(const uint8_t *buf, size_t n) override {
    if (p_.done()) return 0;
    p_.feed((const char *)buf, n);
    return n;
  }

 private:
  RssParser &p_;
};

// 成功返回 0；失败返回 HTTP 状态码、HTTPClient 的负数错误码，或者 kNetBadData
static int fetch(const String &url, NewsKind kind, NewsChannel &ch, char *feed_title, size_t title_size) {
  bool tls = url.startsWith("https://");
  HttpsLock https;  // 普通 http 也排队，免得和别的服务抢内存
  WiFiClientSecure secure;
  WiFiClient plain;
  // 只是读新闻标题，不校验证书（自定义 RSS 什么网站都有，固件带不齐根证书）
  secure.setInsecure();
  secure.setHandshakeTimeout(kTlsHandshakeTimeoutS);
  HTTPClient http;
  http.setTimeout(10000);
  http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
  http.setUserAgent("Mozilla/5.0 (DeskScreen)");  // 百度、知乎不认不像浏览器的请求
  if (!(tls ? http.begin(secure, url) : http.begin(plain, url))) return HTTPC_ERROR_CONNECTION_REFUSED;

  int status = http.GET();
  if (status != HTTP_CODE_OK) {
    tls ? net_report(NetService::News, status, secure) : net_report(NetService::News, status);
    http.end();
    return status;
  }

  const char *bad = nullptr;
  String detail;
  if (kind == NewsKind::Rss) {
    RssParser parser(ch);
    RssSink sink(parser);
    int r = http.writeToStream(&sink);
    if (!parser.ok()) {
      detail = r < 0 ? String("RSS: ") + HTTPClient::errorToString(r) : String("RSS: no <item>");
      bad = detail.c_str();
    }
    snprintf(feed_title, title_size, "%s", parser.feed_title());
  } else {
    // 先把整个响应读完再解析（见 github.cpp 的说明）；二三十 KB，String 会放到 PSRAM
    String body = http.getString();
    JsonDocument filter;
    news_build_filter(kind, filter);
    JsonDocument doc(&psram_alloc);
    DeserializationError err = deserializeJson(doc, body, DeserializationOption::Filter(filter));
    bool ok = !err && (kind == NewsKind::Sina    ? news_parse_sina(doc, ch)
                       : kind == NewsKind::Baidu ? news_parse_baidu(doc, ch)
                                                 : news_parse_zhihu(doc, ch));
    if (!ok) {
      detail = err ? String("JSON: ") + err.c_str() + ", " + body.length() + " bytes" : String("no items");
      bad = detail.c_str();
    }
  }
  http.end();
  if (bad) {
    tls ? net_report(NetService::News, kNetBadData, secure, bad) : net_report(NetService::News, kNetBadData, bad);
    return kNetBadData;
  }
  tls ? net_report(NetService::News, HTTP_CODE_OK, secure) : net_report(NetService::News, HTTP_CODE_OK);
  return 0;
}

static uint32_t schedule(uint32_t ms) {
  due_at = millis() + ms;
  return ms;
}

static uint32_t news_step() {
  if (reconfigure) {
    reconfigure = false;
    Settings s = settings_get();
    String key = s.news_src + "|" + s.news_rss;
    if (key == cfg_key && due_at) {
      // 保存的是别的设置，资讯照原来的计划
      int32_t left = (int32_t)(due_at - millis());
      return left > 0 ? left : 0;
    }
    cfg_key = key;
    xSemaphoreTake(lock, portMAX_DELAY);
    rebuild(*latest);
    fresh = true;
    xSemaphoreGive(lock);
    next_ch = 0;
    round_failed = false;
  }
  if (latest->nch == 0) {  // 没选任何来源，等设置改了再来
    due_at = 0;
    return kNetJobIdle;
  }
  if (!WiFi.isConnected()) return schedule(5000);
  if (quiet && next_ch == 0) return schedule(kNightCheckMs);  // 夜里不拉；一轮拉到一半进了夜间就拉完这一轮

  int i = next_ch;
  String url = urls[i];
  NewsKind kind = kinds[i];
  // 失败了也保留上次的新闻，只记下原因；只在这一轮没拉到的才重拉
  bool skip = false;
  xSemaphoreTake(lock, portMAX_DELAY);
  if (round_failed && latest->ch[i].error == 0 && latest->ch[i].count > 0) skip = true;
  xSemaphoreGive(lock);

  if (!skip) {
    memset(scratch, 0, sizeof(NewsChannel));
    char feed_title[sizeof(scratch->name)] = "";
    int err = fetch(url, kind, *scratch, feed_title, sizeof(feed_title));
    xSemaphoreTake(lock, portMAX_DELAY);
    // 拉的时候设置可能改了：频道换了位置就不写回去，下一轮重新拉
    if (i < latest->nch && urls[i] == url) {
      NewsChannel &ch = latest->ch[i];
      ch.error = err;
      if (err == 0) {
        memcpy(ch.items, scratch->items, sizeof(ch.items));
        ch.count = scratch->count;
        // RSS 用它自己的名字（默认名「订阅 N」才换，IT之家这种内置的不动）
        if (kind == NewsKind::Rss && feed_title[0] && strncmp(ch.name, "订阅 ", strlen("订阅 ")) == 0) {
          snprintf(ch.name, sizeof(ch.name), "%s", feed_title);
        }
        for (int k = 0; k < ch.count; k++) {
          if (!ch.items[k].source[0]) snprintf(ch.items[k].source, sizeof(ch.items[k].source), "%s", ch.name);
        }
        time_t now = time(nullptr);
        if (now > 1700000000) latest->updated = now;
      } else {
        round_failed = true;
      }
      fresh = true;
    }
    xSemaphoreGive(lock);
  }

  if (++next_ch < latest->nch) return schedule(kGapMs);
  next_ch = 0;
  bool failed = false;
  xSemaphoreTake(lock, portMAX_DELAY);
  for (int k = 0; k < latest->nch; k++) failed |= latest->ch[k].error != 0;
  xSemaphoreGive(lock);
  round_failed = failed;
  return schedule(failed ? kRetryMs : kRefreshMs);
}

void news_begin() {
  lock = xSemaphoreCreateMutex();
  latest = (NewsData *)heap_caps_calloc(1, sizeof(NewsData), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  scratch = (NewsChannel *)heap_caps_calloc(1, sizeof(NewsChannel), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!latest || !scratch) {
    log_e("news: no PSRAM for %u bytes", (unsigned)sizeof(NewsData));
    return;
  }
  // 开机先让天气、股票这些先拉，资讯晚 20 秒
  job = net_job_add("news", news_step, 20000);
}

void news_request_refresh() {
  if (job < 0) return;
  reconfigure = true;
  net_job_kick(job);
}

void news_set_quiet(bool q) {
  bool was = quiet;
  quiet = q;
  if (was && !q) net_job_kick(job);  // 天亮了马上拉一轮
}

bool news_take_update(NewsData &out) {
  if (!latest) return false;
  xSemaphoreTake(lock, portMAX_DELAY);
  bool got = fresh;
  if (got) {
    memcpy(&out, latest, sizeof(NewsData));
    fresh = false;
  }
  xSemaphoreGive(lock);
  return got;
}

std::string news_voice_brief() {
  if (!latest) return "";
  xSemaphoreTake(lock, portMAX_DELAY);
  std::string s = news_brief(*latest, 5, 2400);
  xSemaphoreGive(lock);
  return s;
}
