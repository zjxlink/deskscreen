#pragma once

#include <stddef.h>
#include <stdint.h>

#include "track.h"

// 把一首歌解成单声道 16 位 PCM（板子只有一个喇叭，立体声两路取平均）。
// MP3 用 minimp3，FLAC 用 dr_flac，WAV 自己读。大块内存都放在 PSRAM，任务栈只要几 KB。
// 不依赖 Arduino，可在电脑上测试
class SongDecoder {
 public:
  ~SongDecoder() { close(); }
  // p 是 track_probe 的结果；read 要一直能用到 close
  bool open(TrackRead read, void *ctx, uint32_t size, const TrackProbe &p);
  void close();
  bool is_open() const { return format_ != TrackFormat::None; }
  // 解最多 max 个采样，返回解出几个；0 表示放完了（或者后面的数据坏了）
  size_t read(int16_t *out, size_t max);
  // 跳到第 ms 毫秒（MP3 是大约的位置）
  bool seek(uint32_t ms);
  // 解到了第几毫秒
  uint32_t pos_ms() const;
  uint32_t rate() const { return rate_; }

 private:
  size_t read_mp3(int16_t *out, size_t max);
  size_t read_wav(int16_t *out, size_t max);
  size_t read_flac(int16_t *out, size_t max);
  bool fill_mp3();

  TrackFormat format_ = TrackFormat::None;
  TrackRead read_ = nullptr;
  void *ctx_ = nullptr;
  uint32_t size_ = 0;
  TrackProbe probe_ = {};
  uint32_t rate_ = 0;
  uint64_t samples_ = 0;  // 已经交出去的采样数（从歌的开头算）
  uint32_t file_pos_ = 0;  // 下次从文件哪里读（MP3、WAV）
  uint32_t data_end_ = 0;
  // MP3
  void *mp3_ = nullptr;      // mp3dec_t
  uint8_t *in_ = nullptr;    // 读进来还没解的数据
  size_t in_len_ = 0;
  int16_t *pcm_ = nullptr;   // 解出来还没交出去的（已经混成单声道）
  size_t pcm_len_ = 0, pcm_at_ = 0;
  int bad_ = 0;
  // FLAC
  void *flac_ = nullptr;     // drflac
  uint32_t flac_pos_ = 0;    // dr_flac 读文件的位置
  friend struct SongDecoderFlacIo;
};
