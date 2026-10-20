#include "music.h"

#include <math.h>
#include <string.h>

#include <algorithm>

static constexpr float kMinLoudDb = -55;  // 比这还小的声音不算（安静的房间大约 -60dBFS）
static constexpr float kBarRangeDb = 40;   // 频谱条从「最近最响的那条」往下 40dB 画满
static constexpr float kRefFallPerFrame = 0.05f;  // 参考电平每帧落 0.05dB（每秒约 1.5dB），声音变小后频谱条慢慢变高
static constexpr float kRefMinDb = -45;    // 参考电平最低到这里，安静时频谱条不会被底噪撑满
static constexpr uint32_t kBeatGapMs = 250;  // 两个节拍至少隔 250ms（每分钟 240 拍以内）
static constexpr int kMinBeats = 4;          // 最近 4 秒至少 4 个节拍
static constexpr float kLoudShare = 0.85f;   // 最近 4 秒至少 85% 的时间有声音
static constexpr float kSteadyShare = 0.6f; // 至少 60% 的节拍间隔和中位数差不多（或者是它的一半、两倍）
static constexpr uint32_t kStartMs = 2500;   // 像音乐的样子持续 2.5 秒才算开始
static constexpr uint32_t kStopMs = 4000;    // 不像音乐持续 4 秒才算停了

MusicMeter::MusicMeter() {
  for (int i = 0; i < kMusicFrame; i++) window_[i] = 0.5f - 0.5f * cosf(2 * (float)M_PI * i / (kMusicFrame - 1));
  // 频谱条的边界：60Hz 到 8kHz 等比分成 16 段，每段至少一个 FFT 点
  const float bin_hz = (float)kMusicRate / kMusicFrame;
  int prev = 0;
  for (int b = 0; b <= kMusicBands; b++) {
    float hz = 60.0f * powf(8000.0f / 60.0f, (float)b / kMusicBands);
    int k = (int)lroundf(hz / bin_hz);
    if (k <= prev) k = prev + 1;
    if (k > kMusicFrame / 2) k = kMusicFrame / 2;
    band_lo_[b] = k;
    prev = k;
  }
}

// 原地基 2 FFT
static void fft(float *re, float *im, int n) {
  for (int i = 1, j = 0; i < n; i++) {
    int bit = n >> 1;
    for (; j & bit; bit >>= 1) j ^= bit;
    j ^= bit;
    if (i < j) {
      float t = re[i];
      re[i] = re[j];
      re[j] = t;
      t = im[i];
      im[i] = im[j];
      im[j] = t;
    }
  }
  for (int len = 2; len <= n; len <<= 1) {
    float ang = -2 * (float)M_PI / len;
    float wr = cosf(ang), wi = sinf(ang);
    for (int i = 0; i < n; i += len) {
      float cr = 1, ci = 0;
      for (int k = 0; k < len / 2; k++) {
        int a = i + k, b = a + len / 2;
        float xr = re[b] * cr - im[b] * ci;
        float xi = re[b] * ci + im[b] * cr;
        re[b] = re[a] - xr;
        im[b] = im[a] - xi;
        re[a] += xr;
        im[a] += xi;
        float t = cr * wr - ci * wi;
        ci = cr * wi + ci * wr;
        cr = t;
      }
    }
  }
}

void MusicMeter::feed(const int16_t *pcm, uint32_t now_ms) {
  double sum = 0;
  for (int i = 0; i < kMusicFrame; i++) {
    float s = pcm[i] / 32768.0f;
    sum += (double)s * s;
    re_[i] = s * window_[i];
    im_[i] = 0;
  }
  level_db_ = 10 * log10f((float)(sum / kMusicFrame) + 1e-10f);
  bool loud = level_db_ > kMinLoudDb;

  fft(re_, im_, kMusicFrame);
  // 每条频谱的平均功率；Hann 窗加上 FFT 的长度，换算成和电平差不多的 dB
  const float norm = 4.0f / ((float)kMusicFrame * kMusicFrame * 0.375f);
  float bass = 0, db[kMusicBands], top = -120;
  for (int b = 0; b < kMusicBands; b++) {
    float p = 0;
    int lo = band_lo_[b], hi = band_lo_[b + 1];
    for (int k = lo; k < hi; k++) p += re_[k] * re_[k] + im_[k] * im_[k];
    p = p * norm / (hi - lo);
    if (b < 4) bass += p;  // 60～200Hz：鼓点
    db[b] = 10 * log10f(p + 1e-12f);
    if (db[b] > top) top = db[b];
  }
  // 频谱条按最近最响的那条来定满格，手机外放和音箱都能画得差不多高
  ref_db_ = top > ref_db_ ? top : ref_db_ - kRefFallPerFrame;
  if (ref_db_ < kRefMinDb) ref_db_ = kRefMinDb;
  for (int b = 0; b < kMusicBands; b++) {
    float h = (db[b] - (ref_db_ - kBarRangeDb)) / kBarRangeDb;
    h = h < 0 ? 0 : h > 1 ? 1 : h;
    // 上升快、落下慢，看起来跟得上节奏又不闪
    bars_[b] = h > bars_[b] ? h : bars_[b] * 0.85f + h * 0.15f;
  }

  // 节拍：低音能量突然比最近的平均高出一截
  if (loud && bass > bass_avg_ * 2.0f && bass > 1e-7f && now_ms - last_beat_ >= kBeatGapMs) {
    last_beat_ = now_ms;
    beat_ = true;
    beats_[beat_pos_] = now_ms;
    beat_pos_ = (beat_pos_ + 1) % 16;
  }
  bass_avg_ = bass_avg_ * 0.9f + bass * 0.1f;

  loud_[pos_] = loud;
  pos_ = (pos_ + 1) % kHistory;
  update_state(now_ms);
}

int MusicMeter::recent_beats(uint32_t now_ms) const {
  int n = 0;
  for (uint32_t t : beats_) n += t && now_ms - t <= 4000;
  return n;
}

// 最近 4 秒的节拍是不是有规律：音乐的鼓点间隔差不多一样，说话的音节忽快忽慢
bool MusicMeter::steady_beats(uint32_t now_ms) const {
  uint32_t t[16];
  int n = 0;
  for (uint32_t b : beats_)
    if (b && now_ms - b <= 4000) t[n++] = b;
  if (n < kMinBeats) return false;
  std::sort(t, t + n);
  float iv[15], sorted[15];
  for (int i = 0; i + 1 < n; i++) iv[i] = sorted[i] = (float)(t[i + 1] - t[i]);
  int m = n - 1;
  std::sort(sorted, sorted + m);
  float median = sorted[m / 2];
  int ok = 0;
  for (int i = 0; i < m; i++) {
    float r = iv[i] / median;
    ok += fabsf(r - 1) < 0.15f || fabsf(r - 2) < 0.3f || fabsf(r - 0.5f) < 0.08f;
  }
  return ok >= kSteadyShare * m;
}

void MusicMeter::update_state(uint32_t now_ms) {
  int loud = 0;
  for (bool l : loud_) loud += l;
  bool like = loud >= kLoudShare * kHistory && steady_beats(now_ms);
  if (like) {
    if (!on_since_) on_since_ = now_ms ? now_ms : 1;
    off_since_ = 0;
    if (now_ms - on_since_ >= kStartMs) playing_ = true;
  } else {
    on_since_ = 0;
    if (!off_since_) off_since_ = now_ms ? now_ms : 1;
    if (now_ms - off_since_ >= kStopMs) playing_ = false;
  }
}

void MusicMeter::idle(uint32_t now_ms) {
  for (float &b : bars_) b *= 0.85f;
  memset(loud_, 0, sizeof(loud_));
  memset(beats_, 0, sizeof(beats_));
  bass_avg_ = 0;
  beat_ = false;
  playing_ = false;
  on_since_ = 0;
  off_since_ = now_ms ? now_ms : 1;
}

bool MusicMeter::take_beat() {
  bool b = beat_;
  beat_ = false;
  return b;
}
