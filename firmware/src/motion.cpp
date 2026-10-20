#include "motion.h"

#include <math.h>

static float dot3(const float *a, const float *b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
static float len3(const float *a) { return sqrtf(dot3(a, a)); }

MotionEvent MotionDetector::update(uint32_t now_ms, float ax, float ay, float az) {
  float a[3] = {ax, ay, az};
  if (!started_) {
    started_ = true;
    for (int i = 0; i < 3; i++) g_[i] = ref_[i] = a[i];
    last_ms_ = now_ms;
    return MotionEvent::None;
  }
  float dt = (now_ms - last_ms_) / 1000.0f;
  last_ms_ = now_ms;
  if (dt <= 0 || dt > 0.5f) dt = 0.02f;

  // 重力 = 1 秒低通；减掉重力剩下的就是动作
  float k = dt / (1.0f + dt);
  float dyn[3];
  for (int i = 0; i < 3; i++) {
    g_[i] += (a[i] - g_[i]) * k;
    dyn[i] = a[i] - g_[i];
  }
  float dmag = len3(dyn);

  // 朝向：和平时的方向比
  float gl = len3(g_), rl = len3(ref_);
  float cosang = gl > 0.3f && rl > 0.3f ? dot3(g_, ref_) / (gl * rl) : 1.0f;
  bool flipped_now = upside_ ? cosang < 0.3f : cosang < -0.35f;
  if (flipped_now != upside_) {
    if (!flipping_) {
      flipping_ = true;
      flip_since_ = now_ms;
    } else if (now_ms - flip_since_ >= kFlipHoldMs) {
      upside_ = flipped_now;
      flipping_ = false;
    }
  } else {
    flipping_ = false;
  }
  // 静止且没倒过来时，平时的方向慢慢跟上（30 秒），换个姿势摆放也能适应
  if (dmag < 0.05f && !upside_ && cosang > -0.35f) {
    float kr = dt / (30.0f + dt);
    for (int i = 0; i < 3; i++) ref_[i] += (g_[i] - ref_[i]) * kr;
  }

  // 摇一摇：一段时间内来回好几下大的加速度，方向要交替
  MotionEvent ev = MotionEvent::None;
  if (dmag > kShakeG && now_ms - last_peak_ms_ > 80) {
    // 用变化最大的那个轴的正负判断方向
    int axis = 0;
    for (int i = 1; i < 3; i++) {
      if (fabsf(dyn[i]) > fabsf(dyn[axis])) axis = i;
    }
    int sign = dyn[axis] > 0 ? 1 : -1;
    if (sign != last_peak_sign_) {
      last_peak_sign_ = sign;
      last_peak_ms_ = now_ms;
      if (peak_count_ < 8) peaks_[peak_count_++] = now_ms;
    }
  }
  int n = 0;  // 去掉窗口外的
  for (int i = 0; i < peak_count_; i++) {
    if (now_ms - peaks_[i] <= kShakeWindowMs) peaks_[n++] = peaks_[i];
  }
  peak_count_ = n;
  bool recent_shake = peak_count_ >= 2 || (int32_t)(now_ms - shake_until_) < 0;
  if (peak_count_ >= kShakePeaks && (int32_t)(now_ms - shake_until_) >= 0) {
    ev = MotionEvent::Shake;
    shake_until_ = now_ms + kShakeCooldownMs;
    peak_count_ = 0;
  }

  // 急刹急转：中等大小的加速度持续一小会儿；在摇晃的时候不算
  if (dmag > kJoltG && !recent_shake) {
    if (!jolting_) {
      jolting_ = true;
      jolt_since_ = now_ms;
    } else if (now_ms - jolt_since_ >= kJoltHoldMs && ev == MotionEvent::None &&
               (int32_t)(now_ms - jolt_until_) >= 0 && !upside_ && !flipping_) {
      ev = MotionEvent::Jolt;
      jolt_until_ = now_ms + kJoltCooldownMs;
    }
  } else {
    jolting_ = false;
  }
  return ev;
}
