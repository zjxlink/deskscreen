#include "stock.h"

#include <HTTPClient.h>
#include <WiFi.h>

#include "net.h"
#include "settings.h"

// 东方财富和腾讯都失败时（多半是网络问题）的最长重试间隔
static constexpr uint32_t kRetryMs = 5 * 60 * 1000;

// 默认的 User-Agent「ESP32HTTPClient」容易被网站的反爬规则拦掉，按浏览器的样子发
static const char kUserAgent[] =
    "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0 Safari/537.36";

static TaskHandle_t task_handle;
static SemaphoreHandle_t lock;
static StockData latest;
static bool fresh = false;

// 行情是公开数据，不带任何凭据，用普通 HTTP：省掉 TLS 的内存和握手时间。
// 实机上 HTTPS 握手成功后服务器不回任何内容直接断开（-5），东方财富网页版本身也走 http。
// 返回 HTTP 状态码或 HTTPClient 的负数错误码，200 时 body 是完整响应；
// 其他情况 detail 可能有给网络诊断看的说明
static int http_get(String url, const char *referer, String &body, String &detail) {
  WiFiClient client;
  HTTPClient http;
  http.setTimeout(10000);
  // HTTPClient 自己跟随跳转时会把 //host/path 当成本站路径，所以手动跟随，最多一次
  int status;
  for (int hop = 0;; hop++) {
    if (!http.begin(client, url)) return HTTPC_ERROR_CONNECTION_REFUSED;
    // end() 会清掉自定义请求头，所以每次 begin() 之后都要重新加
    http.setUserAgent(kUserAgent);
    http.addHeader("Referer", referer);
    status = http.GET();
    if (status < 300 || status >= 400 || hop >= 1) break;
    String location = http.getLocation();
    const char *prefix = stock_redirect_prefix(location.c_str(), "http");
    if (!prefix) break;
    http.end();
    // 服务器回的是 keep-alive，end() 会留着这条连接，不关掉的话下一个请求会发到原来的主机上
    client.stop();
    url = String(prefix) + location;
    log_i("stock redirected to %s", url.c_str());
  }
  if (status == HTTP_CODE_OK) {
    // 先读完再解析，原因见 github.cpp
    body = http.getString();
  } else if (status >= 300 && status < 400) {
    // 跳转没跟随（或跳了还在跳）时，把目标地址显示在网络诊断里
    detail = String("redirect to ") + http.getLocation();
  }
  http.end();
  return status;
}

// 东方财富：主数据源。失败时 error 为 HTTP 状态码、HTTPClient 的负数错误码或 kNetBadData
static StockStatus fetch_eastmoney(const StockCode *codes, int n, StockData &out, int &error) {
  error = kNetBadData;
  char secids[512];
  if (!stock_build_secids(codes, n, secids, sizeof(secids))) return StockStatus::Error;
  String url =
      "http://push2.eastmoney.com/api/qt/ulist.np/get?fltt=2&invt=2"
      "&fields=f2,f3,f4,f12,f13,f14&ut=bd1d9ddb04089700cf9c27f6f7426281&secids=";
  url += secids;

  String body, detail;
  int status = http_get(url, "https://quote.eastmoney.com/", body, detail);
  if (status != HTTP_CODE_OK) {
    net_report(NetService::Stock, status, detail.length() ? detail.c_str() : nullptr);
    error = status;
    return StockStatus::Error;
  }
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, body);
  if (err) {
    detail = String("JSON: ") + err.c_str() + ", " + body.length() + " bytes";
    net_report(NetService::Stock, kNetBadData, detail.c_str());
    return StockStatus::Error;
  }
  StockStatus s = stock_parse(doc, codes, n, out);
  net_report(NetService::Stock, s == StockStatus::Ok ? HTTP_CODE_OK : kNetBadData,
             s == StockStatus::Ok ? nullptr : "unexpected data");
  return s;
}

// 腾讯：备用数据源，东方财富失败或者冷却中时用。错误码含义同上
static StockStatus fetch_tencent(const StockCode *codes, int n, StockData &out, int &error) {
  error = kNetBadData;
  char query[256];
  if (!stock_build_tencent_query(codes, n, query, sizeof(query))) return StockStatus::Error;
  // /utf8/ 下返回 UTF-8，默认是 GBK
  String url = String("http://qt.gtimg.cn/utf8/q=") + query;

  String body, detail;
  int status = http_get(url, "https://gu.qq.com/", body, detail);
  if (status != HTTP_CODE_OK) {
    net_report(NetService::StockBackup, status, detail.length() ? detail.c_str() : nullptr);
    error = status;
    return StockStatus::Error;
  }
  StockStatus s = stock_parse_tencent(body.c_str(), codes, n, out);
  if (s != StockStatus::Ok) detail = String("unexpected data, ") + body.length() + " bytes";
  net_report(NetService::StockBackup, s == StockStatus::Ok ? HTTP_CODE_OK : kNetBadData,
             s == StockStatus::Ok ? nullptr : detail.c_str());
  return s;
}

static void publish(const StockData &d) {
  xSemaphoreTake(lock, portMAX_DELAY);
  latest = d;
  fresh = true;
  xSemaphoreGive(lock);
}

static void stock_task(void *) {
  static StockData d;
  static StockData tmp;
  int failures = 0;
  // 东方财富会按 IP 限流，被拦后还一直请求只会被拦得更久：失败后冷却一段时间，期间直接用腾讯
  int primary_failures = 0;
  uint32_t primary_failed_at = 0;
  for (;;) {
    uint32_t wait_ms = 5000;
    StockCode codes[kMaxStocks];
    int n = stock_split_codes(settings_get().stocks.c_str(), codes, kMaxStocks);
    if (n == 0) {
      d.status = StockStatus::NotSet;
      d.count = 0;
      publish(d);
      wait_ms = portMAX_DELAY;  // 等设置页面填写后被唤醒
    } else if (WiFi.isConnected()) {
      if (d.status != StockStatus::Ok) {
        d.status = StockStatus::Loading;
        publish(d);
      }
      int error = kNetBadData;
      StockStatus s = StockStatus::Error;
      bool try_primary =
          primary_failures == 0 || millis() - primary_failed_at >= stock_primary_cooldown_ms(primary_failures);
      if (try_primary) {
        s = fetch_eastmoney(codes, n, tmp, error);
        if (s == StockStatus::Ok) {
          primary_failures = 0;
        } else {
          primary_failures++;
          primary_failed_at = millis();
          log_w("eastmoney failed %d times, using tencent for %lu s", primary_failures,
                (unsigned long)(stock_primary_cooldown_ms(primary_failures) / 1000));
        }
      }
      if (s != StockStatus::Ok) {
        int backup_error;
        s = fetch_tencent(codes, n, tmp, backup_error);
        // 两个都失败时，屏幕上显示主数据源的错误码；没试主数据源时显示备用的
        if (!try_primary) error = backup_error;
      }
      struct tm now;
      bool has_time = getLocalTime(&now, 0);
      if (s == StockStatus::Ok) {
        d = tmp;
        d.status = s;
        failures = 0;
        if (has_time) snprintf(d.updated, sizeof(d.updated), "%02d:%02d", now.tm_hour, now.tm_min);
        wait_ms = has_time ? stock_refresh_ms(now) : 60 * 1000;
      } else {
        // 失败时保留上次的行情，只有从没成功过才显示错误
        if (d.status != StockStatus::Ok) {
          d.status = s;
          d.error = error;
        }
        wait_ms = net_retry_ms(++failures, kRetryMs);
      }
      publish(d);
    }
    ulTaskNotifyTake(pdTRUE, wait_ms == portMAX_DELAY ? portMAX_DELAY : pdMS_TO_TICKS(wait_ms));
  }
}

void stock_begin() {
  lock = xSemaphoreCreateMutex();
  xTaskCreate(stock_task, "stock", 12288, nullptr, 1, &task_handle);
}

void stock_request_refresh() {
  if (task_handle) xTaskNotifyGive(task_handle);
}

bool stock_take_update(StockData &out) {
  xSemaphoreTake(lock, portMAX_DELAY);
  bool got = fresh;
  if (got) {
    out = latest;
    fresh = false;
  }
  xSemaphoreGive(lock);
  return got;
}
