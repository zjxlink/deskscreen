#include "pomodoro.h"

static uint32_t minutes(uint16_t m) {
  return (uint32_t)m * 60 * 1000;
}

uint32_t Pomodoro::total_ms() const {
  switch (phase_) {
    case PomoPhase::Focus:
    case PomoPhase::Idle: return minutes(cfg_.focus_min);
    case PomoPhase::ShortBreak: return minutes(cfg_.short_min);
    case PomoPhase::LongBreak: return minutes(cfg_.long_min);
  }
  return 0;
}

uint32_t Pomodoro::remaining_ms(uint32_t now_ms) const {
  if (phase_ == PomoPhase::Idle) return total_ms();
  if (!running_) return paused_ms_;
  int32_t left = (int32_t)(end_ms_ - now_ms);
  return left > 0 ? (uint32_t)left : 0;
}

void Pomodoro::enter(PomoPhase p, uint32_t now_ms, bool run) {
  phase_ = p;
  running_ = run;
  paused_ms_ = total_ms();
  end_ms_ = now_ms + paused_ms_;
}

void Pomodoro::toggle(uint32_t now_ms) {
  if (phase_ == PomoPhase::Idle) {
    enter(PomoPhase::Focus, now_ms, true);
  } else if (running_) {
    paused_ms_ = remaining_ms(now_ms);
    running_ = false;
  } else {
    end_ms_ = now_ms + paused_ms_;
    running_ = true;
  }
}

void Pomodoro::reset() {
  phase_ = PomoPhase::Idle;
  running_ = false;
  cycle_done_ = 0;
}

void Pomodoro::skip(uint32_t now_ms) {
  switch (phase_) {
    case PomoPhase::Idle:
      break;
    case PomoPhase::Focus:
      // 跳过的专注不算完成，直接短休息
      enter(PomoPhase::ShortBreak, now_ms, true);
      break;
    case PomoPhase::ShortBreak:
    case PomoPhase::LongBreak:
      if (phase_ == PomoPhase::LongBreak) cycle_done_ = 0;
      enter(PomoPhase::Focus, now_ms, false);
      break;
  }
}

bool Pomodoro::tick(uint32_t now_ms, int day) {
  if (day >= 0 && day != day_) {
    if (day_ >= 0) done_today_ = 0;  // 第一次对时不清零
    day_ = day;
  }
  if (!running_ || phase_ == PomoPhase::Idle || remaining_ms(now_ms) > 0) return false;

  if (phase_ == PomoPhase::Focus) {
    done_today_++;
    cycle_done_++;
    if (cycle_done_ >= cfg_.long_every) {
      enter(PomoPhase::LongBreak, now_ms, true);
    } else {
      enter(PomoPhase::ShortBreak, now_ms, true);
    }
  } else {
    if (phase_ == PomoPhase::LongBreak) cycle_done_ = 0;
    // 休息结束不自动开始专注，等人回来点开始
    enter(PomoPhase::Focus, now_ms, false);
  }
  return true;
}
