#include "weather.h"

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>

#include "net.h"
#include "settings.h"

static constexpr uint32_t kRefreshMs = 15 * 60 * 1000;
static constexpr uint32_t kRetryMs = 60 * 1000;

static TaskHandle_t task_handle;
static SemaphoreHandle_t lock;
static WeatherData latest;
static bool fresh = false;

// 成功返回 0；失败返回 HTTP 状态码、HTTPClient 的负数错误码，或者 kErrBadData（返回内容不对）
static constexpr int kErrBadData = -100;

static int fetch(WeatherData &out) {
  Settings s = settings_get();
  String url = "https://api.open-meteo.com/v1/forecast?latitude=" + String(s.lat, 4) +
               "&longitude=" + String(s.lon, 4) +
               "&current=temperature_2m,relative_humidity_2m,weather_code"
               "&daily=temperature_2m_max,temperature_2m_min"
               "&timezone=auto&forecast_days=1";

  HttpsLock https;
  // 天气数据不敏感，先跳过证书校验；以后可换成内置根证书
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.setTimeout(10000);
  if (!http.begin(client, url)) return HTTPC_ERROR_CONNECTION_REFUSED;

  int status = http.GET();
  if (status != HTTP_CODE_OK) {
    net_log_failure("weather", status, client);
    http.end();
    return status;
  }
  String body = http.getString();
  http.end();

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, body);
  if (err) {
    log_w("weather json: %s", err.c_str());
    return kErrBadData;
  }
  JsonObject cur = doc["current"];
  JsonObject daily = doc["daily"];
  if (cur.isNull() || daily.isNull()) return kErrBadData;

  out.temp = cur["temperature_2m"] | 0.0f;
  out.humidity = cur["relative_humidity_2m"] | 0;
  out.code = cur["weather_code"] | 0;
  out.temp_max = daily["temperature_2m_max"][0] | 0.0f;
  out.temp_min = daily["temperature_2m_min"][0] | 0.0f;
  out.error = 0;
  return 0;
}

static void publish(const WeatherData &d) {
  xSemaphoreTake(lock, portMAX_DELAY);
  latest = d;
  fresh = true;
  xSemaphoreGive(lock);
}

static void weather_task(void *) {
  bool ever_ok = false;
  for (;;) {
    uint32_t wait_ms = 5000;
    if (WiFi.isConnected()) {
      WeatherData d = {};
      d.error = fetch(d);
      if (d.error == 0) {
        ever_ok = true;
        publish(d);
        wait_ms = kRefreshMs;
      } else {
        // 失败时保留上次的天气，只有从没成功过才在屏幕上显示错误
        if (!ever_ok) publish(d);
        wait_ms = kRetryMs;
      }
    }
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(wait_ms));
  }
}

void weather_begin() {
  lock = xSemaphoreCreateMutex();
  // TLS 握手比较吃栈，给足 16KB
  xTaskCreate(weather_task, "weather", 16384, nullptr, 1, &task_handle);
}

void weather_request_refresh() {
  if (task_handle) xTaskNotifyGive(task_handle);
}

bool weather_take_update(WeatherData &out) {
  xSemaphoreTake(lock, portMAX_DELAY);
  bool got = fresh;
  if (got) {
    out = latest;
    fresh = false;
  }
  xSemaphoreGive(lock);
  return got;
}
