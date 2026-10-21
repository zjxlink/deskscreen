#pragma once

#include <stddef.h>
#include <stdint.h>

// 音乐播放器里和硬件无关的部分：认文件格式、读歌曲标签（歌名、歌手、专辑、封面、时长）、
// 解析 .lrc 歌词、歌曲目录缓存的一行、播放顺序。可在电脑上测试

enum class TrackFormat : uint8_t { None, Mp3, Flac, Wav };

// 按扩展名认格式：.mp3 .flac .wav（大小写都行）。隐藏文件（. 开头，包括 macOS 的 ._ 文件）不算
TrackFormat track_format(const char *name);
// 同名歌词文件：把 path 的扩展名换成 .lrc，放不下时返回 false
bool track_lrc_path(const char *path, char *out, size_t cap);

static constexpr size_t kTagMax = 64;  // 歌名、歌手、专辑最长这么多字节（UTF-8）

// 读文件的回调：从 off 开始读 len 字节，返回读到多少
using TrackRead = size_t (*)(void *ctx, uint32_t off, uint8_t *buf, size_t len);

struct TrackProbe {
  TrackFormat format;
  char title[kTagMax];
  char artist[kTagMax];
  char album[kTagMax];
  uint32_t rate;       // 采样率
  uint8_t channels;
  uint8_t bits;        // WAV 的位深（16 或 24），别的格式不用
  uint32_t dur_ms;     // 时长，算不出来时为 0
  uint32_t data_off;   // 声音数据从哪里开始（MP3 跳过 ID3 标签；WAV 是 data 块）
  uint32_t data_len;
  uint32_t cover_off;  // 嵌在文件里的封面（JPG），没有时 cover_len 为 0
  uint32_t cover_len;
  // MP3：每帧多少采样、第一帧多少字节（Xing 头那一帧不是声音）、VBR 的目录（定位用，100 个百分点）
  uint16_t mp3_frame_samples;
  bool mp3_has_toc;
  uint8_t mp3_toc[100];
  uint32_t mp3_bitrate;  // 第一帧的码率（bps），CBR 定位用
};

// 读出文件的格式信息和标签。文件太坏、不是这种格式时返回 false
bool track_probe(TrackRead read, void *ctx, uint32_t size, TrackFormat format, TrackProbe &out);

// MP3 跳到第 ms 毫秒大约是文件里的第几个字节（VBR 用目录，CBR 按码率算）
uint32_t track_mp3_seek_offset(const TrackProbe &p, uint32_t ms);

// 网页上传时的文件名整理成 FAT 能存的：去掉 \ / : * ? " < > | 和控制字符、开头的点和空格，
// 太长时截短（不切断一个字）。扩展名必须是 .mp3 .flac .wav .lrc（改成小写），否则返回 false
bool track_safe_name(const char *in, char *out, size_t cap);
// 歌单文件夹名，规则同上（不要扩展名）；空的或者全被去掉了返回 false
bool track_safe_folder(const char *in, char *out, size_t cap);

// 文件名当歌名：去掉文件夹和扩展名；「歌手 - 歌名」这样的拆开（标签里没有时用）
void track_name_from_file(const char *path, char *title, char *artist, size_t cap);

// ---- 歌词 ----

struct LrcLine {
  uint32_t ms;
  uint16_t text;  // 在文字池里的位置
};

struct Lyrics {
  LrcLine *lines;
  int max_lines;
  char *pool;
  size_t pool_size;
  int count;
};

// 解析 .lrc（UTF-8、GBK、UTF-16 都行）：每行 [mm:ss.xx]歌词，一行可以有好几个时间。
// 认 [offset:毫秒]，别的标签（[ti:] [ar:] 这些）跳过。结果按时间排好，空行也留着（表示间奏）
void lrc_parse(const uint8_t *data, size_t len, Lyrics &out);
// 放到 ms 毫秒时该显示第几行，还没到第一行时返回 -1
int lrc_find(const Lyrics &l, uint32_t ms);
inline const char *lrc_text(const Lyrics &l, int i) { return l.pool + l.lines[i].text; }

// ---- 歌曲目录 ----

static constexpr size_t kTrackPathMax = 128;  // 相对 music 文件夹的路径，如「华语/晴天.mp3」

struct TrackInfo {
  char path[kTrackPathMax];
  char title[kTagMax];
  char artist[kTagMax];
  char album[kTagMax];
  uint32_t size;    // 文件大小，变了就重新读标签
  uint32_t dur_ms;
};

// 目录缓存（music/.index）一行：大小、时长、路径、歌名、歌手、专辑，用制表符分开，以换行结尾。
// 字段里的制表符和换行换成空格。返回写了多少字节，放不下时返回 0
size_t track_index_line(const TrackInfo &t, char *out, size_t cap);
// 解析一行（不含换行），格式不对时返回 false
bool track_index_parse(const char *line, size_t len, TrackInfo &out);

// 路径在哪个歌单：第一级文件夹名，直接放在 music 下的歌返回空字符串
void track_folder(const char *path, char *out, size_t cap);

// ---- 播放顺序 ----

enum class PlayMode : uint8_t { Loop, Shuffle, One };  // 列表循环、随机、单曲循环

// 一个歌单里的播放顺序：order 里是歌在歌单里的序号。随机时打乱，当前这首放最前面
class PlayOrder {
 public:
  // 歌单有 n 首（最多 kMax 首），从第 current 首开始
  void set(int n, int current, PlayMode mode, uint32_t seed);
  void set_mode(PlayMode mode, uint32_t seed);
  int current() const { return n_ ? order_[pos_] : -1; }
  // 下一首、上一首（列表循环和随机放到头会接着从另一头放）。user 为 false 是自然放完：单曲循环时还是这一首
  int next(bool user);
  int prev();
  // 直接跳到歌单里的第 i 首
  void jump(int i);
  int size() const { return n_; }
  PlayMode mode() const { return mode_; }

  static constexpr int kMax = 1000;

 private:
  void shuffle(uint32_t seed);
  int order_[kMax];
  int n_ = 0;
  int pos_ = 0;
  PlayMode mode_ = PlayMode::Loop;
};

// 睡眠定时：每点一次换下一档（关、15、30、60 分钟），返回新的分钟数
int sleep_next_choice(int minutes);
