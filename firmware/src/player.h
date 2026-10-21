#pragma once

#include <stddef.h>
#include <stdint.h>

#include "track.h"

class String;

// 音乐播放器：放 TF 卡 music 文件夹里的 MP3、FLAC、WAV，第一级子文件夹是歌单。
// 歌名、歌手、封面从歌曲标签里读，同名 .lrc 是歌词。离开音乐页也接着放（后台播放），
// 记住放到哪一首、哪一秒（存在卡上的 music/.state），开机不自动放。
// 读卡、解码都在后台任务里做，这里的函数只是发命令、取状态，主循环里随便调

static constexpr int kPlayerMaxTracks = 1000;  // 最多认这么多首
static constexpr int kPlayerMaxFolders = 24;   // 「全部」加上最多 23 个歌单
static constexpr int kPlayerCover = 92;        // 封面边长（像素）

enum class PlayerCard : uint8_t {
  NoSlot,   // 这块板子没有 TF 卡槽或者喇叭
  Reading,  // 正在读卡
  NoCard,   // 没插卡、卡坏了或者不是 FAT32
  Empty,    // 有卡，music 文件夹里没有歌
  Ok,
};

struct PlayerStatus {
  bool loaded;    // 有一首当前的歌（在放或者暂停着）
  bool playing;   // 在放（没暂停）
  bool loading;   // 刚打开或者刚跳过去，还没出声
  uint32_t pos_ms;
  uint32_t dur_ms;
  char title[kTagMax];
  char artist[kTagMax];
  char album[kTagMax];
  char folder[kTagMax];  // 正在放的歌单，「全部」时为空
  int queue_pos;         // 正在放歌单里的第几首（从 0 开始）
  int queue_len;
  PlayMode mode;
  uint32_t track_version;  // 换歌时加一，界面据此换封面、歌词
  int sleep_left_s;        // 睡眠定时还剩几秒，0 表示没开
  const char *error;       // 打不开时的原因，否则为 nullptr
};

void player_begin();
bool player_supported();

// 进音乐页、插拔卡、传了新歌后调用：重新看一眼 music 文件夹（标签有缓存，第二次很快）
void player_rescan();
PlayerCard player_card();
// 列表有变化时版本号加一，界面据此刷新
uint32_t player_list_version();

// 歌单：0 是「全部」，后面是 music 下的文件夹（按名字排）
int player_folder_count();
bool player_folder_name(int i, char *out, size_t cap);
// 列表这一屏显示哪个歌单（不影响正在放的）
void player_set_view(int folder);
int player_view();
int player_view_count();
bool player_view_track(int i, TrackInfo &out);
// 正在放的那首在这个歌单里是第几首，不在时返回 -1
int player_view_playing();

// 从列表里点了一首：从这首开始放这个歌单
void player_play_view(int i);
// 播放/暂停。还没选过歌时从记住的那首（或者第一首）开始
void player_toggle();
void player_pause();
void player_resume();
void player_next();
void player_prev();
void player_seek_to(uint32_t ms);
// 换播放顺序：列表循环 → 随机 → 单曲循环
void player_cycle_mode();
// 睡眠定时：关 → 15 → 30 → 60 分钟 → 关；到时间前 10 秒慢慢变小，然后暂停
void player_cycle_sleep();

PlayerStatus player_status();
// 在放（没暂停），用来让小橘跟着跳、不熄屏这些
bool player_playing();
// 这首歌的封面（kPlayerCover x kPlayerCover 的 RGB565，LVGL 的字节序）复制到 px，没有封面时返回 false
bool player_cover(uint16_t *px);
// 现在该显示的两行歌词（当前句、下一句），没有歌词时返回 false
bool player_lyrics(char *cur, size_t cur_cap, char *next, size_t next_cap);
// 因为别人要用喇叭（AI 聊天回答、影片）自己暂停了：返回一次 true
bool player_take_yielded();

// ---- 设置网页：列表、上传、删除 ----

// 歌曲列表（JSON）：卡的状态、空间，每首的路径、大小、时长、歌名、歌手，以及有哪些文件夹
void player_list_json(String &out);
// 上传一首歌（或者 .lrc 歌词）到 folder 文件夹（空字符串表示直接放在 music 下）。
// 和固件上传一样分三步：begin 时知道总大小，write 一块一块写，end 时给文件名
bool player_upload_begin(const char *folder, uint64_t size);
bool player_upload_write(const uint8_t *data, size_t len);
bool player_upload_end(const char *name, char *saved, size_t saved_size);
void player_upload_abort();
const char *player_upload_error();
// 删掉一首歌（path 是相对 music 的路径），同名歌词也删
bool player_delete(const char *path);
// 新建歌单文件夹
bool player_mkdir(const char *name);
