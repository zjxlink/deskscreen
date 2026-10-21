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

static int job = -1;
static SemaphoreHandle_t lock;
static GithubData latest;
static bool fresh = false;

static const char kQuery[] =
    "{\"query\":\"query{viewer{login contributionsCollection{contributionCalendar{"
    "totalContributions weeks{contributionDays{contributionCount contributionLevel weekday}}}}}}\"}";

// 失败时 error 为 HTTP 状态码、HTTPClient 的负数错误码或 kNetBadData
static GithubStatus fetch(const String &token, GithubData &out, int &error) {
  error = kNetBadData;
  HttpsLock https;
  WiFiClientSecure client;
  client.setCACert(ROOT_CA_BUNDLE);
  client.setHandshakeTimeout(kTlsHandshakeTimeoutS);
  HTTPClient http;
  http.setTimeout(15000);
  if (!http.begin(client, "https://api.github.com/graphql")) {
    error = HTTPC_ERROR_CONNECTION_REFUSED;
    return GithubStatus::Error;
  }
  http.addHeader("Authorization", "bearer " + token);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("User-Agent", "DeskScreen");

  int status = http.POST((uint8_t *)kQuery, strlen(kQuery));
  if (status != HTTP_CODE_OK) {
    net_report(NetService::Github, status, client);
    error = status;
    http.end();
    return status == 401 ? GithubStatus::BadToken : GithubStatus::Error;
  }

  // 先把整个响应读完再解析，不要边收边解析（不要用 HTTP/1.0 + getStream）：
  // HTTP/1.0 下服务器发完就断开，WiFiClientSecure 一发现断开就丢掉还没读的数据，JSON 只收到一半。
  // 响应二三十 KB，String 超过 4KB 会自动放到 PSRAM
  String body = http.getString();
  http.end();
  JsonDocument filter;
  github_build_filter(filter);
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, body, DeserializationOption::Filter(filter));
  if (err) {
    String detail = String("JSON: ") + err.c_str() + ", " + body.length() + " bytes";
    net_report(NetService::Github, kNetBadData, client, detail.c_str());
    return GithubStatus::Error;
  }
  GithubStatus s = github_parse(doc, out);
  // 解析失败时多半是 GraphQL 返回了 errors，把原因记下来
  net_report(NetService::Github, s == GithubStatus::Ok ? HTTP_CODE_OK : kNetBadData, client,
             s == GithubStatus::Ok ? nullptr : (doc["errors"][0]["message"] | "missing data.viewer"));
  return s;
}

static void publish(const GithubData &d) {
  xSemaphoreTake(lock, portMAX_DELAY);
  latest = d;
  fresh = true;
  xSemaphoreGive(lock);
}

static uint32_t github_step() {
  static GithubData d;  // 放在静态区，任务栈留给 TLS
  static int failures = 0;
  String token = settings_get().github_token;
  if (token.isEmpty()) {
    d.status = GithubStatus::NoToken;
    publish(d);
    return kNetJobIdle;  // 等设置页面填写后被唤醒
  }
  if (!WiFi.isConnected()) return 5000;
  if (d.status != GithubStatus::Ok) {
    d.status = GithubStatus::Loading;
    publish(d);
  }
  int error;
  GithubStatus s = fetch(token, d, error);
  uint32_t wait_ms;
  if (s == GithubStatus::Ok) {
    d.status = s;
    failures = 0;
    wait_ms = kRefreshMs;
  } else {
    // 拉取失败时保留上次的数据，只有从没成功过才显示错误
    if (d.status != GithubStatus::Ok || s == GithubStatus::BadToken) {
      d.status = s;
      d.error = error;
    }
    wait_ms = s == GithubStatus::BadToken ? kNetJobIdle : net_retry_ms(++failures, kRetryMs);
  }
  publish(d);
  return wait_ms;
}

void github_begin() {
  lock = xSemaphoreCreateMutex();
  latest.status = GithubStatus::Loading;
  job = net_job_add("github", github_step);
}

void github_request_refresh() {
  net_job_kick(job);
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
