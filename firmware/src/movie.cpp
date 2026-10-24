#include "movie.h"

#include "board.h"

#ifdef HAS_SD_CARD

#include <Arduino.h>
#include <LovyanGFX.hpp>
#include <SD_MMC.h>
#include <esp_heap_caps.h>

#include <algorithm>

#include "avi.h"
#include "avplay.h"
#include "movie_util.h"
#include "photos.h"

static constexpr const char *kDir = "/movies";
static constexpr const char *kProgress = "/movies/.progress";
static constexpr const char *kProgressTmp = "/movies/.progress.tmp";
static constexpr size_t kProgressMax = 16 * 1024;
static constexpr uint32_t kSaveEveryMs = 60000;

enum : uint32_t {
  kCmdScan = 1,
  kCmdPlay = 2,
  kCmdToggle = 4,
  kCmdPause = 8,
  kCmdSeek = 16,
  kCmdClose = 32,
};

// 播放任务：第一次有命令时才建，片子关着又过了几分钟没命令就自己退出（省下 8KB 内部内存）
static TaskHandle_t task;
static bool task_starting = false;
static bool ready = false;  // movie_begin 成功了（有 TF 卡槽，内存够）
static constexpr uint32_t kTaskIdleExitMs = 3 * 60 * 1000;
static void movie_task(void *);
static SemaphoreHandle_t list_lock;
static portMUX_TYPE cmd_mux = portMUX_INITIALIZER_UNLOCKED;
static volatile uint32_t cmds = 0;
static int cmd_index = 0;
static uint32_t cmd_seek_ms = 0;

static MovieInfo *movies;  // PSRAM，kMovieMax 个
static int count = 0;
static volatile uint32_t version = 0;
static volatile MovieCard card = MovieCard::Reading;

// 正在放的片子，状态由后台任务写，主循环读
static File file;
static AviReader avi;
static volatile bool st_open = false, st_paused = false, st_loading = false, st_ended = false, st_sound = false;
static volatile uint32_t st_pos = 0, st_dur = 0;
static volatile int st_index = -1;
static const char *volatile st_error = nullptr;
static char playing_name[kMovieNameMax];
static uint32_t last_save = 0;
static int reopen_tries = 0;  // 读卡出错后重新打开过几次（这次播放）
static constexpr int kMaxReopen = 3;
// Arduino 的 File::seek 用 32 位有符号数，超过 2GB 的位置跳不过去，后半部分读不出来
static constexpr uint32_t kMaxFileBytes = 0x7FFFFFFF;

// 两帧轮流用：一帧在屏幕上显示，另一帧给后台解码
static LGFX_Sprite frames[2];
static volatile bool frame_ready = false;
static int frame_shown = -1;
static int frame_new = 0;

static uint32_t take_cmds() {
  portENTER_CRITICAL(&cmd_mux);
  uint32_t c = cmds;
  cmds = 0;
  portEXIT_CRITICAL(&cmd_mux);
  return c;
}

static void send(uint32_t cmd) {
  if (!ready) return;
  portENTER_CRITICAL(&cmd_mux);
  cmds |= cmd;
  TaskHandle_t t = task;
  bool start = !t && !task_starting;
  if (start) task_starting = true;
  portEXIT_CRITICAL(&cmd_mux);
  if (t) {
    xTaskNotifyGive(t);
    return;
  }
  if (!start) return;  // 别处正在建，建好后会看到这条命令
  // 和相册一样放在 WiFi 那个核上；优先级比相册高一点，放片子时不卡
  TaskHandle_t created = nullptr;
  if (xTaskCreatePinnedToCore(movie_task, "movie", 8192, nullptr, 2, &created, 0) != pdPASS) {
    created = nullptr;
    log_e("movie: no memory for task");
  }
  portENTER_CRITICAL(&cmd_mux);
  task = created;
  task_starting = false;
  portEXIT_CRITICAL(&cmd_mux);
}

static size_t file_read(void *ctx, uint32_t off, uint8_t *buf, size_t len) {
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

static String path_of(const char *name) {
  return String(kDir) + "/" + name;
}

// 看到哪里的记录整个读进来（拿着卡的锁调用），没有时是空字符串。buf 是 kProgressMax 字节
static void read_progress_locked(char *buf) {
  buf[0] = '\0';
  File f = SD_MMC.open(kProgress);
  if (!f) return;
  int n = f.read((uint8_t *)buf, kProgressMax - 1);
  buf[n > 0 ? n : 0] = '\0';
  f.close();
}

// 记下 name 看到第几秒：先写临时文件再改名，写到一半断电也不会把记录弄坏
static void save_progress(const char *name, uint32_t ms) {
  char *old = (char *)heap_caps_malloc(kProgressMax * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!old) return;
  char *out = old + kProgressMax;
  if (photos_card_lock()) {
    read_progress_locked(old);
    size_t n = movie_progress_set(old, name, ms / 1000, out, kProgressMax);
    File f = SD_MMC.open(kProgressTmp, FILE_WRITE);
    bool ok = f && f.write((const uint8_t *)out, n) == n;
    if (f) f.close();
    if (ok) {
      SD_MMC.remove(kProgress);
      SD_MMC.rename(kProgressTmp, kProgress);
    }
  }
  photos_card_unlock();
  free(old);
  // 列表里的也改掉
  xSemaphoreTake(list_lock, portMAX_DELAY);
  for (int i = 0; i < count; i++) {
    if (strcmp(movies[i].name, name) == 0) movies[i].pos_ms = ms / 1000 * 1000;
  }
  version = version + 1;
  xSemaphoreGive(list_lock);
}

// 重新看一眼 movies 文件夹，读出每部片子的时长
static void scan() {
  auto *found = (MovieInfo *)heap_caps_malloc(sizeof(MovieInfo) * kMovieMax, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  char *progress = (char *)heap_caps_malloc(kProgressMax, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!found || !progress) {
    free(found);
    free(progress);
    return;
  }
  int n = 0;
  MovieCard result = MovieCard::NoCard;
  if (photos_card_lock()) {
    if (!SD_MMC.exists(kDir)) SD_MMC.mkdir(kDir);
    File dir = SD_MMC.open(kDir);
    if (dir && dir.isDirectory()) {
      read_progress_locked(progress);
      for (File f = dir.openNextFile(); f && n < kMovieMax; f = dir.openNextFile()) {
        const char *name = f.name();
        const char *slash = strrchr(name, '/');  // 有的版本返回完整路径
        if (slash) name = slash + 1;
        if (f.isDirectory() || !movie_is_file(name) || strlen(name) >= kMovieNameMax) continue;
        MovieInfo &m = found[n++];
        strlcpy(m.name, name, sizeof(m.name));
        AviReader r;
        m.dur_ms = r.open(file_read, &f, f.size()) ? r.duration_ms() : 0;
        m.pos_ms = movie_progress_find(progress, m.name) * 1000;
      }
      dir.close();
      result = n > 0 ? MovieCard::Ok : MovieCard::Empty;
    }
  }
  photos_card_unlock();
  std::sort(found, found + n, [](const MovieInfo &a, const MovieInfo &b) { return strcmp(a.name, b.name) < 0; });
  xSemaphoreTake(list_lock, portMAX_DELAY);
  memcpy(movies, found, sizeof(MovieInfo) * n);
  count = n;
  card = result;
  version = version + 1;
  xSemaphoreGive(list_lock);
  free(found);
  free(progress);
}

static void close_file() {
  photos_card_lock();
  if (file) file.close();
  photos_card_unlock();
}

static uint32_t frame_ms(uint32_t f) {
  return (uint32_t)((uint64_t)f * avi.frame_us() / 1000);
}

static void close_movie() {
  if (!st_open) return;
  if (!st_error) save_progress(playing_name, st_ended ? st_dur : st_pos);
  close_file();
  st_open = false;
  st_index = -1;
}

// 解码一帧到后台那一帧，交给屏幕。等屏幕取走上一帧时有新命令就不等了
static bool show_frame(const uint8_t *jpg, size_t len) {
  while (frame_ready) {
    if (cmds) return true;
    vTaskDelay(pdMS_TO_TICKS(2));
  }
  int back = frame_shown == 0 ? 1 : 0;
  LGFX_Sprite &spr = frames[back];
  // 解码时要临时借 3.9KB 内部 RAM，内部 RAM 紧的时候偶尔借不到，等一下再试一次
  bool ok = false;
  for (int tries = 0; !ok && tries < 2; tries++) {
    if (tries) vTaskDelay(pdMS_TO_TICKS(10));
    spr.fillScreen(TFT_BLACK);
    ok = spr.drawJpg(jpg, len, 0, 0, SCREEN_W, SCREEN_H, 0, 0, 0.0f, 0.0f, lgfx::datum_t::middle_center);
  }
  if (!ok) {
    log_w("movie: can't decode frame (%u bytes), internal RAM free %u, largest %u", (unsigned)len,
          (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
          (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA));
    return false;
  }
  // 精灵里的像素是给 SPI 屏幕用的高低字节对调格式，LVGL 要的是正常字节序
  auto *px = (uint16_t *)spr.getBuffer();
  for (int i = 0; i < SCREEN_W * SCREEN_H; i++) px[i] = __builtin_bswap16(px[i]);
  frame_new = back;
  frame_ready = true;
  return true;
}

static bool hook_show(void *, const uint8_t *jpg, size_t len, uint32_t frame) {
  if (!show_frame(jpg, len)) return false;
  st_loading = false;
  st_pos = frame_ms(frame);
  // 放的时候每分钟记一次看到哪里，突然断电也差不了多少
  if (millis() - last_save >= kSaveEveryMs) {
    last_save = millis();
    save_progress(playing_name, st_pos);
  }
  return true;
}

static bool hook_stop(void *) { return cmds != 0; }

static void hook_lock(void *, bool take) {
  if (take) photos_card_lock();
  else photos_card_unlock();
}

// 暂停时跳了位置：把那一帧显示出来，读的位置再放回那一帧
static void show_still() {
  uint32_t f = avi.frame_pos(), off, len;
  photos_card_lock();
  bool ok = avi.next(off, len) && len <= 512 * 1024;
  uint8_t *buf = ok ? (uint8_t *)heap_caps_malloc(len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) : nullptr;
  ok = buf && avi.read(off, buf, len);
  avi.seek_frame(f);
  photos_card_unlock();
  if (ok) show_frame(buf, len);
  free(buf);
  st_loading = false;
}

static void seek(uint32_t ms) {
  if (st_dur && ms >= st_dur) ms = st_dur > 1000 ? st_dur - 1000 : 0;
  uint32_t f = (uint32_t)((uint64_t)ms * 1000 / avi.frame_us());
  photos_card_lock();
  avi.seek_frame(f);
  photos_card_unlock();
  st_pos = frame_ms(avi.frame_pos());
  st_ended = false;
  st_loading = true;
}

static void open_movie(int index) {
  close_movie();
  MovieInfo m;
  if (!movie_info(index, m)) return;
  st_error = nullptr;
  st_index = index;
  st_open = true;
  st_paused = false;
  st_ended = false;
  st_loading = true;
  st_pos = 0;
  st_dur = m.dur_ms;
  strlcpy(playing_name, m.name, sizeof(playing_name));
  bool ok = false, too_big = false;
  reopen_tries = 0;
  if (photos_card_lock()) {
    file = SD_MMC.open(path_of(m.name));
    too_big = file && file.size() > kMaxFileBytes;
    ok = file && !too_big && avi.open(file_read, &file, file.size());
  }
  photos_card_unlock();
  if (!ok) {
    close_file();
    st_error = too_big ? "文件超过 2GB，板子放不了后半部分。请在电脑上分成几段再转换"
                       : "打不开这个文件。请用电脑上的转换工具转成 MJPEG 的 .avi";
    st_loading = false;
    return;
  }
  st_dur = avi.duration_ms();
  st_sound = avi.has_audio();
  // 从上次看到的地方接着放，快看完了的从头放
  uint32_t start = m.pos_ms;
  if (start + kMovieEndSlackMs >= st_dur) start = 0;
  if (start > 0) seek(start);
  last_save = millis();
}

// 读卡一直出错：关掉文件重新打开，从 frame 接着放。打不开时返回 false
static bool reopen_at(uint32_t frame) {
  close_file();
  vTaskDelay(pdMS_TO_TICKS(200));
  bool ok = false;
  if (photos_card_lock()) {
    file = SD_MMC.open(path_of(playing_name));
    ok = file && avi.open(file_read, &file, file.size()) && avi.seek_frame(frame);
  }
  photos_card_unlock();
  log_w("movie: read error, reopened at frame %u: %s", (unsigned)frame, ok ? "ok" : "failed");
  return ok;
}

static void movie_task(void *) {
  for (;;) {
    bool playing = st_open && !st_paused && !st_ended && !st_error;
    if (!playing && !cmds && !ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(kTaskIdleExitMs)) && !st_open) {
      portENTER_CRITICAL(&cmd_mux);
      bool quit = cmds == 0;
      if (quit) task = nullptr;
      portEXIT_CRITICAL(&cmd_mux);
      if (quit) {
        log_i("movie: idle, task exits");
        vTaskDelete(nullptr);
      }
    }
    uint32_t c = take_cmds();
    if (c & kCmdScan) scan();
    if (c & kCmdClose) close_movie();
    if (c & kCmdPlay) open_movie(cmd_index);
    if (!st_open || st_error) continue;
    if ((c & kCmdPause) && !st_paused) {
      st_paused = true;
      save_progress(playing_name, st_pos);
    }
    if (c & kCmdToggle) {
      if (st_ended) {
        seek(0);  // 放完了再点就从头放
        st_paused = false;
      } else {
        st_paused = !st_paused;
        if (st_paused) save_progress(playing_name, st_pos);
      }
    }
    if (c & kCmdSeek) {
      seek(cmd_seek_ms);
      if (st_paused) show_still();
    }
    if (st_paused || st_ended) continue;
    AvPlayHooks hooks = {hook_show, hook_stop, hook_lock, nullptr};
    AvPlayResult r = avplay_run(avi, hooks, true);
    st_loading = false;
    if (r.shown > 0) st_pos = frame_ms(r.frame);
    if (r.read_error) {
      // 以前读卡出错会当成放完了，直接跳到结尾，还把「看到哪里」记成看完了
      uint32_t at = r.shown > 0 ? r.frame + 1 : r.frame;
      if (++reopen_tries <= kMaxReopen && reopen_at(at)) continue;
      save_progress(playing_name, st_pos);
      close_file();
      st_error = "读 TF 卡一直出错，放不下去了。可以换张卡试试，或者在电脑上检查一下卡";
    } else if (r.ended) {
      st_ended = true;
      st_pos = st_dur;
      save_progress(playing_name, st_dur);
    } else if (r.shown == 0 && !cmds) {
      st_error = "这个文件的画面解不出来。请用电脑上的转换工具重新转一下";
    } else {
      // 被命令打断：读的位置放回最后显示的那一帧的下一帧
      photos_card_lock();
      avi.seek_frame(r.shown > 0 ? r.frame + 1 : r.frame);
      photos_card_unlock();
    }
  }
}

void movie_begin() {
  list_lock = xSemaphoreCreateMutex();
  movies = (MovieInfo *)heap_caps_calloc(kMovieMax, sizeof(MovieInfo), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!movies) return;
  for (auto &f : frames) {
    f.setPsram(true);
    f.setColorDepth(16);
    if (!f.createSprite(SCREEN_W, SCREEN_H)) {
      log_e("no memory for movie frames");
      return;
    }
  }
  ready = true;
}

bool movie_supported() { return ready; }

void movie_rescan() { send(kCmdScan); }

MovieCard movie_card() { return ready ? card : MovieCard::NoSlot; }

uint32_t movie_list_version() { return version; }

int movie_count() { return count; }

bool movie_info(int i, MovieInfo &out) {
  if (!list_lock) return false;
  xSemaphoreTake(list_lock, portMAX_DELAY);
  bool ok = i >= 0 && i < count;
  if (ok) out = movies[i];
  xSemaphoreGive(list_lock);
  return ok;
}

void movie_play(int i) {
  portENTER_CRITICAL(&cmd_mux);
  cmd_index = i;
  portEXIT_CRITICAL(&cmd_mux);
  send(kCmdPlay);
}

void movie_toggle_pause() { send(kCmdToggle); }

void movie_pause() {
  if (st_open && !st_paused && !st_ended) send(kCmdPause);
}

void movie_seek_to(uint32_t ms) {
  portENTER_CRITICAL(&cmd_mux);
  cmd_seek_ms = ms;
  portEXIT_CRITICAL(&cmd_mux);
  send(kCmdSeek);
}

void movie_seek_by(int32_t ms) {
  // 连着点几下时从上一次要跳的位置接着算
  portENTER_CRITICAL(&cmd_mux);
  uint32_t base = (cmds & kCmdSeek) ? cmd_seek_ms : st_pos;
  portEXIT_CRITICAL(&cmd_mux);
  int64_t t = (int64_t)base + ms;
  movie_seek_to(t < 0 ? 0 : (uint32_t)t);
}

void movie_close() { send(kCmdClose); }

MovieStatus movie_status() {
  MovieStatus s;
  s.open = st_open;
  s.paused = st_paused;
  s.loading = st_loading;
  s.ended = st_ended;
  s.sound = st_sound;
  s.pos_ms = st_pos;
  s.dur_ms = st_dur;
  s.index = st_index;
  s.error = st_error;
  return s;
}

bool movie_playing() { return st_open && !st_paused && !st_ended && !st_error; }

bool movie_take_frame(const uint16_t *&px) {
  if (!frame_ready) return false;
  frame_shown = frame_new;
  px = (const uint16_t *)frames[frame_shown].getBuffer();
  frame_ready = false;
  return true;
}

// ---- 设置网页：上传、删除 ----

static constexpr const char *kUploadTmp = "/movies/.upload.tmp";
static constexpr size_t kUploadBuf = 32 * 1024;  // 攒够一大块再写卡，比每次写 1.4KB 快得多
static constexpr uint64_t kUploadSpare = 1024 * 1024;
static constexpr const char *kNotAvi = "不是 .avi 文件。请先用电脑上的转换工具转好再传";
static File upload_file;
static uint8_t *upload_buf;
static size_t upload_fill = 0;
static uint64_t upload_total = 0;
static bool upload_ok = false;
static const char *upload_err = "";

// 板子上在放就先停下来，等后台任务关好文件
static void stop_playing() {
  if (!st_open) return;
  send(kCmdClose);
  for (int i = 0; i < 300 && st_open; i++) delay(10);
}

void movie_rescan_now() {
  if (ready) scan();
}

bool movie_card_space(uint64_t &total, uint64_t &used) {
  bool ok = photos_card_lock();
  if (ok) {
    total = SD_MMC.totalBytes();
    used = SD_MMC.usedBytes();
  }
  photos_card_unlock();
  return ok;
}

static bool upload_fail(const char *why) {
  movie_upload_abort();
  upload_err = why;
  return false;
}

static bool upload_flush() {
  if (upload_fill == 0) return true;
  if (upload_total == upload_fill && !movie_is_avi_header(upload_buf, upload_fill)) return upload_fail(kNotAvi);
  photos_card_lock();
  bool ok = upload_file && upload_file.write(upload_buf, upload_fill) == upload_fill;
  photos_card_unlock();
  upload_fill = 0;
  return ok || upload_fail("写入 TF 卡失败，可能是卡被拔了");
}

bool movie_upload_begin(uint64_t size) {
  upload_ok = false;
  upload_fill = 0;
  upload_total = 0;
  if (!ready) {
    upload_err = "这块板子没有 TF 卡槽";
    return false;
  }
  if (size > kMaxFileBytes) {
    upload_err = "文件超过 2GB，板子放不了后半部分。请在电脑上分成几段再转换";
    return false;
  }
  if (!upload_buf) upload_buf = (uint8_t *)heap_caps_malloc(kUploadBuf, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!upload_buf) {
    upload_err = "板子内存不够";
    return false;
  }
  stop_playing();
  upload_err = "没有检测到 TF 卡";
  if (photos_card_lock()) {
    uint64_t free_bytes = SD_MMC.totalBytes() - SD_MMC.usedBytes();
    if (free_bytes < size + kUploadSpare) {
      upload_err = "TF 卡剩余空间不够";
    } else {
      if (!SD_MMC.exists(kDir)) SD_MMC.mkdir(kDir);
      upload_file = SD_MMC.open(kUploadTmp, FILE_WRITE);
      upload_ok = (bool)upload_file;
      if (!upload_ok) upload_err = "写入 TF 卡失败";
    }
  }
  photos_card_unlock();
  return upload_ok;
}

bool movie_upload_write(const uint8_t *data, size_t len) {
  // Content-Length 可能是假的（或者分块上传没有），按实际收到的再拦一次
  if (upload_ok && upload_total + len > kMaxFileBytes) {
    upload_fail("文件超过 2GB，板子放不了后半部分。请在电脑上分成几段再转换");
  }
  while (upload_ok && len > 0) {
    size_t n = std::min(len, kUploadBuf - upload_fill);
    memcpy(upload_buf + upload_fill, data, n);
    upload_fill += n;
    upload_total += n;
    data += n;
    len -= n;
    if (upload_fill == kUploadBuf) upload_flush();
  }
  return upload_ok;
}

bool movie_upload_end(const char *name, char *saved, size_t saved_size) {
  if (upload_ok && upload_total < 12) upload_fail(kNotAvi);
  if (upload_ok) upload_flush();
  if (!upload_ok) return false;
  char base[kMovieNameMax - 4];  // 留出重名时加 _2 的地方
  movie_safe_name(name, String(millis()).c_str(), base, sizeof(base));
  String stem = String(base).substring(0, strlen(base) - 4), file_name = base;
  bool ok = photos_card_lock();
  upload_file.close();
  if (ok) {
    for (int i = 2; SD_MMC.exists(path_of(file_name.c_str())) && i < 1000; i++) file_name = stem + "_" + i + ".avi";
    ok = SD_MMC.rename(kUploadTmp, path_of(file_name.c_str()));
    if (!ok) SD_MMC.remove(kUploadTmp);
  }
  photos_card_unlock();
  upload_ok = false;
  if (!ok) {
    upload_err = "写入 TF 卡失败";
    return false;
  }
  strlcpy(saved, file_name.c_str(), saved_size);
  scan();
  return true;
}

void movie_upload_abort() {
  bool had = upload_file;
  photos_card_lock();
  if (upload_file) upload_file.close();
  if (had) SD_MMC.remove(kUploadTmp);
  photos_card_unlock();
  upload_ok = false;
  upload_fill = 0;
}

const char *movie_upload_error() { return upload_err; }

bool movie_delete(const char *name) {
  if (!ready || !movie_is_file(name) || strchr(name, '/') || strchr(name, '\\')) return false;
  stop_playing();
  bool ok = photos_card_lock() && SD_MMC.remove(path_of(name));
  photos_card_unlock();
  if (ok) save_progress(name, 0);  // 看到哪里的记录也删掉
  scan();
  return ok;
}

#else  // 没有 TF 卡槽的板子

void movie_begin() {}
bool movie_supported() { return false; }
void movie_rescan() {}
MovieCard movie_card() { return MovieCard::NoSlot; }
uint32_t movie_list_version() { return 0; }
int movie_count() { return 0; }
bool movie_info(int, MovieInfo &) { return false; }
void movie_play(int) {}
void movie_toggle_pause() {}
void movie_pause() {}
void movie_seek_by(int32_t) {}
void movie_seek_to(uint32_t) {}
void movie_close() {}
MovieStatus movie_status() { return {}; }
bool movie_playing() { return false; }
bool movie_take_frame(const uint16_t *&) { return false; }
void movie_rescan_now() {}
bool movie_card_space(uint64_t &, uint64_t &) { return false; }
bool movie_upload_begin(uint64_t) { return false; }
bool movie_upload_write(const uint8_t *, size_t) { return false; }
bool movie_upload_end(const char *, char *, size_t) { return false; }
void movie_upload_abort() {}
const char *movie_upload_error() { return "这块板子没有 TF 卡槽"; }
bool movie_delete(const char *) { return false; }

#endif
