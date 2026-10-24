#include "photos.h"

#include "board.h"

#ifdef HAS_SD_CARD

#include <ArduinoJson.h>
#include <LovyanGFX.hpp>
#include <SD_MMC.h>
#include <esp_heap_caps.h>

#include <algorithm>
#include <new>
#include <vector>

#include "album.h"
#include "psram_alloc.h"
#include "avi.h"
#include "avplay.h"
#include "bing.h"
#include "gif.h"

static constexpr const char *kTmp = "/photos/.upload.tmp";
static constexpr const char *kBingTmp = "/bing/.download.tmp";
static constexpr uint32_t kMountRetryMs = 10000;       // 没插卡时多久再试一次
static constexpr size_t kMaxFileBytes = 8 * 1024 * 1024;  // 再大的照片、动图就不读了，免得 PSRAM 不够
static constexpr uint64_t kMinFreeBytes = 1024 * 1024;    // 卡快满时不再接收上传和下载
static constexpr int kMaxTries = 5;  // 一次最多试几张，都读不出来就等下一次
static constexpr size_t kCaptionMax = 160;

// 文件名和列表都放 PSRAM：卡上上千张照片时，用 String 会在内部内存里占几十 KB
struct PhotoFile {
  PsString name;
  uint32_t size;
  PsString caption;  // 必应图片的说明
};

// 一个图片来源对应卡上的一个文件夹
struct Source {
  const char *dir;
  std::vector<PhotoFile, PsramStl<PhotoFile>> files;
  bool dirty = true;
  PhotoOrder order;
};

static SemaphoreHandle_t lock;
static TaskHandle_t task;
static bool mounted = false;
static bool tried = false;
static uint32_t last_try = 0;
static Source sources[2];
static int last_source = -1;
static bool order_shuffle = true;
static volatile bool want_shuffle = true;
static volatile bool use_local = true;
static volatile bool use_bing = true;
static volatile bool busy = false;
static volatile bool frame_ready = false;
static volatile int failed = 0;
static volatile bool stop_req = false;  // 离开相册页时停止放动图、视频
static volatile uint32_t gif_min_ms = 10000;
static volatile bool video_sound = true;  // 视频带声音时放出来

// 没插卡时内存里的当天必应图片
static uint8_t *mem_data = nullptr;
static size_t mem_len = 0;
static char mem_date[9] = "";
static char mem_caption[kCaptionMax] = "";

// 两帧轮流用：一帧在屏幕上显示，另一帧给后台解码
static LGFX_Sprite frames[2];
static char captions[2][kCaptionMax];
static bool animated[2];  // 这一帧是动图或视频里的
static int frame_shown = -1;
static int frame_new = 0;

static File upload_file;
static bool upload_ok = false;

static Source &local_src() { return sources[(int)PhotoSource::Local]; }
static Source &bing_src() { return sources[(int)PhotoSource::Bing]; }

static void unmount_locked() {
  SD_MMC.end();
  mounted = false;
  for (Source &s : sources) {
    s.files.clear();
    s.order.reset(0, order_shuffle, 1);
    s.dirty = true;
  }
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
  for (Source &s : sources) {
    s.dirty = true;
    if (!SD_MMC.exists(s.dir)) SD_MMC.mkdir(s.dir);
  }
  log_i("TF card mounted, %llu MB", SD_MMC.cardSize() / (1024 * 1024));
  return true;
}

static String path_of(const Source &s, const String &name) {
  return String(s.dir) + "/" + name;
}

// 必应图片的说明在同名的 .txt 里
static String caption_path(const char *date) {
  return String("/bing/") + date + ".txt";
}

static String read_caption_locked(const String &jpg_name) {
  File f = SD_MMC.open(caption_path(jpg_name.substring(0, 8).c_str()));
  if (!f) return String();
  char buf[kCaptionMax];
  int n = f.read((uint8_t *)buf, sizeof(buf) - 1);
  buf[n > 0 ? n : 0] = '\0';
  return String(buf);
}

// 重新扫描一个文件夹；卡被拔掉时卸载
static bool scan_locked(Source &s) {
  File dir = SD_MMC.open(s.dir);
  if (!dir || !dir.isDirectory()) {
    if (!SD_MMC.mkdir(s.dir)) {
      unmount_locked();  // 连文件夹都建不了，多半是卡被拔了
      return false;
    }
    dir = SD_MMC.open(s.dir);
    if (!dir) {
      unmount_locked();
      return false;
    }
  }
  bool bing = &s == &bing_src();
  s.files.clear();
  for (File f = dir.openNextFile(); f && (int)s.files.size() < kAlbumMaxPhotos; f = dir.openNextFile()) {
    const char *name = f.name();
    const char *slash = strrchr(name, '/');  // 有的版本返回完整路径
    if (slash) name = slash + 1;
    if (f.isDirectory() || !album_is_photo(name)) continue;
    if (bing && !bing_is_file(name)) continue;
    s.files.push_back({PsString(name), (uint32_t)f.size(), PsString()});
  }
  dir.close();
  std::sort(s.files.begin(), s.files.end(), [](const PhotoFile &a, const PhotoFile &b) { return a.name < b.name; });
  if (bing) {
    for (PhotoFile &f : s.files) f.caption = read_caption_locked(String(f.name.c_str())).c_str();
  }
  order_shuffle = want_shuffle;
  s.order.reset(s.files.size(), order_shuffle, esp_random());
  s.dirty = false;
  failed = 0;
  return true;
}

// 必应图片用内存里的那一张：没插卡，或者卡上还没下载好
static bool bing_from_mem() {
  return mem_data && !(mounted && bing_src().order.count() > 0);
}

static int local_count() { return mounted ? local_src().order.count() : 0; }
static int bing_count() { return mounted && bing_src().order.count() > 0 ? bing_src().order.count() : mem_data ? 1 : 0; }

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

// 选下一张并读进内存（拿着锁调用）。没有可放的图时返回 nullptr 且 name 为空。
// 视频边放边读，不读进内存，返回 nullptr，name 和 src_out 照常给出
static uint8_t *pick_locked(String &name, String &caption, size_t &len, int &src_out, MediaKind &kind) {
  name = "";
  kind = MediaKind::Still;
  bool card = mount_locked();
  bool scanned[2] = {false, false};
  bool reshuffle = order_shuffle != want_shuffle;  // 扫描时会更新 order_shuffle，先记下来
  if (card) {
    for (int i = 0; i < 2 && card; i++) {
      if (sources[i].dirty || reshuffle) {
        card = scan_locked(sources[i]);
        scanned[i] = true;
      }
    }
  }
  bool has_local = use_local && local_count() > 0;
  bool has_bing = use_bing && bing_count() > 0;
  if (card && !has_local && !has_bing) {
    // 还没有图时每次都重新看一眼文件夹，刚拷进去的照片也能跟上
    for (int i = 0; i < 2 && card; i++) {
      if (!scanned[i] && (i == 0 ? use_local : use_bing)) {
        card = scan_locked(sources[i]);
        scanned[i] = true;
      }
    }
    has_local = use_local && local_count() > 0;
    has_bing = use_bing && bing_count() > 0;
  }
  int src = album_next_source(last_source, has_local, has_bing);
  if (src < 0) return nullptr;
  src_out = src;

  if (src == (int)PhotoSource::Bing && bing_from_mem()) {
    name = String(mem_date) + ".jpg";
    caption = mem_caption;
    len = mem_len;
    auto *buf = (uint8_t *)heap_caps_malloc(len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (buf) memcpy(buf, mem_data, len);
    return buf;
  }
  Source &s = sources[src];
  bool new_round;
  int k = s.order.next(&new_round);
  if (new_round && !scanned[src]) {
    // 每放完一轮重新看一次文件夹，拔卡换照片后也能跟上
    if (!scan_locked(s)) return nullptr;
    k = s.order.next();
  }
  if (k < 0) return nullptr;
  name = s.files[k].name.c_str();
  caption = s.files[k].caption.c_str();
  kind = album_media_kind(name.c_str());
  if (kind == MediaKind::Video) return nullptr;
  return read_locked(path_of(s, name), len);
}

static int back_frame() {
  return frame_shown == 0 ? 1 : 0;
}

// 等屏幕取走上一帧；要停止时返回 false
static bool wait_taken() {
  while (frame_ready) {
    if (stop_req) return false;
    vTaskDelay(pdMS_TO_TICKS(2));
  }
  return !stop_req;
}

// 等到 due（毫秒时间），要停止时提前返回
static void wait_until(uint32_t due) {
  for (int32_t wait; !stop_req && (wait = (int32_t)(due - millis())) > 0;) vTaskDelay(pdMS_TO_TICKS(wait > 10 ? 10 : wait));
}

// 等到 due 再把画好的一帧交出去。慢了就马上交，并且不再追赶落下的时间
static void publish(int back, const char *caption, bool anim, uint32_t &due) {
  wait_until(due);
  if ((int32_t)(millis() - due) > 200) due = millis();
  strlcpy(captions[back], caption, kCaptionMax);
  animated[back] = anim;
  frame_new = back;
  frame_ready = true;
}

// 精灵里的像素是给 SPI 屏幕用的高低字节对调格式，LVGL 要的是正常字节序
static void swap_bytes(LGFX_Sprite &spr) {
  auto *px = (uint16_t *)spr.getBuffer();
  for (int i = 0; i < SCREEN_W * SCREEN_H; i++) px[i] = __builtin_bswap16(px[i]);
}

// 解码一张 JPG/PNG 到精灵，缩小到能完整放进屏幕（不放大），居中
static bool draw_still(LGFX_Sprite &spr, const uint8_t *data, size_t len, bool png) {
  spr.fillScreen(TFT_BLACK);
  bool ok = png ? spr.drawPng(data, len, 0, 0, SCREEN_W, SCREEN_H, 0, 0, 0.0f, 0.0f, lgfx::datum_t::middle_center)
                : spr.drawJpg(data, len, 0, 0, SCREEN_W, SCREEN_H, 0, 0, 0.0f, 0.0f, lgfx::datum_t::middle_center);
  if (ok) swap_bytes(spr);
  return ok;
}

static void *psram_alloc(size_t n) {
  return heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

// 放一个 GIF：至少放 gif_min_ms，不够一遍时放完这一遍。只有一帧的 GIF 当照片放。
// 打不开时返回 false
static bool play_gif(const uint8_t *data, size_t len) {
  static GifDecoder *gif = nullptr;  // LZW 表有十几 KB，放在 PSRAM，一直留着
  if (!gif) {
    void *mem = psram_alloc(sizeof(GifDecoder));
    if (!mem) return false;
    gif = new (mem) GifDecoder();
  }
  if (!gif->open(data, len)) return false;
  size_t n = (size_t)gif->width() * gif->height();
  auto *canvas = (uint16_t *)psram_alloc(n * 2);
  auto *prev = (uint16_t *)psram_alloc(n * 2);  // 申请不到也能放，只是处理方式 3 当成保留
  if (!canvas) {
    free(prev);
    return false;
  }
  uint32_t start = millis(), due = start;
  bool ok = false;
  while (wait_taken()) {
    int ms = gif->next_frame(canvas, prev);
    if (ms < 0) {
      if (!ok) break;  // 第一帧就坏了
      wait_until(due);  // 最后一帧也放够它的时间
      if (stop_req || millis() - start >= gif_min_ms) break;
      gif->rewind();
      continue;
    }
    int back = back_frame();
    media_blit_fit(canvas, gif->width(), gif->height(), (uint16_t *)frames[back].getBuffer(), SCREEN_W, SCREEN_H);
    // 只有一帧的 GIF 当照片放，交给主循环按间隔换下一张
    bool single = !ok && !gif->has_more();
    ok = true;
    publish(back, "", !single, due);
    if (single) break;
    due += ms;
  }
  free(canvas);
  free(prev);
  return ok;
}

// AviReader 的读回调：调用的地方已经拿着锁
static size_t avi_read(void *ctx, uint32_t off, uint8_t *buf, size_t len) {
  File &f = *(File *)ctx;
  if (!f.seek(off)) return 0;
  size_t got = 0;
  while (got < len) {
    int n = f.read(buf + got, len - got);
    if (n <= 0) break;
    got += n;
  }
  return got;
}

static bool video_show(void *, const uint8_t *jpg, size_t len, uint32_t) {
  if (!wait_taken()) return true;  // 要停了，下一轮就会退出
  int back = back_frame();
  if (!draw_still(frames[back], jpg, len, false)) return false;
  uint32_t now = millis();
  publish(back, "", true, now);
  return true;
}

static bool video_stop(void *) { return stop_req; }

static void video_lock(void *, bool take) {
  if (take) xSemaphoreTake(lock, portMAX_DELAY);
  else xSemaphoreGive(lock);
}

// 边读边放一个 MJPEG 视频（有声音时一起放），放到结尾或者被停止。打不开或者开头的帧都解不出来时返回 false
static bool play_video(const String &path) {
  File f;
  AviReader avi;
  xSemaphoreTake(lock, portMAX_DELAY);
  if (mounted) f = SD_MMC.open(path);
  bool ok = f && avi.open(avi_read, &f, f.size());
  xSemaphoreGive(lock);
  int shown = 0;
  if (ok) {
    AvPlayHooks hooks = {video_show, video_stop, video_lock, nullptr};
    shown = avplay_run(avi, hooks, video_sound).shown;
  }
  xSemaphoreTake(lock, portMAX_DELAY);
  if (f) f.close();
  xSemaphoreGive(lock);
  if (!ok) log_w("can't open video %s", path.c_str());
  else if (shown == 0) log_w("can't decode video %s", path.c_str());
  return shown > 0;
}

static void decode_next() {
  for (int attempt = 0; attempt < kMaxTries; attempt++) {
    String name, caption;
    size_t len = 0;
    int src = -1;
    MediaKind kind;
    xSemaphoreTake(lock, portMAX_DELAY);
    uint8_t *data = pick_locked(name, caption, len, src, kind);
    xSemaphoreGive(lock);
    if (name.isEmpty()) return;  // 没有可放的图
    last_source = src;
    if (kind == MediaKind::Video) {
      if (play_video(path_of(sources[src], name)) || stop_req) return;
      failed = failed + 1;
      continue;
    }
    if (!data) {
      failed = failed + 1;
      continue;
    }
    bool ok;
    if (kind == MediaKind::Gif) {
      ok = play_gif(data, len);
    } else {
      int back = back_frame();
      ok = draw_still(frames[back], data, len, is_png(name));
      if (ok) {
        uint32_t now = millis();
        publish(back, caption.c_str(), false, now);
      }
    }
    free(data);
    if (ok || stop_req) return;
    log_w("can't decode %s", name.c_str());
    failed = failed + 1;
  }
}

static void photo_task(void *) {
  xSemaphoreTake(lock, portMAX_DELAY);
  if (mount_locked()) {
    for (Source &s : sources) {
      if (!scan_locked(s)) break;
    }
  }
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
  local_src().dir = "/photos";
  bing_src().dir = "/bing";
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
  s.local = local_count();
  s.bing = bing_count();
  s.count = (use_local ? s.local : 0) + (use_bing ? s.bing : 0);
  if (!mounted) {
    s.card = PhotoCard::NoCard;
  } else {
    s.card = s.local > 0 ? PhotoCard::Ok : PhotoCard::Empty;
  }
  return s;
}

void photos_set_sources(bool local, bool bing) {
  use_local = local;
  use_bing = bing;
}

void photos_set_video_sound(bool on) {
  video_sound = on;
}

bool photos_card_lock() {
  xSemaphoreTake(lock, portMAX_DELAY);
  return mount_locked();
}

void photos_card_unlock() {
  xSemaphoreGive(lock);
}

void photos_request_next(bool shuffle, uint32_t gif_ms) {
  if (!task || busy || frame_ready) return;
  want_shuffle = shuffle;
  gif_min_ms = gif_ms;
  stop_req = false;
  busy = true;
  xTaskNotifyGive(task);
}

void photos_stop() {
  if (busy) stop_req = true;
}

bool photos_busy() {
  return busy;
}

bool photos_take_frame(const uint16_t *&px, const char *&caption, bool &anim) {
  if (!frame_ready) return false;
  frame_shown = frame_new;
  px = (const uint16_t *)frames[frame_shown].getBuffer();
  caption = captions[frame_shown];
  anim = animated[frame_shown];
  frame_ready = false;
  return true;
}

static void list_files(JsonDocument &doc, const char *key, const Source &s, bool with_caption) {
  JsonArray arr = doc[key].to<JsonArray>();
  for (const PhotoFile &f : s.files) {
    JsonObject o = arr.add<JsonObject>();
    o["n"] = f.name.c_str();
    o["s"] = f.size;
    if (with_caption) o["c"] = f.caption.c_str();
  }
}

String photos_list_json() {
  JsonDocument doc(psram_json());  // 上千张照片时有几十 KB
  xSemaphoreTake(lock, portMAX_DELAY);
  tried = false;  // 打开网页时立即重试挂载，刚插上的卡不用等
  bool ok = mount_locked();
  for (int i = 0; i < 2 && ok; i++) {
    if (sources[i].dirty) ok = scan_locked(sources[i]);
  }
  if (ok) {
    doc["card"] = "ok";
    doc["total"] = (double)SD_MMC.totalBytes();  // 字节数可能超过 32 位
    doc["used"] = (double)SD_MMC.usedBytes();
    list_files(doc, "photos", local_src(), false);
    list_files(doc, "bing", bing_src(), true);
  } else {
    doc["card"] = "none";
  }
  if (bing_from_mem()) {  // 没插卡时内存里的那一张
    JsonObject m = doc["bing_mem"].to<JsonObject>();
    m["n"] = String(mem_date) + ".jpg";
    m["c"] = mem_caption;
  }
  xSemaphoreGive(lock);
  doc["failed"] = (int)failed;
  String out;
  serializeJson(doc, out);
  return out;
}

static bool valid_name(PhotoSource src, const String &name) {
  if (src == PhotoSource::Bing) return bing_is_file(name.c_str());
  return name.length() > 0 && name.indexOf('/') < 0 && name.indexOf('\\') < 0 && album_is_photo(name.c_str());
}

bool photos_upload_begin(const String &filename) {
  xSemaphoreTake(lock, portMAX_DELAY);
  tried = false;
  upload_ok = mount_locked() && SD_MMC.totalBytes() - SD_MMC.usedBytes() >= kMinFreeBytes;
  if (upload_ok) {
    if (!SD_MMC.exists(local_src().dir)) SD_MMC.mkdir(local_src().dir);
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
  String name = base;
  String stem = name.substring(0, name.length() - 4), ext = name.substring(name.length() - 4);
  for (int i = 2; SD_MMC.exists(path_of(local_src(), name)) && i < 1000; i++) name = stem + "_" + i + ext;
  bool ok = SD_MMC.rename(kTmp, path_of(local_src(), name));
  if (!ok) SD_MMC.remove(kTmp);
  local_src().dirty = true;
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

// 删掉的必应图片留一个 .del 记录，免得下次又下载回来
static String deleted_path(const char *date) {
  return String("/bing/") + date + ".del";
}

bool photos_delete(PhotoSource src, const String &name) {
  if (!valid_name(src, name)) return false;
  Source &s = sources[(int)src];
  xSemaphoreTake(lock, portMAX_DELAY);
  bool ok = mounted && SD_MMC.remove(path_of(s, name));
  if (ok && src == PhotoSource::Bing) {
    String date = name.substring(0, 8);
    SD_MMC.remove(caption_path(date.c_str()));
    File f = SD_MMC.open(deleted_path(date.c_str()), FILE_WRITE);
    f.close();
  }
  s.dirty = true;
  xSemaphoreGive(lock);
  return ok;
}

bool photos_read(PhotoSource src, const String &name,
                 void (*fn)(void *ctx, const uint8_t *data, size_t len, size_t total), void *ctx) {
  if (!valid_name(src, name)) return false;
  static uint8_t buf[4096];
  xSemaphoreTake(lock, portMAX_DELAY);
  if (src == PhotoSource::Bing && mem_data && name == String(mem_date) + ".jpg" && bing_from_mem()) {
    fn(ctx, nullptr, 0, mem_len);
    for (size_t off = 0; off < mem_len; off += sizeof(buf)) {
      size_t n = std::min(sizeof(buf), mem_len - off);
      memcpy(buf, mem_data + off, n);  // 发送时不拿着内存里的原图，下载任务换图也不怕
      fn(ctx, buf, n, mem_len);
    }
    xSemaphoreGive(lock);
    return true;
  }
  File f = mounted ? SD_MMC.open(path_of(sources[(int)src], name)) : File();
  bool ok = (bool)f && !f.isDirectory();
  if (ok) {
    size_t total = f.size();
    fn(ctx, nullptr, 0, total);
    for (int n; (n = f.read(buf, sizeof(buf))) > 0;) fn(ctx, buf, n, total);
  }
  xSemaphoreGive(lock);
  return ok;
}

bool photos_bing_card() {
  xSemaphoreTake(lock, portMAX_DELAY);
  bool ok = mount_locked() && SD_MMC.totalBytes() - SD_MMC.usedBytes() >= kMinFreeBytes;
  if (ok && !SD_MMC.exists(bing_src().dir)) ok = SD_MMC.mkdir(bing_src().dir);
  xSemaphoreGive(lock);
  return ok;
}

bool photos_bing_known(const char *date) {
  if (!bing_is_date(date)) return false;
  xSemaphoreTake(lock, portMAX_DELAY);
  bool known = mounted && (SD_MMC.exists(String("/bing/") + date + ".jpg") || SD_MMC.exists(deleted_path(date)));
  xSemaphoreGive(lock);
  return known;
}

bool photos_bing_save(const char *date, const uint8_t *data, size_t len, const char *caption) {
  if (!bing_is_date(date)) return false;
  xSemaphoreTake(lock, portMAX_DELAY);
  bool ok = mounted;
  if (ok) {
    // 先写说明，再把图片改成正式文件名，扫描时看到图片就一定有说明
    File t = SD_MMC.open(caption_path(date), FILE_WRITE);
    if (t) {
      t.print(caption);
      t.close();
    }
    File f = SD_MMC.open(kBingTmp, FILE_WRITE);
    ok = f && f.write(data, len) == len;
    f.close();
    String path = String("/bing/") + date + ".jpg";
    if (ok) {
      SD_MMC.remove(path);
      ok = SD_MMC.rename(kBingTmp, path);
    }
    if (!ok) SD_MMC.remove(kBingTmp);
    bing_src().dirty = true;
  }
  xSemaphoreGive(lock);
  return ok;
}

void photos_bing_keep(const char *date, uint8_t *data, size_t len, const char *caption) {
  xSemaphoreTake(lock, portMAX_DELAY);
  free(mem_data);
  mem_data = data;
  mem_len = len;
  strlcpy(mem_date, date, sizeof(mem_date));
  strlcpy(mem_caption, caption, sizeof(mem_caption));
  xSemaphoreGive(lock);
}

bool photos_bing_kept(const char *date) {
  xSemaphoreTake(lock, portMAX_DELAY);
  bool kept = mem_data && strcmp(mem_date, date) == 0;
  xSemaphoreGive(lock);
  return kept;
}

void photos_bing_prune(int keep, const char *oldest) {
  xSemaphoreTake(lock, portMAX_DELAY);
  File dir = mounted ? SD_MMC.open(bing_src().dir) : File();
  if (!dir || !dir.isDirectory()) {
    xSemaphoreGive(lock);
    return;
  }
  std::vector<String> jpgs, dels;
  for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
    const char *name = f.name();
    const char *slash = strrchr(name, '/');
    if (slash) name = slash + 1;
    if (bing_is_file(name)) {
      jpgs.push_back(name);
    } else if (strlen(name) == 12 && strcmp(name + 8, ".del") == 0) {
      dels.push_back(name);
    }
  }
  dir.close();
  std::sort(jpgs.begin(), jpgs.end());
  for (int i = 0; i + keep < (int)jpgs.size(); i++) {
    String date = jpgs[i].substring(0, 8);
    SD_MMC.remove(path_of(bing_src(), jpgs[i]));
    SD_MMC.remove(caption_path(date.c_str()));
    bing_src().dirty = true;
  }
  for (const String &d : dels) {
    if (oldest && strncmp(d.c_str(), oldest, 8) < 0) SD_MMC.remove(path_of(bing_src(), d));
  }
  xSemaphoreGive(lock);
}

#else  // 没有 TF 卡槽的板子

void photos_begin() {}
bool photos_supported() { return false; }
PhotoStatus photos_status() { return {PhotoCard::NoSlot, 0, 0, 0, 0}; }
void photos_set_sources(bool, bool) {}
void photos_set_video_sound(bool) {}
bool photos_card_lock() { return false; }
void photos_card_unlock() {}
void photos_request_next(bool, uint32_t) {}
bool photos_busy() { return false; }
void photos_stop() {}
bool photos_take_frame(const uint16_t *&, const char *&, bool &) { return false; }
String photos_list_json() { return "{\"card\":\"noslot\"}"; }
bool photos_upload_begin(const String &) { return false; }
bool photos_upload_write(const uint8_t *, size_t) { return false; }
bool photos_upload_end(String &) { return false; }
void photos_upload_abort() {}
bool photos_delete(PhotoSource, const String &) { return false; }
bool photos_read(PhotoSource, const String &, void (*)(void *, const uint8_t *, size_t, size_t), void *) { return false; }
bool photos_bing_card() { return false; }
bool photos_bing_known(const char *) { return false; }
bool photos_bing_save(const char *, const uint8_t *, size_t, const char *) { return false; }
void photos_bing_keep(const char *, uint8_t *data, size_t, const char *) { free(data); }
bool photos_bing_kept(const char *) { return false; }
void photos_bing_prune(int, const char *) {}

#endif
