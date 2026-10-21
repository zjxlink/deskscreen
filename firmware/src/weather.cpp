#include "weather.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>

#include "net.h"
#include "settings.h"

static constexpr uint32_t kRefreshMs = 15 * 60 * 1000;
static constexpr uint32_t kRetryMs = 60 * 1000;

static int job = -1;
static SemaphoreHandle_t lock;
static WeatherData latest;
static bool fresh = false;

// 成功返回 0；失败返回 HTTP 状态码、HTTPClient 的负数错误码，或者 kNetBadData（返回内容不对）
static int fetch(WeatherData &out) {
  Settings s = settings_get();
  String url = "https://api.open-meteo.com/v1/forecast?latitude=" + String(s.lat, 4) +
               "&longitude=" + String(s.lon, 4) + kWeatherQuery;

  HttpsLock https;
  // 天气数据不敏感，先跳过证书校验；以后可换成内置根证书
  WiFiClientSecure client;
  client.setInsecure();
  client.setHandshakeTimeout(kTlsHandshakeTimeoutS);
  HTTPClient http;
  http.setTimeout(10000);
  if (!http.begin(client, url)) return HTTPC_ERROR_CONNECTION_REFUSED;

  int status = http.GET();
  if (status != HTTP_CODE_OK) {
    net_report(NetService::Weather, status, client);
    http.end();
    return status;
  }
  String body = http.getString();
  http.end();

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, body);
  if (err) {
    net_report(NetService::Weather, kNetBadData, client, (String("JSON: ") + err.c_str()).c_str());
    return kNetBadData;
  }
  if (!weather_parse(doc, out)) {
    net_report(NetService::Weather, kNetBadData, client, "missing current/daily");
    return kNetBadData;
  }
  net_report(NetService::Weather, HTTP_CODE_OK, client);
  return 0;
}

static void publish(const WeatherData &d) {
  xSemaphoreTake(lock, portMAX_DELAY);
  latest = d;
  fresh = true;
  xSemaphoreGive(lock);
}

static uint32_t weather_step() {
  static bool ever_ok = false;
  static int failures = 0;
  if (!WiFi.isConnected()) return 5000;
  WeatherData d = {};
  d.error = fetch(d);
  if (d.error == 0) {
    ever_ok = true;
    failures = 0;
    publish(d);
    return kRefreshMs;
  }
  // 失败时保留上次的天气，只有从没成功过才在屏幕上显示错误
  if (!ever_ok) publish(d);
  return net_retry_ms(++failures, kRetryMs);
}

void weather_begin() {
  lock = xSemaphoreCreateMutex();
  job = net_job_add("weather", weather_step);
}

void weather_request_refresh() {
  net_job_kick(job);
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
