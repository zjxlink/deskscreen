#include "net.h"

#include <HTTPClient.h>
#include <esp_heap_caps.h>

static SemaphoreHandle_t https_mutex() {
  static SemaphoreHandle_t m = xSemaphoreCreateMutex();
  return m;
}

HttpsLock::HttpsLock() { xSemaphoreTake(https_mutex(), portMAX_DELAY); }

HttpsLock::~HttpsLock() { xSemaphoreGive(https_mutex()); }

void net_log_failure(const char *what, int code, WiFiClientSecure &client) {
  char tls[96] = "";
  client.lastError(tls, sizeof(tls));
  String reason = code < 0 ? HTTPClient::errorToString(code) : String("http status");
  log_w("%s http %d (%s), tls: %s, internal heap %u, largest block %u", what, code, reason.c_str(),
        tls[0] ? tls : "-", heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
        heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
}
