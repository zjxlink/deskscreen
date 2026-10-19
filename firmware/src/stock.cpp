#include "stock.h"

#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>

#include "net.h"
#include "settings.h"

static constexpr uint32_t kRetryMs = 60 * 1000;

static TaskHandle_t task_handle;
static SemaphoreHandle_t lock;
static StockData latest;
static bool fresh = false;

// 失败时 error 为 HTTP 状态码或 HTTPClient 的负数错误码，返回内容不对时为 -100
static StockStatus fetch(const StockCode *codes, int n, StockData &out, int &error) {
  error = -100;
  char secids[512];
  if (!stock_build_secids(codes, n, secids, sizeof(secids))) return StockStatus::Error;
  String url =
      "https://push2.eastmoney.com/api/qt/ulist.np/get?fltt=2&invt=2"
      "&fields=f2,f3,f4,f12,f13,f14&ut=bd1d9ddb04089700cf9c27f6f7426281&secids=";
  url += secids;

  HttpsLock https;
  // 行情是公开数据，不带任何凭据，先跳过证书校验
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.setTimeout(10000);
  http.useHTTP10(true);
  if (!http.begin(client, url)) {
    error = HTTPC_ERROR_CONNECTION_REFUSED;
    return StockStatus::Error;
  }
  int status = http.GET();
  if (status != HTTP_CODE_OK) {
    net_log_failure("stock", status, client);
    error = status;
    http.end();
    return StockStatus::Error;
  }

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, http.getStream());
  http.end();
  if (err) {
    log_w("stock json: %s", err.c_str());
    return StockStatus::Error;
  }
  return stock_parse(doc, codes, n, out);
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
      int error;
      StockStatus s = fetch(codes, n, tmp, error);
      struct tm now;
      bool has_time = getLocalTime(&now, 0);
      if (s == StockStatus::Ok) {
        d = tmp;
        d.status = s;
        if (has_time) snprintf(d.updated, sizeof(d.updated), "%02d:%02d", now.tm_hour, now.tm_min);
        wait_ms = has_time ? stock_refresh_ms(now) : 30 * 1000;
      } else {
        // 失败时保留上次的行情，只有从没成功过才显示错误
        if (d.status != StockStatus::Ok) {
          d.status = s;
          d.error = error;
        }
        wait_ms = kRetryMs;
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
