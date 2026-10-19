#include "net.h"

#include <HTTPClient.h>
#include <esp_heap_caps.h>
#include <time.h>

static SemaphoreHandle_t https_mutex() {
  static SemaphoreHandle_t m = xSemaphoreCreateMutex();
  return m;
}

HttpsLock::HttpsLock() { xSemaphoreTake(https_mutex(), portMAX_DELAY); }

HttpsLock::~HttpsLock() { xSemaphoreGive(https_mutex()); }

struct NetResult {
  bool tried;
  int code;
  char when[8];     // HH:MM，还没对时则是开机后的秒数
  char detail[96];  // 失败原因：TLS 错误或调用方给的说明
};

static NetResult results[(int)NetService::Count];
static portMUX_TYPE results_mux = portMUX_INITIALIZER_UNLOCKED;

static const char *const kNames[] = {"天气", "GitHub", "自选股（东方财富）", "自选股（备用：腾讯）", "在线升级"};
static const char *const kLogNames[] = {"weather", "github", "stock", "stock-backup", "ota"};
static_assert(sizeof(kNames) / sizeof(kNames[0]) == (int)NetService::Count, "kNames out of sync");
static_assert(sizeof(kLogNames) / sizeof(kLogNames[0]) == (int)NetService::Count, "kLogNames out of sync");

void net_report(NetService s, int code, WiFiClientSecure &client, const char *detail) {
  // 连接成功时 lastError 返回的是 socket 编号（正数），只有负数才是 TLS 错误
  char tls[96] = "TLS: ";
  if (!detail && code != HTTP_CODE_OK && client.lastError(tls + 5, sizeof(tls) - 5) < 0) detail = tls;
  net_report(s, code, detail);
}

void net_report(NetService s, int code, const char *detail) {
  NetResult r = {};
  r.tried = true;
  r.code = code;
  struct tm t;
  if (getLocalTime(&t, 0) && t.tm_year >= 2024 - 1900) {
    snprintf(r.when, sizeof(r.when), "%02d:%02d", t.tm_hour, t.tm_min);
  } else {
    snprintf(r.when, sizeof(r.when), "%lus", (unsigned long)(millis() / 1000));
  }
  if (code != HTTP_CODE_OK) {
    if (detail) strlcpy(r.detail, detail, sizeof(r.detail));
    String reason = code == kNetBadData ? String("bad data")
                    : code < 0          ? HTTPClient::errorToString(code)
                                        : String("http status");
    log_w("%s http %d (%s), %s, internal heap %u, largest block %u", kLogNames[(int)s], code,
          reason.c_str(), r.detail[0] ? r.detail : "-", heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
          heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
  }
  portENTER_CRITICAL(&results_mux);
  results[(int)s] = r;
  portEXIT_CRITICAL(&results_mux);
}

static String escape(const char *s) {
  String out;
  for (; *s; s++) {
    if (*s == '<') out += "&lt;";
    else if (*s == '>') out += "&gt;";
    else if (*s == '&') out += "&amp;";
    else out += *s;
  }
  return out;
}

String net_status_html() {
  NetResult copy[(int)NetService::Count];
  portENTER_CRITICAL(&results_mux);
  memcpy(copy, results, sizeof(copy));
  portEXIT_CRITICAL(&results_mux);

  String html;
  for (int i = 0; i < (int)NetService::Count; i++) {
    const NetResult &r = copy[i];
    html += "<li>";
    html += kNames[i];
    html += "：";
    if (!r.tried) {
      html += "还没有请求过";
    } else if (r.code == HTTP_CODE_OK) {
      html += String("成功（") + r.when + "）";
    } else {
      html += String("<b>失败 ") + r.code + "</b>（" + r.when + "）";
      if (r.code == kNetBadData) {
        html += " 返回内容格式不对";
      } else if (r.code < 0) {
        html += " " + escape(HTTPClient::errorToString(r.code).c_str());
      }
      if (r.detail[0]) html += "<br><small>" + escape(r.detail) + "</small>";
    }
    html += "</li>";
  }
  html += String("<li>内部 RAM：剩余 ") + heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024 +
          " KB，开机以来最少 " + heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL) / 1024 +
          " KB，最大连续块 " + heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) / 1024 + " KB</li>";
  // PSRAM 只放界面对象、图片和大块数据，HTTPS 用不到，所以单独列一行
  size_t psram_total = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
  if (psram_total == 0) {
    html += "<li>PSRAM：<b>没有检测到</b></li>";
  } else {
    html += String("<li>PSRAM：共 ") + psram_total / 1024 + " KB，剩余 " +
            heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024 + " KB，开机以来最少 " +
            heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM) / 1024 + " KB</li>";
  }
  return html;
}

uint32_t net_retry_ms(int failures, uint32_t max_ms) {
  uint32_t ms = 10 * 1000;
  for (int i = 1; i < failures && ms < max_ms; i++) ms *= 2;
  return ms < max_ms ? ms : max_ms;
}
