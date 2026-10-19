#include "weather.h"

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>

#include "settings.h"

static constexpr uint32_t kRefreshMs = 15 * 60 * 1000;
static constexpr uint32_t kRetryMs = 60 * 1000;

static TaskHandle_t task_handle;
static SemaphoreHandle_t lock;
static WeatherData latest;
static bool fresh = false;

static bool fetch(WeatherData &out) {
  Settings s = settings_get();
  String url = "https://api.open-meteo.com/v1/forecast?latitude=" + String(s.lat, 4) +
               "&longitude=" + String(s.lon, 4) +
               "&current=temperature_2m,relative_humidity_2m,weather_code"
               "&daily=temperature_2m_max,temperature_2m_min"
               "&timezone=auto&forecast_days=1";

  // 天气数据不敏感，先跳过证书校验；以后可换成内置根证书
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.setTimeout(10000);
  if (!http.begin(client, url)) return false;

  int status = http.GET();
  if (status != HTTP_CODE_OK) {
    log_w("weather http %d", status);
    http.end();
    return false;
  }
  String body = http.getString();
  http.end();

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, body);
  if (err) {
    log_w("weather json: %s", err.c_str());
    return false;
  }
  JsonObject cur = doc["current"];
  JsonObject daily = doc["daily"];
  if (cur.isNull() || daily.isNull()) return false;

  out.temp = cur["temperature_2m"] | 0.0f;
  out.humidity = cur["relative_humidity_2m"] | 0;
  out.code = cur["weather_code"] | 0;
  out.temp_max = daily["temperature_2m_max"][0] | 0.0f;
  out.temp_min = daily["temperature_2m_min"][0] | 0.0f;
  return true;
}

static void weather_task(void *) {
  for (;;) {
    uint32_t wait_ms = 5000;
    if (WiFi.isConnected()) {
      WeatherData d;
      if (fetch(d)) {
        xSemaphoreTake(lock, portMAX_DELAY);
        latest = d;
        fresh = true;
        xSemaphoreGive(lock);
        wait_ms = kRefreshMs;
      } else {
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
