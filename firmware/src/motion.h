#pragma once

#include <stdint.h>

// 从加速度计读数里认出几种动作：摇一摇、急刹急转（或者被拿起来）、倒过来。
// 不依赖硬件，时间和读数（单位 g）由调用方传入，可在电脑上测试。
// 不假设板子怎么摆：以平时静止时的重力方向为准，桌上、车里斜着装都一样

enum class MotionEvent { None, Shake, Jolt };

class MotionDetector {
 public:
  static constexpr float kShakeG = 0.9f;       // 摇晃时每一下超过这么多才算
  static constexpr int kShakePeaks = 4;        // 1.2 秒内来回这么多下算摇一摇
  static constexpr uint32_t kShakeWindowMs = 1200;
  static constexpr float kJoltG = 0.3f;        // 急刹、急转的加速度
  static constexpr uint32_t kJoltHoldMs = 150;  // 持续这么久才算，敲一下桌子不算
  static constexpr uint32_t kShakeCooldownMs = 3000;
  static constexpr uint32_t kJoltCooldownMs = 5000;
  static constexpr uint32_t kFlipHoldMs = 1000;  // 倒过来这么久才算

  // 每 20ms 左右调用一次
  MotionEvent update(uint32_t now_ms, float ax, float ay, float az);

  // 现在是倒过来的（和平时的方向差了 110 度以上）
  bool upside_down() const { return upside_; }

 private:
  bool started_ = false;
  uint32_t last_ms_ = 0;
  float g_[3] = {0, 0, 0};    // 重力方向（1 秒平滑）
  float ref_[3] = {0, 0, 0};  // 平时的重力方向（静止时慢慢更新）

  uint32_t peaks_[8] = {};
  int peak_count_ = 0;
  int last_peak_sign_ = 0;
  uint32_t last_peak_ms_ = 0;
  uint32_t shake_until_ = 0;  // 冷却

  uint32_t jolt_since_ = 0;
  bool jolting_ = false;
  uint32_t jolt_until_ = 0;

  bool upside_ = false;
  uint32_t flip_since_ = 0;
  bool flipping_ = false;
};
