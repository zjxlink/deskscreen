#pragma once

#include <stddef.h>
#include <stdint.h>

// 放音乐时喇叭和麦克风切到歌曲的采样率（如 44.1kHz），唤醒词、回声消除和音乐律动还是要 16kHz：
// 这里先低通滤波（去掉 8kHz 以上，免得混叠），再按比例取样。精度够听清说话、算频谱，不追求高保真。
// 不依赖硬件，可在电脑上测试
class Resampler16k {
 public:
  static constexpr int kTaps = 16;  // 2 的幂

  // 输入采样率，换歌、换采样率时调用；16000 时原样输出
  void reset(uint32_t in_rate);
  uint32_t in_rate() const { return rate_; }
  // 输入 n 个，返回输出了几个（大约 n * 16000 / in_rate 个）。cap 要至少 n * 16000 / in_rate + 2，
  // 放不下的会丢掉
  size_t process(const int16_t *in, size_t n, int16_t *out, size_t cap);

 private:
  uint32_t rate_ = 16000;
  int16_t coef_[kTaps] = {};
  int16_t hist_[kTaps] = {};
  int at_ = 0;
  int32_t prev_ = 0;   // 上一个滤波后的采样
  uint32_t step_ = 65536;  // 输出一个采样前进多少个输入采样（Q16）
  uint32_t t_ = 0;     // 下一个输出在 prev_ 和这次之间的位置（Q16）
};
