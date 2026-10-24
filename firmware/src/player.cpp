#include "player.h"

#include "board.h"

#if defined(HAS_SD_CARD) && defined(HAS_AUDIO)

#include <Arduino.h>
#include <LovyanGFX.hpp>
#include <SD_MMC.h>
#include <esp_heap_caps.h>

#include <algorithm>
#include <new>

#include "audio.h"
#include "photos.h"
#include "song_decoder.h"
#include "text_conv.h"

static constexpr const char *kDir = "/music";
static constexpr const char *kIndex = "/music/.index";
static constexpr const char *kIndexTmp = "/music/.index.tmp";
static constexpr const char *kState = "/music/.state";
static constexpr const char *kStateTmp = "/music/.state.tmp";
static constexpr uint32_t kAheadMs = 500;         // 喇叭的缓冲里攒着这么多（暂停时扔掉的也就这么多）
static constexpr size_t kChunk = 1152;            // 一次解这么多采样
static constexpr uint32_t kMinRate = 4000;        // 低于它要重复 5 次以上，pcm 缓冲（kChunk * 4）放不下
static constexpr uint32_t kMaxRate = 192000;
static constexpr uint32_t kSaveEveryMs = 30000;   // 放的时候每 30 秒记一次放到哪里
static constexpr uint32_t kRestartPrevMs = 3000;  // 放了 3 秒以上点「上一首」是从头放这首
static constexpr uint32_t kFadeMs = 10000;        // 睡眠定时到点前 10 秒慢慢变小
static constexpr size_t kLrcMax = 48 * 1024;      // 歌词文件最大这么多
static constexpr int kLrcLines = 1000;
static constexpr size_t kLrcPool = 160 * 1024;
static constexpr size_t kCoverMax = 1024 * 1024;  // 封面图最大 1MB

enum : uint32_t {
  kCmdPlayView = 1,
  kCmdToggle = 2,
  kCmdPause = 4,
  kCmdResume = 8,
  kCmdNext = 16,
  kCmdPrev = 32,
  kCmdSeek = 64,
  kCmdMode = 128,
  kCmdListChanged = 256,
};

// ---- 歌曲列表（扫描任务写，主循环和播放任务读，都拿 list_lock） ----

struct Library {
  TrackInfo *tracks;
  uint8_t *folder_of;  // 每首在第几个歌单（0 表示直接放在 music 下）
  int count;
  char (*folders)[kTagMax];  // kPlayerMaxFolders 个，0 号是「全部」（放在 PSRAM）
  int folder_count;
};

static SemaphoreHandle_t list_lock;
static Library libs[2];  // 扫描时写另一份，写好了再换过来
static Library *lib = &libs[0];
static volatile uint32_t version = 0;
static volatile PlayerCard card = PlayerCard::Reading;
static TaskHandle_t scan_task_handle;
static SemaphoreHandle_t scan_lock;  // 扫描任务和网页可能同时要扫

// 列表这一屏的歌单
static int view_folder = 0;
static int *view_ids;
static int view_n = 0;

// ---- 播放（播放任务写，主循环读） ----

static TaskHandle_t task;     // 播放任务，第一次要放歌时才建（省下 10KB 内部内存，开机时 WiFi 和 TLS 要用）
static bool ready = false;    // player_begin 成功了
static void start_player_task();
static portMUX_TYPE cmd_mux = portMUX_INITIALIZER_UNLOCKED;
static volatile uint32_t cmds = 0;
static int cmd_view_folder = 0, cmd_view_index = 0;
static uint32_t cmd_seek_ms = 0;

static SemaphoreHandle_t meta_lock;  // 下面这些在播放任务里换，主循环复制
static PlayerStatus st;
static char cur_path[kTrackPathMax];
static Lyrics lyrics;
static uint16_t *cover_px;
static bool cover_ok = false;

static int queue_folder = 0;  // 正在放的歌单
static int *queue_ids;        // 歌单里每首在列表里的序号
static int queue_n = 0;
static PlayOrder *order;
static volatile bool want_play = false;  // 用户想让它放着
static volatile bool yielded = false;
static uint32_t sleep_until = 0;  // millis()，0 表示没开睡眠定时
static volatile int sleep_minutes = 0;

static File file;
static SongDecoder dec;
static uint32_t song_rate = 0;   // 歌的采样率
static uint32_t io_rate = 0;     // 喇叭的采样率
static int up = 1;               // 采样率低于 16kHz 的歌每个采样重复几次
static int down = 1;             // 高于 48kHz 的歌几个采样并成一个（1、2 或 4）
static bool music_on = false;    // 拿着喇叭
static uint32_t music_rate = 0;
static bool at_end = false;
static uint32_t last_save = 0;
static int bad_in_row = 0;       // 连着几首打不开
static bool song_silent = true;  // 这首打开后还一个采样都没解出来
static int silent_in_row = 0;    // 连着几首打开了却解不出声音（只有文件头的空文件等）

// 放的时候播放任务和扫描任务都要读卡：每次读之前拿锁
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

static uint32_t take_cmds() {
  portENTER_CRITICAL(&cmd_mux);
  uint32_t c = cmds;
  cmds = 0;
  portEXIT_CRITICAL(&cmd_mux);
  return c;
}

static void send(uint32_t cmd) {
  portENTER_CRITICAL(&cmd_mux);
  cmds |= cmd;
  portEXIT_CRITICAL(&cmd_mux);
  if (task) xTaskNotifyGive(task);
}

// 主循环和网页发来的命令：播放任务还没建时先建好（列表变了这种不用放歌的命令不建，等它建好后一起处理）
static void send_play(uint32_t cmd) {
  start_player_task();
  send(cmd);
}

static void *psram_calloc(size_t n, size_t size) {
  return heap_caps_calloc(n, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

static String full_path(const char *rel) { return String(kDir) + "/" + rel; }

// ---- 扫描 ----

// 目录缓存读进来（路径 -> 标签），拿着卡的锁调用
static int read_index_locked(TrackInfo *out, int max) {
  File f = SD_MMC.open(kIndex);
  if (!f) return 0;
  int n = 0;
  static char line[kTrackPathMax + kTagMax * 3 + 32];
  size_t len = 0;
  uint8_t buf[512];
  for (;;) {
    int got = f.read(buf, sizeof(buf));
    if (got <= 0) break;
    for (int i = 0; i < got && n < max; i++) {
      if (buf[i] == '\n') {
        if (track_index_parse(line, len, out[n])) n++;
        len = 0;
      } else if (len < sizeof(line) - 1) {
        line[len++] = (char)buf[i];
      }
    }
  }
  f.close();
  return n;
}

static void write_index(const Library &l) {
  if (!photos_card_lock()) {
    photos_card_unlock();
    return;
  }
  File f = SD_MMC.open(kIndexTmp, FILE_WRITE);
  bool ok = (bool)f;
  char line[kTrackPathMax + kTagMax * 3 + 32];
  for (int i = 0; ok && i < l.count; i++) {
    size_t n = track_index_line(l.tracks[i], line, sizeof(line));
    if (n) ok = f.write((const uint8_t *)line, n) == n;
  }
  if (f) f.close();
  if (ok) {
    SD_MMC.remove(kIndex);
    SD_MMC.rename(kIndexTmp, kIndex);
  }
  photos_card_unlock();
}

// 读一首的标签（没在缓存里或者文件变了时）
static void probe_track(const char *rel, uint32_t size, TrackInfo &t) {
  memset(&t, 0, sizeof(t));
  strlcpy(t.path, rel, sizeof(t.path));
  t.size = size;
  TrackProbe p;
  bool ok = false;
  if (photos_card_lock()) {
    File f = SD_MMC.open(full_path(rel));
    if (f) {
      ok = track_probe(file_read, &f, f.size(), track_format(rel), p);
      f.close();
    }
  }
  photos_card_unlock();
  if (ok) {
    strlcpy(t.title, p.title, sizeof(t.title));
    strlcpy(t.artist, p.artist, sizeof(t.artist));
    strlcpy(t.album, p.album, sizeof(t.album));
    t.dur_ms = p.dur_ms;
  }
  if (!t.title[0]) {
    char artist[kTagMax];
    track_name_from_file(rel, t.title, artist, sizeof(t.title));
    if (!t.artist[0]) strlcpy(t.artist, artist, sizeof(t.artist));
  }
}

// 列出 music 和第一级子文件夹里的歌（只要路径和大小，拿着卡的锁调用）
static int list_files_locked(TrackInfo *out, int max) {
  int n = 0;
  File dir = SD_MMC.open(kDir);
  if (!dir || !dir.isDirectory()) return -1;
  for (File f = dir.openNextFile(); f && n < max; f = dir.openNextFile()) {
    const char *name = f.name();
    const char *slash = strrchr(name, '/');  // 有的版本返回完整路径
    if (slash) name = slash + 1;
    if (name[0] == '.') continue;
    if (f.isDirectory()) {
      String sub = String(name);
      File d = SD_MMC.open(full_path(sub.c_str()));
      for (File g = d.openNextFile(); g && n < max; g = d.openNextFile()) {
        const char *gn = g.name();
        const char *gs = strrchr(gn, '/');
        if (gs) gn = gs + 1;
        if (g.isDirectory() || track_format(gn) == TrackFormat::None) continue;
        String rel = sub + "/" + gn;
        if (rel.length() >= kTrackPathMax) continue;
        strlcpy(out[n].path, rel.c_str(), kTrackPathMax);
        out[n].size = g.size();
        n++;
      }
      d.close();
    } else if (track_format(name) != TrackFormat::None && strlen(name) < kTrackPathMax) {
      strlcpy(out[n].path, name, kTrackPathMax);
      out[n].size = f.size();
      n++;
    }
  }
  dir.close();
  return n;
}

static void scan_locked() {
  Library &next = lib == &libs[0] ? libs[1] : libs[0];
  auto *cache = (TrackInfo *)psram_calloc(kPlayerMaxTracks, sizeof(TrackInfo));
  if (!cache) return;
  int found = -1, cached = 0;
  bool mounted = photos_card_lock();
  if (mounted) {
    if (!SD_MMC.exists(kDir)) SD_MMC.mkdir(kDir);
    found = list_files_locked(next.tracks, kPlayerMaxTracks);
    if (found >= 0) cached = read_index_locked(cache, kPlayerMaxTracks);
  }
  photos_card_unlock();
  bool changed = found != cached;
  for (int i = 0; i < found; i++) {
    TrackInfo &t = next.tracks[i];
    uint32_t size = t.size;
    const TrackInfo *hit = nullptr;
    for (int k = 0; k < cached && !hit; k++) {
      if (cache[k].size == size && strcmp(cache[k].path, t.path) == 0) hit = &cache[k];
    }
    if (hit) {
      t = *hit;
    } else {
      char rel[kTrackPathMax];
      strlcpy(rel, t.path, sizeof(rel));
      probe_track(rel, size, t);
      changed = true;
    }
  }
  free(cache);
  if (found < 0) found = 0;
  std::sort(next.tracks, next.tracks + found,
            [](const TrackInfo &a, const TrackInfo &b) { return strcmp(a.path, b.path) < 0; });
  // 歌单：第一级文件夹，按名字排
  strlcpy(next.folders[0], "全部", kTagMax);
  next.folder_count = 1;
  for (int i = 0; i < found; i++) {
    char f[kTagMax];
    track_folder(next.tracks[i].path, f, sizeof(f));
    next.folder_of[i] = 0;
    if (!f[0]) continue;
    int k = 1;
    while (k < next.folder_count && strcmp(next.folders[k], f) != 0) k++;
    if (k == next.folder_count) {
      if (k >= kPlayerMaxFolders) continue;  // 太多了，只在「全部」里
      strlcpy(next.folders[k], f, kTagMax);
      next.folder_count++;
    }
    next.folder_of[i] = (uint8_t)k;
  }
  next.count = found;
  PlayerCard result = !mounted ? PlayerCard::NoCard : found > 0 ? PlayerCard::Ok : PlayerCard::Empty;
  xSemaphoreTake(list_lock, portMAX_DELAY);
  lib = &next;
  card = result;
  version = version + 1;
  xSemaphoreGive(list_lock);
  if (changed && result == PlayerCard::Ok) write_index(next);
  send(kCmdListChanged);
}

static void scan() {
  xSemaphoreTake(scan_lock, portMAX_DELAY);
  scan_locked();
  xSemaphoreGive(scan_lock);
}

// 扫描任务要扫时才建，扫完自己退出，平时不占内部内存的栈
static portMUX_TYPE scan_mux = portMUX_INITIALIZER_UNLOCKED;
static bool scan_running = false;
static bool scan_again = false;

static void scan_task(void *) {
  for (;;) {
    scan();
    portENTER_CRITICAL(&scan_mux);
    if (!scan_again) {
      scan_running = false;
      scan_task_handle = nullptr;
      portEXIT_CRITICAL(&scan_mux);
      vTaskDelete(nullptr);
    }
    scan_again = false;
    portEXIT_CRITICAL(&scan_mux);
  }
}

// 列表里第 i 首在不在歌单 folder 里（拿着 list_lock 调用）
static bool in_folder(int i, int folder) { return folder == 0 || lib->folder_of[i] == folder; }

// 把歌单 folder 的歌的序号写进 ids，返回几首（拿着 list_lock 调用）
static int folder_ids(int folder, int *ids) {
  int n = 0;
  for (int i = 0; i < lib->count; i++) {
    if (in_folder(i, folder)) ids[n++] = i;
  }
  return n;
}

// 找路径，找不到返回 -1（拿着 list_lock 调用）
static int find_path(const char *path) {
  for (int i = 0; i < lib->count; i++) {
    if (strcmp(lib->tracks[i].path, path) == 0) return i;
  }
  return -1;
}

static int find_folder(const char *name) {
  if (!name[0]) return 0;
  for (int k = 1; k < lib->folder_count; k++) {
    if (strcmp(lib->folders[k], name) == 0) return k;
  }
  return -1;
}

static void rebuild_view() {
  xSemaphoreTake(list_lock, portMAX_DELAY);
  if (view_folder >= lib->folder_count) view_folder = 0;
  view_n = folder_ids(view_folder, view_ids);
  xSemaphoreGive(list_lock);
}

// ---- 记住放到哪里 ----

static void save_state() {
  if (!cur_path[0]) return;
  char buf[kTrackPathMax + kTagMax + 48];
  int n = snprintf(buf, sizeof(buf), "%s\n%u\n%d\n%s\n", cur_path, (unsigned)st.pos_ms, (int)order->mode(),
                   st.folder);
  if (n <= 0 || n >= (int)sizeof(buf)) return;
  if (photos_card_lock()) {
    File f = SD_MMC.open(kStateTmp, FILE_WRITE);
    bool ok = f && f.write((const uint8_t *)buf, n) == (size_t)n;
    if (f) f.close();
    if (ok) {
      SD_MMC.remove(kState);
      SD_MMC.rename(kStateTmp, kState);
    }
  }
  photos_card_unlock();
  last_save = millis();
}

// ---- 打开一首 ----

static void close_song() {
  dec.close();
  photos_card_lock();
  if (file) file.close();
  photos_card_unlock();
}

static void stop_music() {
  if (!music_on) return;
  audio_music_end();
  music_on = false;
}

// 封面：JPG 解到 kPlayerCover 见方（等比缩放，居中）。渐进式 JPG 解不了，就当没有
static bool decode_cover(const uint8_t *jpg, size_t len) {
  static LGFX_Sprite spr;
  if (!spr.getBuffer()) {
    spr.setPsram(true);
    spr.setColorDepth(16);
    if (!spr.createSprite(kPlayerCover, kPlayerCover)) return false;
  }
  spr.fillScreen(TFT_BLACK);
  if (!spr.drawJpg(jpg, len, 0, 0, kPlayerCover, kPlayerCover, 0, 0, 0.0f, 0.0f, lgfx::datum_t::middle_center))
    return false;
  auto *px = (const uint16_t *)spr.getBuffer();
  xSemaphoreTake(meta_lock, portMAX_DELAY);
  for (int i = 0; i < kPlayerCover * kPlayerCover; i++) cover_px[i] = __builtin_bswap16(px[i]);  // 精灵是 SPI 屏的字节序
  cover_ok = true;
  xSemaphoreGive(meta_lock);
  return true;
}

// 读封面和歌词（拿着卡的锁调用），封面的 JPG 放进 *jpg（PSRAM，用完 free）
static void load_extras_locked(const TrackProbe &p, uint8_t **jpg, size_t *jpg_len, uint8_t **lrc, size_t *lrc_len) {
  *jpg = nullptr;
  *lrc = nullptr;
  if (p.cover_len > 0 && p.cover_len <= kCoverMax) {
    *jpg = (uint8_t *)heap_caps_malloc(p.cover_len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (*jpg && file_read(&file, p.cover_off, *jpg, p.cover_len) == p.cover_len) {
      *jpg_len = p.cover_len;
    } else {
      free(*jpg);
      *jpg = nullptr;
    }
  }
  if (!*jpg) {
    // 歌里没有封面：看文件夹里有没有 cover.jpg、folder.jpg
    char folder[kTagMax];
    track_folder(cur_path, folder, sizeof(folder));
    static const char *const kNames[] = {"cover.jpg", "folder.jpg", "Cover.jpg", "Folder.jpg"};
    for (const char *name : kNames) {
      String path = folder[0] ? full_path(folder) + "/" + name : full_path(name);
      File f = SD_MMC.open(path);
      if (!f) continue;
      size_t n = f.size();
      if (n > 0 && n <= kCoverMax) {
        *jpg = (uint8_t *)heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (*jpg && file_read(&f, 0, *jpg, n) == n) {
          *jpg_len = n;
        } else {
          free(*jpg);
          *jpg = nullptr;
        }
      }
      f.close();
      if (*jpg) break;
    }
  }
  char lrc_path[kTrackPathMax + 8];
  if (track_lrc_path(cur_path, lrc_path, sizeof(lrc_path))) {
    File f = SD_MMC.open(full_path(lrc_path));
    if (f) {
      size_t n = f.size();
      if (n > 0 && n <= kLrcMax) {
        *lrc = (uint8_t *)heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (*lrc && file_read(&f, 0, *lrc, n) == n) {
          *lrc_len = n;
        } else {
          free(*lrc);
          *lrc = nullptr;
        }
      }
      f.close();
    }
  }
}

// 打开列表里的第 id 首，从 start_ms 开始。打不开时 st.error 说原因
static bool open_song(int id, uint32_t start_ms) {
  close_song();
  TrackInfo t;
  char folder[kTagMax] = "";
  xSemaphoreTake(list_lock, portMAX_DELAY);
  bool ok = id >= 0 && id < lib->count;
  if (ok) {
    t = lib->tracks[id];
    if (queue_folder > 0 && queue_folder < lib->folder_count) strlcpy(folder, lib->folders[queue_folder], kTagMax);
  }
  xSemaphoreGive(list_lock);
  if (!ok) return false;

  xSemaphoreTake(meta_lock, portMAX_DELAY);
  strlcpy(cur_path, t.path, sizeof(cur_path));
  strlcpy(st.title, t.title, sizeof(st.title));
  strlcpy(st.artist, t.artist, sizeof(st.artist));
  strlcpy(st.album, t.album, sizeof(st.album));
  strlcpy(st.folder, folder, sizeof(st.folder));
  st.dur_ms = t.dur_ms;
  st.pos_ms = start_ms;
  st.loaded = true;
  st.loading = true;
  st.error = nullptr;
  st.queue_pos = 0;
  for (int k = 0; k < queue_n; k++) {
    if (queue_ids[k] == id) st.queue_pos = k;
  }
  st.queue_len = queue_n;
  lyrics.count = 0;
  cover_ok = false;
  st.track_version++;
  xSemaphoreGive(meta_lock);

  TrackProbe p;
  uint8_t *jpg = nullptr, *lrc = nullptr;
  size_t jpg_len = 0, lrc_len = 0;
  ok = false;
  if (photos_card_lock()) {
    file = SD_MMC.open(full_path(t.path));
    if (file && track_probe(file_read, &file, file.size(), track_format(t.path), p)) {
      ok = dec.open(file_read, &file, file.size(), p);
      if (ok && start_ms > 0 && start_ms < p.dur_ms) dec.seek(start_ms);
      if (ok) load_extras_locked(p, &jpg, &jpg_len, &lrc, &lrc_len);
    }
  }
  photos_card_unlock();
  if (!ok) {
    close_song();
    st.error = "打不开这首歌，可能文件坏了或者格式不支持";
    st.loading = false;
    return false;
  }
  song_rate = dec.rate();
  // 4kHz 以下重复采样会超出 pcm 缓冲（最多放大 4 倍），192kHz 以上也不支持
  if (song_rate < kMinRate || song_rate > kMaxRate) {
    close_song();
    st.error = "这首歌的采样率不支持（要在 4kHz 到 192kHz 之间）";
    st.loading = false;
    return false;
  }
  down = 1;
  while (song_rate / down > 48000) down *= 2;
  uint32_t r = song_rate / down;
  up = 1;
  while (r * up < 16000) up++;
  io_rate = r * up;
  if (p.dur_ms) st.dur_ms = p.dur_ms;
  at_end = false;
  song_silent = true;
  if (lrc) {
    xSemaphoreTake(meta_lock, portMAX_DELAY);
    lrc_parse(lrc, lrc_len, lyrics);
    xSemaphoreGive(meta_lock);
    free(lrc);
  }
  if (jpg) {
    decode_cover(jpg, jpg_len);
    free(jpg);
  }
  return true;
}

// 把歌单 folder 设成正在放的，从里面的第 index 首开始（拿着 list_lock 调用）
static void set_queue_locked(int folder, int index, PlayMode mode) {
  queue_folder = folder;
  queue_n = folder_ids(folder, queue_ids);
  order->set(queue_n, index, mode, esp_random());
}

// 播放歌单里的下一首（顺序由 order 决定）
static void play_order(int pos_in_queue, bool autoplay) {
  if (pos_in_queue < 0 || pos_in_queue >= queue_n) return;
  int id = queue_ids[pos_in_queue];
  bool ok = open_song(id, 0);
  want_play = autoplay;
  if (ok) {
    bad_in_row = 0;
  } else if (autoplay && ++bad_in_row < queue_n && bad_in_row < 20) {
    vTaskDelay(pdMS_TO_TICKS(300));
    play_order(order->next(true), true);  // 跳过打不开的
    return;
  } else {
    want_play = false;
  }
  save_state();
}

// 开机后第一次扫完：恢复上次放到的地方（暂停着）
static void restore_state() {
  char buf[kTrackPathMax + kTagMax + 48] = "";
  if (photos_card_lock()) {
    File f = SD_MMC.open(kState);
    if (f) {
      int n = f.read((uint8_t *)buf, sizeof(buf) - 1);
      buf[n > 0 ? n : 0] = '\0';
      f.close();
    }
  }
  photos_card_unlock();
  char *lines[4] = {};
  char *save = nullptr;
  int k = 0;
  for (char *s = strtok_r(buf, "\n", &save); s && k < 4; s = strtok_r(nullptr, "\n", &save)) lines[k++] = s;
  // 文件夹那一行可能是空的：strtok 会跳过，所以按「有没有 4 行」分开处理
  const char *path = k >= 1 ? lines[0] : "";
  uint32_t pos = k >= 2 ? strtoul(lines[1], nullptr, 10) : 0;
  int mode = k >= 3 ? atoi(lines[2]) : 0;
  const char *folder = k >= 4 ? lines[3] : "";
  if (mode < 0 || mode > 2) mode = 0;
  xSemaphoreTake(list_lock, portMAX_DELAY);
  int id = path[0] ? find_path(path) : -1;
  int f = find_folder(folder);
  if (f < 0 || (id >= 0 && !in_folder(id, f))) f = 0;
  int index = 0;
  set_queue_locked(f, 0, (PlayMode)mode);
  for (int i = 0; i < queue_n; i++) {
    if (queue_ids[i] == id) index = i;
  }
  order->set(queue_n, index, (PlayMode)mode, esp_random());
  xSemaphoreGive(list_lock);
  st.mode = (PlayMode)mode;
  if (id >= 0 && open_song(id, pos)) return;
  // 没有记录或者那首没了：停在歌单第一首
  if (queue_n > 0) open_song(queue_ids[order->current()], 0);
}

// 扫描完列表换了：正在放的歌在新列表里是第几首，歌单重新算
static void on_list_changed() {
  static bool restored = false;
  if (card != PlayerCard::Ok) return;
  if (!cur_path[0]) {
    if (!restored) {
      restored = true;
      restore_state();
    } else {
      // 之前卡上没歌，现在传了：歌单设成「全部」，点播放就能放
      xSemaphoreTake(list_lock, portMAX_DELAY);
      set_queue_locked(0, 0, order->mode());
      xSemaphoreGive(list_lock);
    }
    return;
  }
  xSemaphoreTake(list_lock, portMAX_DELAY);
  int id = find_path(cur_path);
  int f = find_folder(st.folder);
  if (f < 0) f = 0;
  int index = 0;
  queue_folder = f;
  queue_n = folder_ids(f, queue_ids);
  for (int i = 0; i < queue_n; i++) {
    if (queue_ids[i] == id) index = i;
  }
  order->set(queue_n, index, order->mode(), esp_random());
  xSemaphoreGive(list_lock);
  st.queue_len = queue_n;
  st.queue_pos = index;
  if (id < 0) {
    // 正在放的歌被删了
    stop_music();
    close_song();
    want_play = false;
    xSemaphoreTake(meta_lock, portMAX_DELAY);
    cur_path[0] = '\0';
    st.loaded = false;
    st.track_version++;
    xSemaphoreGive(meta_lock);
    if (queue_n > 0) open_song(queue_ids[order->current()], 0);
  }
}

// ---- 播放任务 ----

static void handle_cmds(uint32_t c) {
  if (c & kCmdListChanged) on_list_changed();
  if (c & kCmdPlayView) {
    xSemaphoreTake(list_lock, portMAX_DELAY);
    int folder = cmd_view_folder < lib->folder_count ? cmd_view_folder : 0;
    set_queue_locked(folder, 0, order->mode());
    // 列表上的第几首就是歌单里的第几首（两边都按路径排）
    int index = cmd_view_index < queue_n ? cmd_view_index : 0;
    order->set(queue_n, index, order->mode(), esp_random());
    xSemaphoreGive(list_lock);
    if (music_on) audio_music_flush();
    play_order(order->current(), true);
  }
  if ((c & kCmdToggle) && !st.loaded && queue_n > 0) {
    play_order(order->current(), true);
    c &= ~kCmdToggle;
  }
  if (!st.loaded) return;
  if (c & kCmdToggle) {
    if (want_play) c |= kCmdPause;
    else c |= kCmdResume;
  }
  if (c & kCmdPause) {
    if (want_play) {
      want_play = false;
      stop_music();
      save_state();
    }
  } else if (c & kCmdResume) {
    if (st.error || at_end) {
      play_order(order->current(), true);  // 打不开的、放完了的从头再来
    } else {
      want_play = true;
    }
  }
  if (c & kCmdNext) {
    if (music_on) audio_music_flush();
    play_order(order->next(true), want_play);
  } else if (c & kCmdPrev) {
    if (music_on) audio_music_flush();
    if (st.pos_ms > kRestartPrevMs && !st.error && dec.is_open()) {
      photos_card_lock();
      dec.seek(0);
      photos_card_unlock();
      st.pos_ms = 0;
      at_end = false;
    } else {
      play_order(order->prev(), want_play);
    }
  }
  if ((c & kCmdSeek) && dec.is_open()) {
    uint32_t ms = cmd_seek_ms;
    if (st.dur_ms && ms >= st.dur_ms) ms = st.dur_ms > 1000 ? st.dur_ms - 1000 : 0;
    if (music_on) audio_music_flush();
    photos_card_lock();
    dec.seek(ms);
    photos_card_unlock();
    st.pos_ms = ms;
    at_end = false;
  }
  if (c & kCmdMode) {
    PlayMode m = order->mode() == PlayMode::Loop ? PlayMode::Shuffle
                 : order->mode() == PlayMode::Shuffle ? PlayMode::One
                                                      : PlayMode::Loop;
    order->set_mode(m, esp_random());
    st.mode = m;
    save_state();
  }
}

// 睡眠定时的音量（0～256）：到点前慢慢变小，到点暂停
static int sleep_gain() {
  if (!sleep_until) return 256;
  int32_t left = (int32_t)(sleep_until - millis());
  if (left <= 0) return 0;
  return left >= (int32_t)kFadeMs ? 256 : (int)(left * 256 / kFadeMs);
}

static int16_t *pcm;  // 解出来的声音（kChunk * 4 个采样，放在 PSRAM）

static void player_task(void *) {
  for (;;) {
    // 睡眠定时到了：暂停（暂停着也照样倒计时）
    uint32_t until = sleep_until;
    if (until && (int32_t)(millis() - until) >= 0) {
      sleep_until = 0;
      sleep_minutes = 0;
      if (want_play) {
        want_play = false;
        save_state();
      }
    }
    until = sleep_until;
    st.sleep_left_s = until ? (int)((until - millis()) / 1000) + 1 : 0;
    bool playing = want_play && st.loaded && !st.error && !at_end;
    if (!playing) {
      stop_music();
      st.playing = false;
      ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));
      handle_cmds(take_cmds());
      continue;
    }
    uint32_t c = take_cmds();
    if (c) {
      handle_cmds(c);
      continue;
    }
    // AI 聊天要回答、影片要放声音：让出喇叭，自己暂停
    if (audio_music_wanted()) {
      want_play = false;
      yielded = true;
      stop_music();
      save_state();
      continue;
    }
    if (!music_on || music_rate != io_rate) {
      stop_music();
      if (!audio_music_begin(io_rate)) {
        vTaskDelay(pdMS_TO_TICKS(200));  // 喇叭正被语音回答占着，等一下再试
        continue;
      }
      music_on = true;
      music_rate = io_rate;
    }
    size_t pending = audio_music_pending();
    st.playing = true;
    uint32_t dpos = dec.pos_ms();
    uint32_t lag = (uint32_t)((uint64_t)pending * 1000 / io_rate);
    st.pos_ms = dpos > lag ? dpos - lag : 0;
    if (millis() - last_save >= kSaveEveryMs) save_state();
    if (pending > io_rate * kAheadMs / 1000) {
      ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(20));
      continue;
    }
    photos_card_lock();
    size_t n = dec.read(pcm, kChunk);
    photos_card_unlock();
    if (n == 0) {
      if (song_silent) {
        // 一个声音都解不出来：当成坏文件跳过，不然单曲循环或只有一首时会不停重开它
        if (++silent_in_row >= queue_n || silent_in_row >= 20) {
          silent_in_row = 0;
          want_play = false;
          close_song();
          st.error = "这首歌没有声音，可能文件坏了";
          st.loading = false;
          continue;
        }
        vTaskDelay(pdMS_TO_TICKS(300));
        play_order(order->next(true), true);
        continue;
      }
      // 这首放完了：单曲循环再放一遍，否则下一首（缓冲里剩的声音照常放完，几乎没有空隙）
      st.pos_ms = st.dur_ms;
      int next = order->next(false);
      play_order(next, true);
      continue;
    }
    song_silent = false;
    silent_in_row = 0;
    st.loading = false;
    if (down > 1) {
      for (size_t i = 0; i < n / down; i++) {
        int32_t sum = 0;
        for (int k = 0; k < down; k++) sum += pcm[i * down + k];
        pcm[i] = (int16_t)(sum / down);
      }
      n /= down;
    }
    if (up > 1) {
      for (size_t i = n; i-- > 0;) {
        for (int k = 0; k < up; k++) pcm[i * up + k] = pcm[i];
      }
      n *= up;
    }
    int g = sleep_gain();
    if (g < 256) {
      for (size_t i = 0; i < n; i++) pcm[i] = (int16_t)(pcm[i] * g / 256);
    }
    audio_music_write(pcm, n);
  }
}

void player_begin() {
  if (!photos_supported() || !audio_available()) return;
  list_lock = xSemaphoreCreateMutex();
  meta_lock = xSemaphoreCreateMutex();
  scan_lock = xSemaphoreCreateMutex();
  for (auto &l : libs) {
    l.tracks = (TrackInfo *)psram_calloc(kPlayerMaxTracks, sizeof(TrackInfo));
    l.folder_of = (uint8_t *)psram_calloc(kPlayerMaxTracks, 1);
    l.folders = (char (*)[kTagMax])psram_calloc(kPlayerMaxFolders, kTagMax);
    if (l.folders) strlcpy(l.folders[0], "全部", kTagMax);
    l.folder_count = 1;
  }
  pcm = (int16_t *)psram_calloc(kChunk * 4, sizeof(int16_t));
  view_ids = (int *)psram_calloc(kPlayerMaxTracks, sizeof(int));
  queue_ids = (int *)psram_calloc(kPlayerMaxTracks, sizeof(int));
  order = (PlayOrder *)psram_calloc(1, sizeof(PlayOrder));
  cover_px = (uint16_t *)psram_calloc(kPlayerCover * kPlayerCover, sizeof(uint16_t));
  lyrics.lines = (LrcLine *)psram_calloc(kLrcLines, sizeof(LrcLine));
  lyrics.max_lines = kLrcLines;
  lyrics.pool = (char *)psram_calloc(kLrcPool, 1);
  lyrics.pool_size = kLrcPool;
  if (!libs[0].tracks || !libs[1].tracks || !libs[0].folder_of || !libs[1].folder_of || !libs[0].folders ||
      !libs[1].folders || !view_ids || !queue_ids || !order || !cover_px || !lyrics.lines || !lyrics.pool || !pcm) {
    log_e("player: no memory");
    return;
  }
  new (order) PlayOrder();
  st.mode = PlayMode::Loop;
  ready = true;
  player_rescan();
}

static void start_player_task() {
  if (!ready || task) return;  // 只在主循环里调用（按钮、网页），不会同时建两个
  // 刚建好的任务先看一遍列表（还没建时扫完的那几次没发给它），再处理命令
  portENTER_CRITICAL(&cmd_mux);
  cmds |= kCmdListChanged;
  portEXIT_CRITICAL(&cmd_mux);
  // 解码在 1 号核（界面那个核，0 号核上有 WiFi 和唤醒词），比界面优先级高一点，放歌不卡；
  // 栈要装下 dr_flac 打开文件时的 5KB 左右
  if (xTaskCreatePinnedToCore(player_task, "player", 10240, nullptr, 3, &task, 1) != pdPASS) {
    task = nullptr;
    log_e("player: no memory for task");
  }
}

bool player_supported() { return ready; }

void player_rescan() {
  if (!ready) return;
  portENTER_CRITICAL(&scan_mux);
  if (scan_running) {
    scan_again = true;
    portEXIT_CRITICAL(&scan_mux);
    return;
  }
  scan_running = true;
  scan_again = false;
  portEXIT_CRITICAL(&scan_mux);
  if (xTaskCreatePinnedToCore(scan_task, "music_scan", 6144, nullptr, 1, &scan_task_handle, 0) != pdPASS) {
    portENTER_CRITICAL(&scan_mux);
    scan_running = false;
    portEXIT_CRITICAL(&scan_mux);
  }
}

PlayerCard player_card() { return ready ? card : PlayerCard::NoSlot; }

uint32_t player_list_version() { return version; }

int player_folder_count() {
  if (!ready) return 0;
  xSemaphoreTake(list_lock, portMAX_DELAY);
  int n = lib->folder_count;
  xSemaphoreGive(list_lock);
  return n;
}

bool player_folder_name(int i, char *out, size_t cap) {
  if (!ready) return false;
  xSemaphoreTake(list_lock, portMAX_DELAY);
  bool ok = i >= 0 && i < lib->folder_count;
  if (ok) strlcpy(out, lib->folders[i], cap);
  xSemaphoreGive(list_lock);
  return ok;
}

void player_set_view(int folder) {
  if (!ready) return;
  view_folder = folder;
  rebuild_view();
}

int player_view() { return view_folder; }

int player_view_count() {
  if (!ready) return 0;
  rebuild_view();  // 列表可能刚扫完换了
  return view_n;
}

bool player_view_track(int i, TrackInfo &out) {
  if (!ready) return false;
  xSemaphoreTake(list_lock, portMAX_DELAY);
  bool ok = i >= 0 && i < view_n && view_ids[i] < lib->count;
  if (ok) out = lib->tracks[view_ids[i]];
  xSemaphoreGive(list_lock);
  return ok;
}

int player_view_playing() {
  if (!ready || !st.loaded) return -1;
  char path[kTrackPathMax];
  xSemaphoreTake(meta_lock, portMAX_DELAY);
  strlcpy(path, cur_path, sizeof(path));
  xSemaphoreGive(meta_lock);
  xSemaphoreTake(list_lock, portMAX_DELAY);
  int found = -1;
  for (int i = 0; i < view_n && found < 0; i++) {
    if (view_ids[i] < lib->count && strcmp(lib->tracks[view_ids[i]].path, path) == 0) found = i;
  }
  xSemaphoreGive(list_lock);
  return found;
}

void player_play_view(int i) {
  portENTER_CRITICAL(&cmd_mux);
  cmd_view_folder = view_folder;
  cmd_view_index = i;
  portEXIT_CRITICAL(&cmd_mux);
  send_play(kCmdPlayView);
}

// 按屏幕上看到的状态决定暂停还是播放（和各处的播放键图标一致），后按的覆盖先按的：
// 播放任务还没来得及处理时连点、或者在音乐页、时钟页、快捷设置几处先后点，结果都是最后一下
void player_toggle() {
  if (!ready || !st.loaded) {
    send_play(kCmdToggle);  // 还没有当前的歌：接着上次放到的地方放
    return;
  }
  portENTER_CRITICAL(&cmd_mux);
  uint32_t pending = cmds & (kCmdPause | kCmdResume);
  cmds &= ~(kCmdToggle | kCmdPause | kCmdResume);
  portEXIT_CRITICAL(&cmd_mux);
  bool pause = pending ? (pending & kCmdResume) != 0 : player_playing();
  if (pause) send(kCmdPause);
  else send_play(kCmdResume);
}
void player_pause() {
  portENTER_CRITICAL(&cmd_mux);
  bool resuming = cmds & kCmdResume;
  cmds &= ~(kCmdToggle | kCmdResume);  // 还没处理的「播放」作废，暂停优先
  portEXIT_CRITICAL(&cmd_mux);
  if (want_play || resuming) send(kCmdPause);
}
void player_resume() { send_play(kCmdResume); }
void player_next() { send_play(kCmdNext); }
void player_prev() { send_play(kCmdPrev); }

void player_seek_to(uint32_t ms) {
  portENTER_CRITICAL(&cmd_mux);
  cmd_seek_ms = ms;
  portEXIT_CRITICAL(&cmd_mux);
  send_play(kCmdSeek);
}

void player_cycle_mode() { send_play(kCmdMode); }

void player_cycle_sleep() {
  start_player_task();  // 倒计时在播放任务里走
  int m = sleep_next_choice(sleep_minutes);
  sleep_minutes = m;
  uint32_t until = m ? millis() + (uint32_t)m * 60000 : 0;
  sleep_until = until == 0 && m ? 1 : until;
  st.sleep_left_s = m * 60;
  if (task) xTaskNotifyGive(task);
}

PlayerStatus player_status() {
  PlayerStatus s = {};
  if (!ready) return s;
  xSemaphoreTake(meta_lock, portMAX_DELAY);
  s = st;
  xSemaphoreGive(meta_lock);
  s.playing = want_play && st.loaded && !st.error;
  return s;
}

bool player_playing() { return ready && want_play && st.loaded && !st.error; }

bool player_cover(uint16_t *px) {
  if (!ready) return false;
  xSemaphoreTake(meta_lock, portMAX_DELAY);
  bool ok = cover_ok;
  if (ok) memcpy(px, cover_px, kPlayerCover * kPlayerCover * sizeof(uint16_t));
  xSemaphoreGive(meta_lock);
  return ok;
}

bool player_lyrics(char *cur, size_t cur_cap, char *next, size_t next_cap) {
  if (!ready) return false;
  xSemaphoreTake(meta_lock, portMAX_DELAY);
  bool ok = lyrics.count > 0;
  if (ok) {
    int i = lrc_find(lyrics, st.pos_ms);
    strlcpy(cur, i >= 0 ? lrc_text(lyrics, i) : "", cur_cap);
    // 下一句：跳过和当前句时间一样的、空的
    int j = i + 1;
    while (j < lyrics.count && !lrc_text(lyrics, j)[0]) j++;
    strlcpy(next, j < lyrics.count ? lrc_text(lyrics, j) : "", next_cap);
  }
  xSemaphoreGive(meta_lock);
  return ok;
}

bool player_take_yielded() {
  bool y = yielded;
  yielded = false;
  return y;
}

// ---- 设置网页 ----

static void json_str(String &out, const char *s) {
  out += '"';
  for (; *s; s++) {
    char c = *s;
    if (c == '"' || c == '\\') {
      out += '\\';
      out += c;
    } else if ((uint8_t)c < 0x20) {
      char b[8];
      snprintf(b, sizeof(b), "\\u%04x", c);
      out += b;
    } else {
      out += c;
    }
  }
  out += '"';
}

void player_list_json(String &out) {
  out = "";
  if (!ready) {
    out = "{\"card\":\"noslot\"}";
    return;
  }
  scan();  // 刚用读卡器拷进去的歌也能看到
  uint64_t total = 0, used = 0;
  bool ok = photos_card_lock();
  if (ok) {
    total = SD_MMC.totalBytes();
    used = SD_MMC.usedBytes();
  }
  photos_card_unlock();
  xSemaphoreTake(list_lock, portMAX_DELAY);
  out.reserve(128 + lib->count * 160);
  out += "{\"card\":\"";
  out += card == PlayerCard::NoCard ? "none" : "ok";
  out += "\"";
  if (ok) {
    char b[64];
    snprintf(b, sizeof(b), ",\"total\":%llu,\"used\":%llu", (unsigned long long)total, (unsigned long long)used);
    out += b;
  }
  out += ",\"folders\":[";
  for (int k = 1; k < lib->folder_count; k++) {
    if (k > 1) out += ',';
    json_str(out, lib->folders[k]);
  }
  out += "],\"tracks\":[";
  for (int i = 0; i < lib->count; i++) {
    const TrackInfo &t = lib->tracks[i];
    if (i) out += ',';
    out += "{\"p\":";
    json_str(out, t.path);
    out += ",\"t\":";
    json_str(out, t.title);
    out += ",\"a\":";
    json_str(out, t.artist);
    char b[48];
    snprintf(b, sizeof(b), ",\"s\":%u,\"d\":%u}", (unsigned)t.size, (unsigned)(t.dur_ms / 1000));
    out += b;
  }
  out += "]}";
  xSemaphoreGive(list_lock);
}

static constexpr const char *kUploadTmp = "/music/.upload.tmp";
static constexpr size_t kUploadBuf = 32 * 1024;  // 攒够一大块再写卡，比每次写 1.4KB 快得多
static constexpr uint64_t kUploadSpare = 1024 * 1024;
static File upload_file;
static uint8_t *upload_buf;
static size_t upload_fill = 0;
static bool upload_ok = false;
static const char *upload_err = "";
static char upload_folder[kTagMax];

static bool upload_fail(const char *why) {
  player_upload_abort();
  upload_err = why;
  return false;
}

static bool upload_flush() {
  if (upload_fill == 0) return true;
  photos_card_lock();
  bool ok = upload_file && upload_file.write(upload_buf, upload_fill) == upload_fill;
  photos_card_unlock();
  upload_fill = 0;
  return ok || upload_fail("写入 TF 卡失败，可能是卡被拔了");
}

bool player_upload_begin(const char *folder, uint64_t size) {
  upload_ok = false;
  upload_fill = 0;
  upload_folder[0] = '\0';
  if (!ready) {
    upload_err = "这块板子没有 TF 卡槽";
    return false;
  }
  if (folder && folder[0] && !track_safe_folder(folder, upload_folder, sizeof(upload_folder))) {
    upload_err = "歌单名字不对";
    return false;
  }
  if (size >= 0xFFFFFFFFull) {
    upload_err = "文件太大";
    return false;
  }
  if (!upload_buf) upload_buf = (uint8_t *)heap_caps_malloc(kUploadBuf, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!upload_buf) {
    upload_err = "板子内存不够";
    return false;
  }
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

bool player_upload_write(const uint8_t *data, size_t len) {
  while (upload_ok && len > 0) {
    size_t n = std::min(len, kUploadBuf - upload_fill);
    memcpy(upload_buf + upload_fill, data, n);
    upload_fill += n;
    data += n;
    len -= n;
    if (upload_fill == kUploadBuf) upload_flush();
  }
  return upload_ok;
}

bool player_upload_end(const char *name, char *saved, size_t saved_size) {
  char base[kTrackPathMax - kTagMax - 8];
  if (upload_ok && !track_safe_name(name, base, sizeof(base))) upload_fail("只能传 MP3、FLAC、WAV 和 .lrc 歌词");
  if (upload_ok) upload_flush();
  if (!upload_ok) return false;
  String dir = upload_folder[0] ? full_path(upload_folder) : String(kDir);
  String rel = upload_folder[0] ? String(upload_folder) + "/" + base : String(base);
  bool ok = photos_card_lock();
  upload_file.close();
  if (ok) {
    if (!SD_MMC.exists(dir)) SD_MMC.mkdir(dir);
    // 同名的直接替换（重新传一遍改好的歌、歌词）
    if (SD_MMC.exists(full_path(rel.c_str()))) SD_MMC.remove(full_path(rel.c_str()));
    ok = SD_MMC.rename(kUploadTmp, full_path(rel.c_str()));
    if (!ok) SD_MMC.remove(kUploadTmp);
  }
  photos_card_unlock();
  upload_ok = false;
  if (!ok) {
    upload_err = "写入 TF 卡失败";
    return false;
  }
  strlcpy(saved, rel.c_str(), saved_size);
  player_rescan();
  return true;
}

void player_upload_abort() {
  bool had = upload_file;
  photos_card_lock();
  if (upload_file) upload_file.close();
  if (had) SD_MMC.remove(kUploadTmp);
  photos_card_unlock();
  upload_ok = false;
  upload_fill = 0;
}

const char *player_upload_error() { return upload_err; }

// 路径要在 music 里面：不能有 ..、反斜杠，最多一层文件夹
static bool safe_rel(const char *path) {
  if (!path || !path[0] || path[0] == '/' || strstr(path, "..") || strchr(path, '\\')) return false;
  const char *slash = strchr(path, '/');
  return !slash || !strchr(slash + 1, '/');
}

bool player_delete(const char *path) {
  if (!ready || !safe_rel(path) || track_format(path) == TrackFormat::None) return false;
  bool playing_it = false;
  xSemaphoreTake(meta_lock, portMAX_DELAY);
  playing_it = strcmp(cur_path, path) == 0;
  xSemaphoreGive(meta_lock);
  if (playing_it) {
    send(kCmdPause);
    for (int i = 0; i < 100 && music_on; i++) delay(10);
  }
  bool ok = photos_card_lock();
  if (ok) {
    if (playing_it) {
      dec.close();
      if (file) file.close();
    }
    ok = SD_MMC.remove(full_path(path));
    char lrc[kTrackPathMax + 8];
    if (ok && track_lrc_path(path, lrc, sizeof(lrc))) SD_MMC.remove(full_path(lrc));
  }
  photos_card_unlock();
  player_rescan();
  return ok;
}

bool player_mkdir(const char *name) {
  char folder[kTagMax];
  if (!ready || !track_safe_folder(name, folder, sizeof(folder))) return false;
  bool ok = photos_card_lock();
  if (ok) {
    if (!SD_MMC.exists(kDir)) SD_MMC.mkdir(kDir);
    ok = SD_MMC.exists(full_path(folder)) || SD_MMC.mkdir(full_path(folder));
  }
  photos_card_unlock();
  return ok;
}

#else  // 没有 TF 卡槽或者喇叭的板子

#include <Arduino.h>

void player_begin() {}
bool player_supported() { return false; }
void player_rescan() {}
PlayerCard player_card() { return PlayerCard::NoSlot; }
uint32_t player_list_version() { return 0; }
int player_folder_count() { return 0; }
bool player_folder_name(int, char *, size_t) { return false; }
void player_set_view(int) {}
int player_view() { return 0; }
int player_view_count() { return 0; }
bool player_view_track(int, TrackInfo &) { return false; }
int player_view_playing() { return -1; }
void player_play_view(int) {}
void player_toggle() {}
void player_pause() {}
void player_resume() {}
void player_next() {}
void player_prev() {}
void player_seek_to(uint32_t) {}
void player_cycle_mode() {}
void player_cycle_sleep() {}
PlayerStatus player_status() { return {}; }
bool player_playing() { return false; }
bool player_cover(uint16_t *) { return false; }
bool player_lyrics(char *, size_t, char *, size_t) { return false; }
bool player_take_yielded() { return false; }
void player_list_json(String &out) { out = "{\"card\":\"noslot\"}"; }
bool player_upload_begin(const char *, uint64_t) { return false; }
bool player_upload_write(const uint8_t *, size_t) { return false; }
bool player_upload_end(const char *, char *, size_t) { return false; }
void player_upload_abort() {}
const char *player_upload_error() { return "这块板子没有 TF 卡槽"; }
bool player_delete(const char *) { return false; }
bool player_mkdir(const char *) { return false; }

#endif
