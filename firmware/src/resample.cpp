#include "resample.h"

#include <math.h>
#include <string.h>

void Resampler16k::reset(uint32_t in_rate) {
  rate_ = in_rate ? in_rate : 16000;
  step_ = (uint32_t)(((uint64_t)rate_ << 16) / 16000);
  memset(hist_, 0, sizeof(hist_));
  at_ = 0;
  prev_ = 0;
  t_ = 0;
  // 加了汉明窗的 sinc 低通，截止 7.2kHz（输入是 16kHz 以下时截到它的 0.45）
  double fc = 7200.0 / rate_;
  if (fc > 0.45) fc = 0.45;
  double h[kTaps], sum = 0;
  for (int i = 0; i < kTaps; i++) {
    double x = i - (kTaps - 1) / 2.0;
    double s = 2 * fc * (x == 0 ? 1.0 : sin(2 * M_PI * fc * x) / (2 * M_PI * fc * x));
    h[i] = s * (0.54 - 0.46 * cos(2 * M_PI * i / (kTaps - 1)));
    sum += h[i];
  }
  for (int i = 0; i < kTaps; i++) coef_[i] = (int16_t)lround(h[i] / sum * 32767);
}

size_t Resampler16k::process(const int16_t *in, size_t n, int16_t *out, size_t cap) {
  if (rate_ == 16000) {
    size_t m = n < cap ? n : cap;
    memcpy(out, in, m * sizeof(int16_t));
    return m;
  }
  size_t m = 0;
  for (size_t i = 0; i < n; i++) {
    hist_[at_] = in[i];
    at_ = (at_ + 1) & (kTaps - 1);
    int32_t acc = 0;
    for (int k = 0; k < kTaps; k++) acc += (int32_t)coef_[k] * hist_[(at_ + k) & (kTaps - 1)];
    int32_t cur = acc >> 15;
    // prev_ 和 cur 之间要输出的采样，线性插值
    while (t_ < 65536) {
      if (m >= cap) return m;
      int32_t v = prev_ + (int32_t)(((int64_t)(cur - prev_) * t_) >> 16);
      out[m++] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
      t_ += step_;
    }
    t_ -= 65536;
    prev_ = cur;
  }
  return m;
}
