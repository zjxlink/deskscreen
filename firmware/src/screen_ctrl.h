#pragma once

#include <stdint.h>

#include "pages.h"

// 屏幕亮度和页面切换的状态机：夜间调暗、夜里临时点亮、自动轮播。
// 不依赖硬件，时间和输入都由调用方传入，可在电脑上按时间线模拟测试。

struct ScreenConfig {
  uint8_t day_brightness = 180;
  bool night_on = false;
  uint16_t night_start = 23 * 60;  // 当天第几分钟
  uint16_t night_end = 7 * 60;
  uint8_t night_brightness = 10;
  uint8_t page_seconds = 15;       // 自动轮播间隔，0 表示不轮播
  bool page_enabled[PAGE_COUNT] = {true, false, false};  // 自动轮播只轮到已设置好的页面
};

class ScreenCtrl {
 public:
  static constexpr uint32_t kWakeMs = 30000;  // 夜间点亮后保持正常亮度的时间
  static constexpr uint32_t kFadeStepMs = 30;  // 亮度渐变的步进间隔，整个渐变约 1 秒

  void configure(const ScreenConfig &cfg) { cfg_ = cfg; }

  // 开机提示页结束后调用，从时钟页开始；在此之前的操作都忽略
  void start(uint32_t now_ms);

  // 主循环每次调用。minutes 为当天第几分钟，还没对时传 -1
  void tick(uint32_t now_ms, int minutes);

  // 用户点屏幕或短按按键。夜间调暗时第一下只点亮屏幕；返回 true 表示切换了页面
  bool on_input(uint32_t now_ms);

  int brightness() const { return current_ < 0 ? cfg_.day_brightness : current_; }
  Page page() const { return page_; }
  bool night() const { return night_now_; }
  bool dimmed() const { return dimmed_; }

  // 页面有变化时返回 true，并给出是否需要切换动画
  bool take_page_change(Page &page, bool &animate);

 private:
  bool woken(uint32_t now_ms) const;
  void show(Page p, uint32_t now_ms, bool animate, bool force = false);
  void fade(uint32_t now_ms);
  void auto_rotate(uint32_t now_ms);

  ScreenConfig cfg_;
  bool started_ = false;
  Page page_ = PAGE_CLOCK;
  bool page_dirty_ = false;
  bool page_animate_ = false;
  uint32_t page_shown_at_ = 0;
  uint32_t woken_until_ = 0;
  bool night_now_ = false;
  bool dimmed_ = false;
  int current_ = -1;
  uint32_t last_step_ = 0;
};
