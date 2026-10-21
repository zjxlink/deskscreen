#pragma once

#include <stdint.h>

// 影片页：放 TF 卡 movies 文件夹里的 MJPEG 视频（.avi，电脑上用 tools/movie 里的工具转好），带声音。
// 可以暂停、快进快退、拖进度条，记住每部片子看到哪里（存在卡上的 movies/.progress）。
// 读卡、解码和放声音都在后台任务里做，这里的函数只是发命令、取状态，主循环里随便调

static constexpr int kMovieMax = 100;          // 列表里最多这么多部
static constexpr int kMovieNameMax = 96;       // 文件名（UTF-8）最长这么多字节，再长的不认

enum class MovieCard : uint8_t {
  NoSlot,   // 这块板子没有 TF 卡槽
  Reading,  // 正在读卡
  NoCard,   // 没插卡、卡坏了或者不是 FAT32
  Empty,    // 有卡，movies 文件夹里没有 .avi
  Ok,
};

struct MovieInfo {
  char name[kMovieNameMax];
  uint32_t dur_ms;   // 时长，读不出来时为 0
  uint32_t pos_ms;   // 上次看到哪里
};

struct MovieStatus {
  bool open;         // 在放（或者暂停在）某一部片子
  bool paused;
  bool loading;      // 刚打开或者刚跳过去，画面还没出来
  bool ended;        // 放完了，停在最后一帧
  bool sound;        // 这部片子有能放的声音
  uint32_t pos_ms;
  uint32_t dur_ms;
  int index;         // 列表里的第几部
  const char *error; // 打不开时的原因，否则为 nullptr
};

void movie_begin();
bool movie_supported();

// 进影片页时调用：重新看一眼 movies 文件夹（插拔卡、拷进新片子都能跟上）
void movie_rescan();
MovieCard movie_card();
// 列表有变化时版本号加一，界面据此刷新
uint32_t movie_list_version();
int movie_count();
// 第 i 部的信息，复制一份出来（列表在后台任务里会变）
bool movie_info(int i, MovieInfo &out);

// 从上次看到的地方开始放第 i 部（看完了的从头放）
void movie_play(int i);
void movie_toggle_pause();
void movie_pause();               // 离开影片页时调用，在放就暂停
void movie_seek_by(int32_t ms);   // 快进快退
void movie_seek_to(uint32_t ms);
void movie_close();               // 回到列表，记下看到哪里
MovieStatus movie_status();
// 正在放（没暂停），用来不让屏幕熄灭、不响整点报时
bool movie_playing();

// 有新解码好的一帧时返回 true，px 是 320x240 的 RGB565，一直有效到下一次返回 true 后又过了一帧
bool movie_take_frame(const uint16_t *&px);
