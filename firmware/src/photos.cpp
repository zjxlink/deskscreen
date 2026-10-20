#include "photos.h"

#include "board.h"

#ifdef HAS_SD_CARD

#include <ArduinoJson.h>
#include <LovyanGFX.hpp>
#include <SD_MMC.h>
#include <esp_heap_caps.h>

#include <algorithm>
#include <vector>

#include "album.h"

static constexpr const char *kDir = "/photos";
static constexpr const char *kTmp = "/photos/.upload.tmp";
static constexpr uint32_t kMountRetryMs = 10000;       // 没插卡时多久再试一次
static constexpr size_t kMaxFileBytes = 8 * 1024 * 1024;  // 再大的照片就不读了，免得 PSRAM 不够
static constexpr uint64_t kMinFreeBytes = 1024 * 1024;    // 卡快满时不再接收上传
static constexpr int kMaxTries = 5;  // 一次最多试几张，都读不出来就等下一次

struct PhotoFile {
  String name;
  uint32_t size;
};

static SemaphoreHandle_t lock;
static TaskHandle_t task;
static bool mounted = false;
static bool tried = false;
static uint32_t last_try = 0;
static std::vector<PhotoFile> files;
static bool list_dirty = true;
static PhotoOrder order;
static bool order_shuffle = true;
static volatile bool want_shuffle = true;
static volatile bool busy = false;
static volatile bool frame_ready = false;
static volatile int failed = 0;

// 两帧轮流用：一帧在屏幕上显示，另一帧给后台解码
static LGFX_Sprite frames[2];
static int frame_shown = -1;
static int frame_new = 0;

static File upload_file;
static bool upload_ok = false;

static void unmount_locked() {
  SD_MMC.end();
  mounted = false;
  files.clear();
  order.reset(0, order_shuffle, 1);
  list_dirty = true;
}

static bool mount_locked() {
  if (mounted) return true;
  uint32_t now = millis();
  if (tried && now - last_try < kMountRetryMs) return false;
  tried = true;
  last_try = now;
  // 先用 4 线模式，个别卡不行再退回 1 线
  bool ok = SD_MMC.setPins(PIN_SD_CLK, PIN_SD_CMD, PIN_SD_D0, PIN_SD_D1, PIN_SD_D2, PIN_SD_D3) &&
            SD_MMC.begin("/sdcard", false);
  if (!ok) {
    SD_MMC.end();
    ok = SD_MMC.setPins(PIN_SD_CLK, PIN_SD_CMD, PIN_SD_D0) && SD_MMC.begin("/sdcard", true);
  }
  if (!ok || SD_MMC.cardType() == CARD_NONE) {
    SD_MMC.end();
    return false;
  }
  mounted = true;
  list_dirty = true;
  if (!SD_MMC.exists(kDir)) SD_MMC.mkdir(kDir);
  log_i("TF card mounted, %llu MB", SD_MMC.cardSize() / (1024 * 1024));
  return true;
}

// 重新扫描 photos 文件夹；卡被拔掉时卸载
static bool scan_locked() {
  File dir = SD_MMC.open(kDir);
  if (!dir || !dir.isDirectory()) {
    if (!SD_MMC.mkdir(kDir)) {
      unmount_locked();  // 连文件夹都建不了，多半是卡被拔了
      return false;
    }
    dir = SD_MMC.open(kDir);
    if (!dir) {
      unmount_locked();
      return false;
    }
  }
  files.clear();
  for (File f = dir.openNextFile(); f && (int)files.size() < kAlbumMaxPhotos; f = dir.openNextFile()) {
    const char *name = f.name();
    const char *slash = strrchr(name, '/');  // 有的版本返回完整路径
    if (slash) name = slash + 1;
    if (!f.isDirectory() && album_is_photo(name)) files.push_back({String(name), (uint32_t)f.size()});
  }
  std::sort(files.begin(), files.end(), [](const PhotoFile &a, const PhotoFile &b) { return a.name < b.name; });
  order_shuffle = want_shuffle;
  order.reset(files.size(), order_shuffle, esp_random());
  list_dirty = false;
  failed = 0;
  return true;
}

// 把整个文件读进 PSRAM，读完立即放开锁，解码时网页还能用卡
static uint8_t *read_locked(const String &path, size_t &len) {
  File f = SD_MMC.open(path);
  if (!f) return nullptr;
  len = f.size();
  if (len == 0 || len > kMaxFileBytes) return nullptr;
  auto *buf = (uint8_t *)heap_caps_malloc(len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!buf) return nullptr;
  size_t got = 0;
  while (got < len) {
    int n = f.read(buf + got, len - got);
    if (n <= 0) break;
    got += n;
  }
  if (got != len) {
    free(buf);
    return nullptr;
  }
  return buf;
}

static bool is_png(String name) {
  name.toLowerCase();
  return name.endsWith(".png");
}

static void decode_next() {
  for (int attempt = 0; attempt < kMaxTries; attempt++) {
    xSemaphoreTake(lock, portMAX_DELAY);
    if (!mount_locked()) {
      xSemaphoreGive(lock);
      return;
    }
    bool scanned = false;
    if (list_dirty || order_shuffle != want_shuffle) {
      if (!scan_locked()) {
        xSemaphoreGive(lock);
        return;
      }
      scanned = true;
    }
    bool new_round;
    int k = order.next(&new_round);
    if ((new_round || k < 0) && !scanned) {
      // 每放完一轮（或者还没有照片时）重新看一次文件夹，拔卡换照片后也能跟上
      if (!scan_locked()) {
        xSemaphoreGive(lock);
        return;
      }
      k = order.next();
    }
    if (k < 0) {
      xSemaphoreGive(lock);
      return;
    }
    String name = files[k].name;
    size_t len = 0;
    uint8_t *data = read_locked(String(kDir) + "/" + name, len);
    xSemaphoreGive(lock);
    if (!data) {
      failed = failed + 1;
      continue;
    }

    int back = frame_shown == 0 ? 1 : 0;
    LGFX_Sprite &spr = frames[back];
    spr.fillScreen(TFT_BLACK);
    // 缩小到能完整放进屏幕（不放大），居中
    bool ok = is_png(name)
                  ? spr.drawPng(data, len, 0, 0, SCREEN_W, SCREEN_H, 0, 0, 0.0f, 0.0f, lgfx::datum_t::middle_center)
                  : spr.drawJpg(data, len, 0, 0, SCREEN_W, SCREEN_H, 0, 0, 0.0f, 0.0f, lgfx::datum_t::middle_center);
    free(data);
    if (!ok) {
      log_w("can't decode %s", name.c_str());
      failed = failed + 1;
      continue;
    }
    // 精灵里的像素是给 SPI 屏幕用的高低字节对调格式，LVGL 要的是正常字节序
    auto *px = (uint16_t *)spr.getBuffer();
    for (int i = 0; i < SCREEN_W * SCREEN_H; i++) px[i] = __builtin_bswap16(px[i]);
    frame_new = back;
    frame_ready = true;
    return;
  }
}

static void photo_task(void *) {
  xSemaphoreTake(lock, portMAX_DELAY);
  if (mount_locked()) scan_locked();
  xSemaphoreGive(lock);
  busy = false;
  for (;;) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    decode_next();
    busy = false;
  }
}

void photos_begin() {
  lock = xSemaphoreCreateMutex();
  pinMode(PIN_SD_POWER, OUTPUT);
  digitalWrite(PIN_SD_POWER, LOW);
  for (auto &f : frames) {
    f.setPsram(true);
    f.setColorDepth(16);
    if (!f.createSprite(SCREEN_W, SCREEN_H)) {
      log_e("no memory for photo frames");
      return;
    }
  }
  busy = true;  // 开机先挂载、扫描一次
  // 解码时间长，放在 WiFi 那个核上低优先级跑，不影响界面
  xTaskCreatePinnedToCore(photo_task, "photos", 8192, nullptr, 1, &task, 0);
}

bool photos_supported() {
  return true;
}

PhotoStatus photos_status() {
  PhotoStatus s = {};
  s.failed = failed;
  if (!mounted) {
    s.card = PhotoCard::NoCard;
    return s;
  }
  s.count = order.count();
  s.card = s.count > 0 ? PhotoCard::Ok : PhotoCard::Empty;
  return s;
}

void photos_request_next(bool shuffle) {
  if (!task || busy || frame_ready) return;
  want_shuffle = shuffle;
  busy = true;
  xTaskNotifyGive(task);
}

bool photos_busy() {
  return busy;
}

bool photos_take_frame(const uint16_t *&px) {
  if (!frame_ready) return false;
  frame_shown = frame_new;
  px = (const uint16_t *)frames[frame_shown].getBuffer();
  frame_ready = false;
  return true;
}

String photos_list_json() {
  JsonDocument doc;
  xSemaphoreTake(lock, portMAX_DELAY);
  tried = false;  // 打开网页时立即重试挂载，刚插上的卡不用等
  if (mount_locked() && (!list_dirty || scan_locked())) {
    doc["card"] = "ok";
    doc["total"] = (double)SD_MMC.totalBytes();  // 字节数可能超过 32 位
    doc["used"] = (double)SD_MMC.usedBytes();
    JsonArray arr = doc["photos"].to<JsonArray>();
    for (const PhotoFile &f : files) {
      JsonObject o = arr.add<JsonObject>();
      o["n"] = f.name;
      o["s"] = f.size;
    }
  } else {
    doc["card"] = "none";
  }
  xSemaphoreGive(lock);
  doc["failed"] = (int)failed;
  String out;
  serializeJson(doc, out);
  return out;
}

static bool valid_name(const String &name) {
  return name.length() > 0 && name.indexOf('/') < 0 && name.indexOf('\\') < 0 && album_is_photo(name.c_str());
}

bool photos_upload_begin(const String &filename) {
  xSemaphoreTake(lock, portMAX_DELAY);
  tried = false;
  upload_ok = mount_locked() && SD_MMC.totalBytes() - SD_MMC.usedBytes() >= kMinFreeBytes;
  if (upload_ok) {
    if (!SD_MMC.exists(kDir)) SD_MMC.mkdir(kDir);
    upload_file = SD_MMC.open(kTmp, FILE_WRITE);
    upload_ok = (bool)upload_file;
  }
  xSemaphoreGive(lock);
  return upload_ok;
}

bool photos_upload_write(const uint8_t *data, size_t len) {
  if (!upload_ok) return false;
  xSemaphoreTake(lock, portMAX_DELAY);
  upload_ok = upload_file.write(data, len) == len;
  xSemaphoreGive(lock);
  return upload_ok;
}

bool photos_upload_end(String &saved_name) {
  if (!upload_ok) {
    photos_upload_abort();
    return false;
  }
  xSemaphoreTake(lock, portMAX_DELAY);
  upload_file.close();
  // saved_name 进来时是浏览器给的文件名，重名时加 _2、_3……
  char base[64];
  album_safe_name(saved_name.c_str(), String(millis()).c_str(), base, sizeof(base));
  String stem = String(base).substring(0, strlen(base) - 4);
  String name = base;
  for (int i = 2; SD_MMC.exists(String(kDir) + "/" + name) && i < 1000; i++) name = stem + "_" + i + ".jpg";
  bool ok = SD_MMC.rename(kTmp, String(kDir) + "/" + name);
  if (!ok) SD_MMC.remove(kTmp);
  list_dirty = true;
  xSemaphoreGive(lock);
  saved_name = name;
  return ok;
}

void photos_upload_abort() {
  xSemaphoreTake(lock, portMAX_DELAY);
  if (upload_file) upload_file.close();
  if (mounted) SD_MMC.remove(kTmp);
  upload_ok = false;
  xSemaphoreGive(lock);
}

bool photos_delete(const String &name) {
  if (!valid_name(name)) return false;
  xSemaphoreTake(lock, portMAX_DELAY);
  bool ok = mounted && SD_MMC.remove(String(kDir) + "/" + name);
  list_dirty = true;
  xSemaphoreGive(lock);
  return ok;
}

bool photos_read(const String &name, void (*fn)(void *ctx, const uint8_t *data, size_t len, size_t total), void *ctx) {
  if (!valid_name(name)) return false;
  static uint8_t buf[4096];
  xSemaphoreTake(lock, portMAX_DELAY);
  File f = mounted ? SD_MMC.open(String(kDir) + "/" + name) : File();
  bool ok = (bool)f && !f.isDirectory();
  if (ok) {
    size_t total = f.size();
    fn(ctx, nullptr, 0, total);
    for (int n; (n = f.read(buf, sizeof(buf))) > 0;) fn(ctx, buf, n, total);
  }
  xSemaphoreGive(lock);
  return ok;
}

#else  // 没有 TF 卡槽的板子

void photos_begin() {}
bool photos_supported() { return false; }
PhotoStatus photos_status() { return {PhotoCard::NoSlot, 0, 0}; }
void photos_request_next(bool) {}
bool photos_busy() { return false; }
bool photos_take_frame(const uint16_t *&) { return false; }
String photos_list_json() { return "{\"card\":\"noslot\"}"; }
bool photos_upload_begin(const String &) { return false; }
bool photos_upload_write(const uint8_t *, size_t) { return false; }
bool photos_upload_end(String &) { return false; }
void photos_upload_abort() {}
bool photos_delete(const String &) { return false; }
bool photos_read(const String &, void (*)(void *, const uint8_t *, size_t, size_t), void *) { return false; }

#endif
