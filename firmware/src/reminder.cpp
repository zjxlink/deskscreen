#include "reminder.h"

#include "night.h"

void Reminder::configure(const ReminderConfig &cfg) {
  if (cfg == cfg_) return;
  cfg_ = cfg;
  armed_ = false;  // 下次 tick 从头计时
}

bool Reminder::tick(uint32_t now_ms, int minutes, bool hold) {
  bool in_window = cfg_.on && cfg_.interval_min > 0 && minutes >= 0 &&
                   night_is_active(minutes, cfg_.start, cfg_.end);
  if (!in_window) {
    armed_ = false;  // 时段外不计时，第二天进入时段后重新开始
    return false;
  }
  if (!armed_) {
    armed_ = true;
    due_ms_ = now_ms + interval_ms();
    return false;
  }
  if (hold || (int32_t)(now_ms - due_ms_) < 0) return false;
  due_ms_ = now_ms + interval_ms();
  return true;
}

void Reminder::restart(uint32_t now_ms) {
  if (armed_) due_ms_ = now_ms + interval_ms();
}

uint32_t Reminder::remaining_ms(uint32_t now_ms) const {
  if (!armed_) return 0;
  int32_t left = (int32_t)(due_ms_ - now_ms);
  return left > 0 ? (uint32_t)left : 0;
}
