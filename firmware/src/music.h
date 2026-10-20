#pragma once

#include <stdint.h>

// 音乐律动：分析麦克风的声音，算频谱、找节拍、判断是不是在放音乐。
// 不依赖硬件，可在电脑上测试。声音只在板子上分析，不录下来也不上传

static constexpr int kMusicRate = 16000;   // 采样率，和麦克风一样
static constexpr int kMusicFrame = 512;    // 每帧 512 个采样（32ms），做一次 FFT
static constexpr int kMusicBands = 16;     // 频谱条数，60Hz～8kHz 按对数分

class MusicMeter {
 public:
  MusicMeter();

  // 喂一帧声音（kMusicFrame 个采样），now_ms 是当前时间
  void feed(const int16_t *pcm, uint32_t now_ms);

  // 没有声音可分析时调用（喇叭在响、AI 聊天在用麦克风）：频谱慢慢落下，音乐状态清掉
  void idle(uint32_t now_ms);

  // 频谱条高度 0～1（已经做过平滑，直接画）
  const float *bands() const { return bars_; }

  // 正在放音乐：持续有声音、而且有规律的节拍。说话有停顿、节奏忽快忽慢，风扇声没有节拍，都不算
  bool playing() const { return playing_; }

  // 距离上次调用以来有没有新的节拍（每个节拍只返回一次 true）
  bool take_beat();

  // 当前音量（dBFS，0 是满幅），调试用
  float level_db() const { return level_db_; }

 private:
  static constexpr int kHistory = 125;  // 记最近 4 秒（125 帧）
  void update_state(uint32_t now_ms);
  int recent_beats(uint32_t now_ms) const;
  bool steady_beats(uint32_t now_ms) const;

  float window_[kMusicFrame];
  float re_[kMusicFrame];
  float im_[kMusicFrame];
  int band_lo_[kMusicBands + 1];  // 每条频谱的起止 FFT 序号
  float bars_[kMusicBands] = {};
  float level_db_ = -90;
  float ref_db_ = -45;  // 频谱条满格对应的电平（dB）
  float bass_avg_ = 0;
  uint32_t last_beat_ = 0;
  bool beat_ = false;
  bool loud_[kHistory] = {};
  uint32_t beats_[16] = {};  // 最近的节拍时间
  int beat_pos_ = 0;
  int pos_ = 0;
  bool playing_ = false;
  uint32_t off_since_ = 0;  // 不像音乐的状态从什么时候开始
  uint32_t on_since_ = 0;
};
