#pragma once

#include <stddef.h>
#include <stdint.h>

// MJPEG 视频（.avi）的文件解析，不依赖硬件，可在电脑上测试。
// 网页上传视频时浏览器会转成这种格式（带声音时是 16kHz 单声道 PCM）；电影用 tools/movie 里的转换工具，
// 也是 ffmpeg：MJPEG 画面 + 16kHz 单声道 PCM 声音。
// 每帧是一张完整的 JPG，交给 LovyanGFX 解码；声音只认 16 位 PCM，别的格式（MP3 等）当成没有声音。
// 超过 1GB 的文件 ffmpeg 会写成 OpenDML（AVI 2.0）格式，后面接着几个 AVIX 段，也能接着放。
// 快进快退用文件里的索引：OpenDML 的 indx/ix00，没有时用 idx1，都没有时从头数。
// 文件一边放一边从 TF 卡读，所以通过 read 回调随机读取，不用整个读进内存

// ReadError 是读卡出错（读的位置没变，过一会儿可以再试），和真的放完了（End）分开
enum class AviChunk : uint8_t { End, Video, Audio, ReadError };

class AviReader {
 public:
  // 从 off 开始读 len 个字节到 buf，返回读到的字节数
  using ReadFn = size_t (*)(void *ctx, uint32_t off, uint8_t *buf, size_t len);

  // 读文件头，找到视频数据。不是 AVI 或者没有视频数据时返回 false
  bool open(ReadFn read, void *ctx, uint32_t file_size);

  // 每帧多少微秒（文件头里写的，没写或不合理时按每秒 15 帧）
  uint32_t frame_us() const { return frame_us_; }
  int width() const { return width_; }
  int height() const { return height_; }
  // 一共多少帧（文件头里写的，不知道时为 0）
  uint32_t frames() const { return total_frames_; }
  uint32_t duration_ms() const { return (uint32_t)((uint64_t)total_frames_ * frame_us_ / 1000); }

  // 有能放的声音（16 位 PCM，1～2 个声道）
  bool has_audio() const { return audio_stream_ >= 0 && audio_ok_; }
  uint32_t audio_rate() const { return audio_rate_; }
  int audio_channels() const { return audio_channels_; }

  // 按文件里的顺序取下一块视频或声音数据（声音只在 has_audio 时给），放完了或文件坏了时返回 End，
  // 读卡出错时返回 ReadError
  AviChunk next_chunk(uint32_t &off, uint32_t &len);

  // 只要视频：下一帧 JPG 数据的位置和长度；放完了或文件坏了时返回 false
  bool next(uint32_t &off, uint32_t &len);

  // 读一块数据（next / next_chunk 给出的位置），读不全时返回 false
  bool read(uint32_t off, uint8_t *buf, size_t len) { return read_at(off, buf, len); }

  // 下一次 next / next_chunk 给出的视频帧是第几帧（从 0 开始）
  uint32_t frame_pos() const { return frame_pos_; }

  // 回到第一帧
  void rewind();

  // 跳到第 frame 帧（超过结尾时跳到最后一帧）。之后的声音从那一帧附近接着给。
  // 文件坏了时返回 false，位置不变
  bool seek_frame(uint32_t frame);

 private:
  struct Segment {
    uint32_t start;  // movi 列表里第一块的位置
    uint32_t end;
  };
  static constexpr int kMaxSegments = 8;  // 第一段 + AVIX 段，FAT32 最大 4GB，每段最多 1GB

  bool read_at(uint32_t off, uint8_t *buf, size_t len);
  void parse_hdrl(uint32_t start, uint32_t end);
  void parse_strl(uint32_t start, uint32_t end, int stream);
  void find_extra_segments(uint32_t riff_end);
  int stream_of(const uint8_t *id) const;
  bool seek_indx(uint32_t frame, uint32_t &chunk_off, uint32_t &found);
  bool seek_idx1(uint32_t frame, uint32_t &chunk_off, uint32_t &found);
  void set_pos(uint32_t chunk_off);

  ReadFn read_ = nullptr;
  void *ctx_ = nullptr;
  uint32_t size_ = 0;
  Segment segs_[kMaxSegments] = {};
  int seg_count_ = 0;
  int seg_ = 0;
  uint32_t pos_ = 0;
  uint32_t frame_pos_ = 0;
  uint32_t frame_us_ = 66666;
  uint32_t total_frames_ = 0;
  int width_ = 0;
  int height_ = 0;
  int streams_ = 0;
  int video_stream_ = -1;  // 第几个流是视频，-1 表示文件头里没写（所有 dc/db 块都当视频）
  int audio_stream_ = -1;
  bool audio_ok_ = false;
  uint32_t audio_rate_ = 0;
  int audio_channels_ = 0;
  uint32_t indx_off_ = 0;    // 视频流的 OpenDML 总索引（indx）的条目开始位置
  uint32_t indx_count_ = 0;
  uint32_t idx1_off_ = 0;    // idx1 的条目开始位置
  uint32_t idx1_count_ = 0;
};

// 把 AVI 里的 16 位 PCM 声音（任意采样率，1～2 个声道）转成喇叭用的 16kHz 单声道。
// 声音块的长度不一定是整数个采样，剩下的字节留到下一块
class PcmTo16k {
 public:
  void reset(uint32_t rate, int channels);
  // 转换一块数据，返回写进 out 的采样数（最多 out_max 个，多出来的丢掉）
  size_t convert(const uint8_t *in, size_t len, int16_t *out, size_t out_max);
  // len 字节的输入最多会转出多少个采样
  size_t max_out(size_t len) const;

 private:
  uint32_t step_ = 65536;  // 每个输出采样前进多少个输入采样（16.16 定点）
  uint32_t phase_ = 0;     // 下一个输出采样在 prev_ 和下一个输入采样之间的位置
  int channels_ = 1;
  int16_t prev_ = 0;
  bool has_prev_ = false;
  uint8_t carry_[4];
  size_t carry_len_ = 0;
};
