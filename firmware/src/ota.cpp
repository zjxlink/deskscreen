#include "ota.h"

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <Update.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <esp_ota_ops.h>
#include <time.h>

#include "board.h"
#include "ota_util.h"
#include "net.h"
#include "root_ca.h"
#include "settings.h"

#ifndef FW_VERSION
#define FW_VERSION "dev"
#endif

// 最新 Release 里的 manifest.json。设置里填了自己的升级服务器时先试那边，失败了再来这里
#ifndef OTA_MANIFEST_URL
#define OTA_MANIFEST_URL "https://github.com/zjxlink/test-repo/releases/latest/download/manifest.json"
#endif

static constexpr uint32_t kFirstCheckMs = 60 * 1000;            // 开机 1 分钟后第一次检查
static constexpr uint32_t kCheckIntervalMs = 24 * 3600 * 1000;  // 之后每天检查一次
static constexpr uint32_t kRetryMs = 3600 * 1000;               // 检查失败 1 小时后重试
static constexpr uint32_t kStallMs = 20 * 1000;                 // 下载 20 秒没有数据就放弃

static constexpr uint32_t kCmdCheck = 1;
static constexpr uint32_t kCmdInstall = 2;

static int job = -1;
static uint32_t pending_cmd = 0;  // 网页上点的「检查更新」「立即升级」，kCmd* 的组合
static SemaphoreHandle_t lock;
static OtaStatus status;
static OtaManifest manifest;
static bool auto_ok = false;  // 只有正式版本之间的升级才自动安装
static size_t upload_done = 0;
static size_t upload_total = 0;

// ---- 升级后自检：新固件启动后要稳定运行一段时间才算数，之前重启（崩溃、看门狗、死循环）就退回旧固件 ----
// Arduino 默认一开机就确认新固件，这里改成稍后由 ota_confirm_tick 确认
extern "C" bool verifyRollbackLater() { return true; }

static constexpr uint32_t kConfirmAfterMs = 30000;
static bool confirm_pending = true;

void ota_confirm_tick(uint32_t now_ms) {
  if (!confirm_pending || now_ms < kConfirmAfterMs) return;
  confirm_pending = false;
  esp_ota_img_states_t state;
  const esp_partition_t *running = esp_ota_get_running_partition();
  if (esp_ota_get_state_partition(running, &state) == ESP_OK && state == ESP_OTA_IMG_PENDING_VERIFY) {
    esp_ota_mark_app_valid_cancel_rollback();
    Serial.println("[OTA] 新固件运行正常，已确认，不再回滚");
  }
}

bool ota_rolled_back() {
  const esp_partition_t *last = esp_ota_get_last_invalid_partition();
  return last != nullptr;
}

const char *ota_current_version() {
  return FW_VERSION;
}

static void (*busy_hook)(bool) = nullptr;

void ota_set_busy_hook(void (*hook)(bool busy)) { busy_hook = hook; }

static void set_state(OtaState s, const char *error = nullptr, int progress = -1) {
  xSemaphoreTake(lock, portMAX_DELAY);
  bool was_busy = status.state == OtaState::Downloading;
  status.state = s;
  if (progress >= 0) status.progress = progress;
  if (error) strlcpy(status.error, error, sizeof(status.error));
  xSemaphoreGive(lock);
  if (was_busy && (s == OtaState::Failed || s == OtaState::Idle) && busy_hook) busy_hook(false);
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
  if (ok && busy_hook) busy_hook(true);
  return ok;
}

OtaStatus ota_status() {
  xSemaphoreTake(lock, portMAX_DELAY);
  OtaStatus s = status;
  xSemaphoreGive(lock);
  return s;
}

// 自建服务器可能只有 http（比如局域网里的 NAS），所以两种连接都要能用
struct Conn {
  WiFiClientSecure tls;
  WiFiClient plain;
  HTTPClient http;
  bool secure = true;
};

static void begin_get(Conn &c, const char *url) {
  c.secure = strncmp(url, "https://", 8) == 0;
  // Release 的下载地址会跳转到 githubusercontent.com
  c.http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  c.http.setUserAgent("DeskScreen/" FW_VERSION);
  c.http.setTimeout(15000);
  if (c.secure) {
    c.tls.setCACert(ROOT_CA_BUNDLE);
    c.tls.setHandshakeTimeout(kTlsHandshakeTimeoutS);
    c.http.begin(c.tls, url);
  } else {
    c.http.begin(c.plain, url);
  }
}

static void report(Conn &c, int code, const char *detail = nullptr) {
  if (c.secure) {
    net_report(NetService::Ota, code, c.tls, detail);
  } else {
    net_report(NetService::Ota, code, detail);
  }
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

// 从一个地址取 manifest，失败时把原因写进 err
static bool fetch_manifest(const char *url, OtaManifest &m, char *err, size_t n) {
  HttpsLock https;
  Conn c;
  begin_get(c, url);
  int code = c.http.GET();
  if (code != HTTP_CODE_OK) {
    report(c, code);
    c.http.end();
    snprintf(err, n, "HTTP %d", code);
    return false;
  }
  JsonDocument doc;
  DeserializationError e = deserializeJson(doc, c.http.getString());
  c.http.end();
  if (e || !ota_parse_manifest(doc, BOARD_NAME, m, url)) {
    report(c, kNetBadData, e ? e.c_str() : "manifest has no matching board");
    strlcpy(err, "格式错", n);  // 和两个来源的原因拼在一起，要短，状态里只有 64 字节
    return false;
  }
  report(c, HTTP_CODE_OK);
  return true;
}

static bool check() {
  set_state(OtaState::Checking, "");
  String own = settings_get().ota_url;
  OtaManifest m;
  bool mirror = false;
  char own_err[32] = "", gh_err[32] = "";
  if (own.length() > 0) {
    mirror = fetch_manifest(own.c_str(), m, own_err, sizeof(own_err));
    if (!mirror) log_w("ota: own server failed (%s), trying GitHub", own_err);
  }
  if (!mirror && !fetch_manifest(OTA_MANIFEST_URL, m, gh_err, sizeof(gh_err))) {
    char err[64];
    if (own.length() > 0) {
      snprintf(err, sizeof(err), "检查更新失败（自建 %s，GitHub %s）", own_err, gh_err);
    } else {
      snprintf(err, sizeof(err), "检查更新失败（%s）", gh_err);
    }
    check_failed(err);
    return false;
  }

  bool newer = ota_is_newer(FW_VERSION, m.version);
  int x, y, z;
  bool release_build = ota_parse_version(FW_VERSION, x, y, z);
  xSemaphoreTake(lock, portMAX_DELAY);
  manifest = m;
  auto_ok = newer;
  strlcpy(status.latest, m.version, sizeof(status.latest));
  status.mirror = mirror;
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
  Conn c;
  begin_get(c, m.url);
  HTTPClient &http = c.http;
  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    report(c, code);
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

static uint32_t ota_step() {
  static uint32_t next_check = millis() + kFirstCheckMs;
  uint32_t cmd = __atomic_exchange_n(&pending_cmd, 0, __ATOMIC_SEQ_CST);
  if (!WiFi.isConnected()) return 60 * 1000;
  OtaState s = ota_status().state;
  if (s == OtaState::Downloading || s == OtaState::Done) return 60 * 1000;  // 网页上传进行中或等待重启

  if ((cmd & kCmdCheck) || (cmd & kCmdInstall) || (int32_t)(millis() - next_check) >= 0) {
    next_check = millis() + (check() ? kCheckIntervalMs : kRetryMs);
  }

  bool install = (cmd & kCmdInstall) != 0;
  if (!install && auto_ok && settings_get().auto_update && in_auto_window()) install = true;
  if (install && ota_status().state == OtaState::Available) {
    download();
    // 下载失败（多半是连不上 GitHub）时 1 小时后重新检查，状态回到「有新版本」，自动升级时段内还能再试
    if (ota_status().state == OtaState::Failed) next_check = millis() + kRetryMs;
  }
  return 60 * 1000;
}

void ota_begin() {
  lock = xSemaphoreCreateMutex();
  status.state = OtaState::Idle;
  log_i("firmware %s, board %s", FW_VERSION, BOARD_NAME);
  job = net_job_add("ota", ota_step);  // 第一轮只记下检查时间，1 分钟后才真的检查
}

void ota_check_now() {
  __atomic_fetch_or(&pending_cmd, kCmdCheck, __ATOMIC_SEQ_CST);
  net_job_kick(job);
}

void ota_install_now() {
  __atomic_fetch_or(&pending_cmd, kCmdInstall, __ATOMIC_SEQ_CST);
  net_job_kick(job);
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
