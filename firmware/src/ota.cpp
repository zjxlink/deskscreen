#include "ota.h"

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <Update.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <time.h>

#include "board.h"
#include "ota_util.h"
#include "net.h"
#include "root_ca.h"
#include "settings.h"

#ifndef FW_VERSION
#define FW_VERSION "dev"
#endif

// 最新 Release 里的 manifest.json；国内访问 GitHub 慢的话，可以换成自己的镜像地址
#ifndef OTA_MANIFEST_URL
#define OTA_MANIFEST_URL "https://github.com/zjxlink/test-repo/releases/latest/download/manifest.json"
#endif

static constexpr uint32_t kFirstCheckMs = 60 * 1000;            // 开机 1 分钟后第一次检查
static constexpr uint32_t kCheckIntervalMs = 24 * 3600 * 1000;  // 之后每天检查一次
static constexpr uint32_t kRetryMs = 3600 * 1000;               // 检查失败 1 小时后重试
static constexpr uint32_t kStallMs = 20 * 1000;                 // 下载 20 秒没有数据就放弃

static constexpr uint32_t kCmdCheck = 1;
static constexpr uint32_t kCmdInstall = 2;

static TaskHandle_t task_handle;
static SemaphoreHandle_t lock;
static OtaStatus status;
static OtaManifest manifest;
static bool auto_ok = false;  // 只有正式版本之间的升级才自动安装
static size_t upload_done = 0;
static size_t upload_total = 0;

const char *ota_current_version() {
  return FW_VERSION;
}

static void set_state(OtaState s, const char *error = nullptr, int progress = -1) {
  xSemaphoreTake(lock, portMAX_DELAY);
  status.state = s;
  if (progress >= 0) status.progress = progress;
  if (error) strlcpy(status.error, error, sizeof(status.error));
  xSemaphoreGive(lock);
}

static void fail(const char *error) {
  log_w("ota: %s", error);
  set_state(OtaState::Failed, error);
}

// 在线下载和网页上传共用 Update（同一块 OTA 分区），先占到的一方才能写；
// 判断和占用在同一把锁里完成，避免两边同时开始
static bool try_claim() {
  xSemaphoreTake(lock, portMAX_DELAY);
  bool ok = status.state != OtaState::Downloading && status.state != OtaState::Done;
  if (ok) {
    status.state = OtaState::Downloading;
    status.progress = 0;
    status.error[0] = 0;
  }
  xSemaphoreGive(lock);
  return ok;
}

OtaStatus ota_status() {
  xSemaphoreTake(lock, portMAX_DELAY);
  OtaStatus s = status;
  xSemaphoreGive(lock);
  return s;
}

static void begin_https(WiFiClientSecure &client, HTTPClient &http, const char *url) {
  client.setCACert(ROOT_CA_BUNDLE);
  // Release 的下载地址会跳转到 githubusercontent.com
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.setUserAgent("DeskScreen/" FW_VERSION);
  http.setTimeout(15000);
  http.begin(client, url);
}

// 检查期间如果开始了网页上传，就不再改动状态，免得覆盖上传进度
static void check_failed(const char *error) {
  log_w("ota check: %s", error);
  xSemaphoreTake(lock, portMAX_DELAY);
  if (status.state == OtaState::Checking) {
    status.state = OtaState::Failed;
    strlcpy(status.error, error, sizeof(status.error));
  }
  xSemaphoreGive(lock);
}

static bool check() {
  set_state(OtaState::Checking, "");
  HttpsLock https;
  WiFiClientSecure client;
  HTTPClient http;
  begin_https(client, http, OTA_MANIFEST_URL);
  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    net_report(NetService::Ota, code, client);
    http.end();
    char err[64];
    snprintf(err, sizeof(err), "检查更新失败（HTTP %d）", code);
    check_failed(err);
    return false;
  }
  JsonDocument doc;
  DeserializationError e = deserializeJson(doc, http.getString());
  http.end();
  OtaManifest m;
  if (e || !ota_parse_manifest(doc, BOARD_NAME, m)) {
    net_report(NetService::Ota, kNetBadData, client);
    check_failed("版本信息格式不对");
    return false;
  }
  net_report(NetService::Ota, HTTP_CODE_OK, client);

  bool newer = ota_is_newer(FW_VERSION, m.version);
  int x, y, z;
  bool release_build = ota_parse_version(FW_VERSION, x, y, z);
  xSemaphoreTake(lock, portMAX_DELAY);
  manifest = m;
  auto_ok = newer;
  strlcpy(status.latest, m.version, sizeof(status.latest));
  // 开发版可以手动装上最新正式版，但不会自动安装
  if (status.state == OtaState::Checking) {
    status.state = newer || !release_build ? OtaState::Available : OtaState::UpToDate;
  }
  xSemaphoreGive(lock);
  return true;
}

static void download() {
  xSemaphoreTake(lock, portMAX_DELAY);
  OtaManifest m = manifest;
  xSemaphoreGive(lock);
  if (!try_claim()) return;  // 网页上传抢先开始了

  HttpsLock https;  // 下载期间其他任务的请求会排队等着
  WiFiClientSecure client;
  HTTPClient http;
  begin_https(client, http, m.url);
  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    net_report(NetService::Ota, code, client);
    http.end();
    char err[64];
    snprintf(err, sizeof(err), "下载失败（HTTP %d）", code);
    fail(err);
    return;
  }
  int len = http.getSize();
  if (len > 0 && (uint32_t)len != m.size) {
    http.end();
    fail("固件大小和版本信息不一致");
    return;
  }
  if (!Update.begin(m.size)) {
    http.end();
    fail(Update.errorString());
    return;
  }
  Update.setMD5(m.md5);

  static uint8_t buf[4096];
  WiFiClient *stream = http.getStreamPtr();
  uint32_t written = 0;
  uint32_t last_data = millis();
  while (written < m.size) {
    size_t avail = stream->available();
    if (avail == 0) {
      if (!http.connected() || millis() - last_data > kStallMs) break;
      delay(2);
      continue;
    }
    size_t want = avail < sizeof(buf) ? avail : sizeof(buf);
    if (want > m.size - written) want = m.size - written;
    size_t n = stream->readBytes(buf, want);
    if (Update.write(buf, n) != n) {
      Update.abort();
      http.end();
      fail(Update.errorString());
      return;
    }
    written += n;
    last_data = millis();
    set_state(OtaState::Downloading, nullptr, (int)((uint64_t)written * 100 / m.size));
  }
  http.end();

  if (written != m.size) {
    Update.abort();
    fail("下载中断，请稍后重试");
    return;
  }
  // end() 会校验 MD5，不一致时不会切换启动分区，旧固件照常运行
  if (!Update.end()) {
    fail(Update.errorString());
    return;
  }
  set_state(OtaState::Done, "", 100);
}

static bool in_auto_window() {
  struct tm t;
  if (!getLocalTime(&t, 0) || t.tm_year < 2024 - 1900) return false;
  return t.tm_hour >= 3 && t.tm_hour < 5;
}

static void ota_task(void *) {
  uint32_t next_check = millis() + kFirstCheckMs;
  for (;;) {
    uint32_t cmd = 0;
    xTaskNotifyWait(0, UINT32_MAX, &cmd, pdMS_TO_TICKS(60 * 1000));
    if (!WiFi.isConnected()) continue;
    OtaState s = ota_status().state;
    if (s == OtaState::Downloading || s == OtaState::Done) continue;  // 网页上传进行中或等待重启

    if ((cmd & kCmdCheck) || (cmd & kCmdInstall) || (int32_t)(millis() - next_check) >= 0) {
      next_check = millis() + (check() ? kCheckIntervalMs : kRetryMs);
    }

    bool install = (cmd & kCmdInstall) != 0;
    if (!install && auto_ok && settings_get().auto_update && in_auto_window()) install = true;
    if (install && ota_status().state == OtaState::Available) download();
  }
}

void ota_begin() {
  lock = xSemaphoreCreateMutex();
  status.state = OtaState::Idle;
  log_i("firmware %s, board %s", FW_VERSION, BOARD_NAME);
  xTaskCreate(ota_task, "ota", 12288, nullptr, 1, &task_handle);
}

void ota_check_now() {
  if (task_handle) xTaskNotify(task_handle, kCmdCheck, eSetBits);
}

void ota_install_now() {
  if (task_handle) xTaskNotify(task_handle, kCmdInstall, eSetBits);
}

bool ota_upload_begin(const uint8_t *first, size_t len, size_t total) {
  if (!try_claim()) {
    // 不改状态，在线升级继续进行；网页上显示这条提示
    xSemaphoreTake(lock, portMAX_DELAY);
    strlcpy(status.error, "正在在线升级，请稍后再上传", sizeof(status.error));
    xSemaphoreGive(lock);
    return false;
  }
  if (!ota_looks_like_app_image(first, len)) {
    fail("不是程序固件，请上传名字里不带 merged 的 .bin 文件");
    return false;
  }
  if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
    fail(Update.errorString());
    return false;
  }
  upload_done = 0;
  upload_total = total;
  return ota_upload_write(first, len);
}

bool ota_upload_write(const uint8_t *data, size_t len) {
  if (Update.write((uint8_t *)data, len) != len) {
    Update.abort();
    fail(Update.errorString());
    return false;
  }
  upload_done += len;
  // 总长度含表单开销，比固件略大，所以进度最多显示到 99%
  if (upload_total > 0) {
    uint64_t pct = (uint64_t)upload_done * 100 / upload_total;
    set_state(OtaState::Downloading, nullptr, pct > 99 ? 99 : (int)pct);
  }
  return true;
}

bool ota_upload_end() {
  if (!Update.end(true)) {
    fail(Update.errorString());
    return false;
  }
  set_state(OtaState::Done, "", 100);
  return true;
}

void ota_upload_abort() {
  Update.abort();
  fail("上传中断");
}
