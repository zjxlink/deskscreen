#include "github.h"

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>

#include "net.h"
#include "root_ca.h"
#include "settings.h"

static constexpr uint32_t kRefreshMs = 30 * 60 * 1000;
static constexpr uint32_t kRetryMs = 2 * 60 * 1000;

static TaskHandle_t task_handle;
static SemaphoreHandle_t lock;
static GithubData latest;
static bool fresh = false;

static const char kQuery[] =
    "{\"query\":\"query{viewer{login contributionsCollection{contributionCalendar{"
    "totalContributions weeks{contributionDays{contributionCount contributionLevel weekday}}}}}}\"}";

// 失败时 error 为 HTTP 状态码或 HTTPClient 的负数错误码，返回内容不对时为 -100
static GithubStatus fetch(const String &token, GithubData &out, int &error) {
  error = -100;
  HttpsLock https;
  WiFiClientSecure client;
  client.setCACert(ROOT_CA_BUNDLE);
  HTTPClient http;
  http.setTimeout(15000);
  http.useHTTP10(true);  // 避免分块传输，方便直接流式解析 JSON
  if (!http.begin(client, "https://api.github.com/graphql")) {
    error = HTTPC_ERROR_CONNECTION_REFUSED;
    return GithubStatus::Error;
  }
  http.addHeader("Authorization", "bearer " + token);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("User-Agent", "DeskScreen");

  int status = http.POST((uint8_t *)kQuery, strlen(kQuery));
  if (status == 401) {
    http.end();
    return GithubStatus::BadToken;
  }
  if (status != HTTP_CODE_OK) {
    net_log_failure("github", status, client);
    error = status;
    http.end();
    return GithubStatus::Error;
  }

  JsonDocument filter;
  github_build_filter(filter);
  JsonDocument doc;
  DeserializationError err =
      deserializeJson(doc, http.getStream(), DeserializationOption::Filter(filter));
  http.end();
  if (err) {
    log_w("github json: %s", err.c_str());
    return GithubStatus::Error;
  }
  return github_parse(doc, out);
}

static void publish(const GithubData &d) {
  xSemaphoreTake(lock, portMAX_DELAY);
  latest = d;
  fresh = true;
  xSemaphoreGive(lock);
}

static void github_task(void *) {
  static GithubData d;  // 放在静态区，任务栈留给 TLS
  for (;;) {
    uint32_t wait_ms = 5000;
    String token = settings_get().github_token;
    if (token.isEmpty()) {
      d.status = GithubStatus::NoToken;
      publish(d);
      wait_ms = portMAX_DELAY;  // 等设置页面填写后被唤醒
    } else if (WiFi.isConnected()) {
      if (d.status != GithubStatus::Ok) {
        d.status = GithubStatus::Loading;
        publish(d);
      }
      int error;
      GithubStatus s = fetch(token, d, error);
      if (s == GithubStatus::Ok) {
        d.status = s;
        wait_ms = kRefreshMs;
      } else {
        // 拉取失败时保留上次的数据，只有从没成功过才显示错误
        if (d.status != GithubStatus::Ok || s == GithubStatus::BadToken) {
          d.status = s;
          d.error = error;
        }
        wait_ms = s == GithubStatus::BadToken ? portMAX_DELAY : kRetryMs;
      }
      publish(d);
    }
    ulTaskNotifyTake(pdTRUE, wait_ms == portMAX_DELAY ? portMAX_DELAY : pdMS_TO_TICKS(wait_ms));
  }
}

void github_begin() {
  lock = xSemaphoreCreateMutex();
  latest.status = GithubStatus::Loading;
  xTaskCreate(github_task, "github", 16384, nullptr, 1, &task_handle);
}

void github_request_refresh() {
  if (task_handle) xTaskNotifyGive(task_handle);
}

bool github_take_update(GithubData &out) {
  xSemaphoreTake(lock, portMAX_DELAY);
  bool got = fresh;
  if (got) {
    out = latest;
    fresh = false;
  }
  xSemaphoreGive(lock);
  return got;
}
