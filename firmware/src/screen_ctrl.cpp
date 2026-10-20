#include "screen_ctrl.h"

#include <stdlib.h>

#include "night.h"

void ScreenCtrl::start(uint32_t now_ms) {
  started_ = true;
  show(PAGE_CLOCK, now_ms, false, true);  // 此前屏幕上是开机提示页，必须切过来
}

bool ScreenCtrl::woken(uint32_t now_ms) const {
  return woken_until_ && (int32_t)(now_ms - woken_until_) < 0;
}

void ScreenCtrl::show(Page p, uint32_t now_ms, bool animate, bool force) {
  if (p != page_ || force) {
    page_dirty_ = true;
    page_animate_ = animate;
  }
  page_ = p;
  page_shown_at_ = now_ms;
}

bool ScreenCtrl::take_page_change(Page &page, bool &animate) {
  if (!page_dirty_) return false;
  page_dirty_ = false;
  page = page_;
  animate = page_animate_;
  return true;
}

void ScreenCtrl::tick(uint32_t now_ms, int minutes) {
  night_now_ = cfg_.night_on && minutes >= 0 &&
               night_is_active(minutes, cfg_.night_start, cfg_.night_end);
  away_ = started_ && cfg_.away_off && has_presence_ && !woken(now_ms) &&
          now_ms - last_present_ >= cfg_.away_min * 60000UL;
  dimmed_ = (night_now_ && !woken(now_ms)) || away_;
  fade(now_ms);
  if (started_) auto_rotate(now_ms);
}

// 开机时直接设定亮度；之后每 30ms 走一步，越接近目标步子越小
void ScreenCtrl::fade(uint32_t now_ms) {
  int target = away_ ? 0 : dimmed_ ? cfg_.night_brightness : cfg_.day_brightness;
  if (current_ < 0) {
    current_ = target;
    last_step_ = now_ms;
    return;
  }
  if (current_ == target || (uint32_t)(now_ms - last_step_) < kFadeStepMs) return;
  last_step_ = now_ms;
  int step = abs(target - current_) / 8;
  if (step < 1) step = 1;
  current_ += target > current_ ? step : -step;
}

void ScreenCtrl::auto_rotate(uint32_t now_ms) {
  uint32_t interval = (cfg_.page_seconds ? cfg_.page_seconds : 15) * 1000UL;
  if (pinned_) {
    if (page_ == pinned_page_) {
      page_shown_at_ = now_ms;
    } else if ((uint32_t)(now_ms - page_shown_at_) >= interval) {
      show(pinned_page_, now_ms, true);
    }
    return;
  }
  // 夜间固定显示时钟，临时点亮期间也不自动轮播
  if (night_now_) {
    if (page_ != PAGE_CLOCK && !woken(now_ms)) show(PAGE_CLOCK, now_ms, true);
    page_shown_at_ = now_ms;
    return;
  }
  if (cfg_.page_seconds == 0 || (uint32_t)(now_ms - page_shown_at_) < interval) return;
  for (int k = 1; k <= PAGE_COUNT; k++) {
    Page p = (Page)((page_ + k) % PAGE_COUNT);
    if (cfg_.page_enabled[p] && !(cfg_.page_unavailable & (1u << p))) {
      if (p == page_) {
        page_shown_at_ = now_ms;  // 只有当前页可轮播，原地不动
      } else {
        show(p, now_ms, true);
      }
      return;
    }
  }
}

void ScreenCtrl::set_presence(bool present, uint32_t now_ms) {
  if (!has_presence_ || present) last_present_ = now_ms;
  has_presence_ = true;
}

void ScreenCtrl::activity(uint32_t now_ms) {
  last_present_ = now_ms;  // 有人在操作，当然有人
  away_ = false;
  // 点亮期间每次操作都顺延 30 秒
  if (night_now_) woken_until_ = now_ms + kWakeMs;
  dimmed_ = false;
}

bool ScreenCtrl::on_input(uint32_t now_ms) {
  if (!started_) return false;
  bool was_dimmed = dimmed_;
  activity(now_ms);
  if (was_dimmed) {
    dimmed_ = false;
    return false;
  }
  // 手动切换时轮流显示所有页面（没设置的页面也能看到提示），只跳过板子上没有硬件的页面
  Page next = page_;
  for (int k = 1; k <= PAGE_COUNT; k++) {
    next = (Page)((page_ + k) % PAGE_COUNT);
    if (!(cfg_.page_unavailable & (1u << next))) break;
  }
  show(next, now_ms, true);
  return true;
}
