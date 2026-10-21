#include "net.h"

#include <HTTPClient.h>
#include <esp_heap_caps.h>
#include <mbedtls/platform.h>
#include <time.h>

static SemaphoreHandle_t https_mutex() {
  static SemaphoreHandle_t m = xSemaphoreCreateMutex();
  return m;
}

static void *tls_calloc(size_t n, size_t size) {
  void *p = heap_caps_calloc(n, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  return p ? p : heap_caps_calloc(n, size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}

// heap_caps_free 认得两种内存，所以切换之前从内部 RAM 分配的也能正常释放
static void tls_free(void *p) { heap_caps_free(p); }

void net_tls_use_psram() {
  if (heap_caps_get_total_size(MALLOC_CAP_SPIRAM) > 0) mbedtls_platform_set_calloc_free(tls_calloc, tls_free);
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

static const char *const kNames[] = {"天气", "GitHub", "自选股（东方财富）", "自选股（备用：腾讯）", "在线升级", "AI 语音聊天（阿里云百炼）", "必应每日一图", "资讯"};
static const char *const kLogNames[] = {"weather", "github", "stock", "stock-backup", "ota", "voice", "bing", "news"};
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
  // HTTPS 优先用 PSRAM，内部 RAM 主要是任务栈、屏幕缓冲和 WiFi
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

// ---------- 共用的后台联网任务 ----------

struct NetJobSlot {
  const char *name;
  NetJob fn;
  uint32_t wait;  // 上次返回的等待时间
  uint32_t due;   // 下次该跑的时间（millis）
  volatile bool kicked;
};

static constexpr int kMaxNetJobs = 8;
static NetJobSlot net_jobs[kMaxNetJobs];
static volatile int net_job_count = 0;
static TaskHandle_t net_worker = nullptr;

static void net_worker_task(void *) {
  UBaseType_t min_stack = UINT32_MAX;
  for (;;) {
    uint32_t wait = 60000;
    for (int i = 0; i < net_job_count; i++) {
      NetJobSlot &j = net_jobs[i];
      uint32_t now = millis();
      if (j.kicked || (j.wait != kNetJobIdle && (int32_t)(now - j.due) >= 0)) {
        j.kicked = false;
        j.wait = j.fn();
        now = millis();
        j.due = now + j.wait;
        UBaseType_t free_stack = uxTaskGetStackHighWaterMark(nullptr);  // 字节
        if (free_stack < min_stack) {
          min_stack = free_stack;
          log_i("net worker: %u bytes of stack left after %s", (unsigned)free_stack, j.name);
        }
      }
      if (j.wait != kNetJobIdle) {
        int32_t left = (int32_t)(j.due - now);
        if (left < 0) left = 0;
        if ((uint32_t)left < wait) wait = left;
      }
    }
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(wait));
  }
}

int net_job_add(const char *name, NetJob job, uint32_t first_ms) {
  int id = net_job_count;
  if (id >= kMaxNetJobs) return -1;
  NetJobSlot &j = net_jobs[id];
  j.name = name;
  j.fn = job;
  j.wait = first_ms;
  j.due = millis() + first_ms;
  j.kicked = false;
  net_job_count = id + 1;
  // TLS 握手和解析 GitHub 的回应比较吃栈，和原来最大的那个任务一样给 16KB
  if (!net_worker) xTaskCreate(net_worker_task, "net", 16384, nullptr, 1, &net_worker);
  xTaskNotifyGive(net_worker);
  return id;
}

void net_job_kick(int id) {
  if (id < 0 || id >= net_job_count) return;
  net_jobs[id].kicked = true;
  if (net_worker) xTaskNotifyGive(net_worker);
}
