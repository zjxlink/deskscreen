#pragma once

#include <stdint.h>

// 回声消除要知道麦克风每个采样录下时喇叭正在放什么。喇叭和麦克风用同一组 I2S 时钟，
// 所以「喇叭放到第几个采样」减「麦克风录到第几个采样」是个常数，只是每次读麦克风时量到的值有
// 一个 DMA 缓冲（256 个采样）左右的抖动。这里取最近 64 次的平均当作偏移
class RefAligner {
 public:
  void add(int32_t est) {
    if (count_ == kN) sum_ -= hist_[at_];
    hist_[at_] = est;
    sum_ += est;
    at_ = (at_ + 1) % kN;
    if (count_ < kN) count_++;
  }
  int32_t offset() const { return count_ ? (int32_t)(sum_ / count_) : 0; }
  bool ready() const { return count_ >= 8; }

 private:
  static constexpr int kN = 64;
  int32_t hist_[kN] = {};
  int count_ = 0;
  int at_ = 0;
  int64_t sum_ = 0;
};

// 环形缓冲里取第 index 个采样（index 只增不减，容量是 2 的幂，溢出回绕也对）。
// 还没写到或者已经被覆盖的位置返回 0
inline int16_t ring_at(const int16_t *ring, uint32_t size, uint32_t written, uint32_t index) {
  uint32_t age = written - index;  // 1 表示最新写的那个
  if (age == 0 || age > size || (int32_t)age < 0) return 0;
  return ring[index & (size - 1)];
}
