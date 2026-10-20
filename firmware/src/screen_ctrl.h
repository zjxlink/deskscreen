#pragma once

#include <stdint.h>

#include "pages.h"

// 屏幕亮度和页面切换的状态机：夜间调暗、夜里临时点亮、没人时熄屏、自动轮播。
// 不依赖硬件，时间和输入都由调用方传入，可在电脑上按时间线模拟测试。

struct ScreenConfig {
  uint8_t day_brightness = 180;
  bool night_on = false;
  uint16_t night_start = 23 * 60;  // 当天第几分钟
  uint16_t night_end = 7 * 60;
  uint8_t night_brightness = 10;
  uint8_t page_seconds = 15;       // 自动轮播间隔，0 表示不轮播
  bool page_enabled[PAGE_COUNT] = {true};  // 自动轮播只轮到已设置好的页面，默认只有时钟页
  uint32_t page_unavailable = 0;   // 第 i 位表示这块板子没有第 i 页的硬件（如没麦克风就没有 AI 聊天），手动切换也跳过
  bool away_off = false;           // 有人体感应时：没人一段时间后熄屏，有人回来再亮
  uint8_t away_min = 5;            // 多久没人算离开
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

  // 固定在某个页面（番茄钟进行中）：自动轮播不再切走；手动切走后，过一个轮播间隔再回来。夜间也一样
  void pin(Page p) {
    pinned_ = true;
    pinned_page_ = p;
  }
  void unpin() { pinned_ = false; }

  // 有人在操作但不切换页面（快捷设置页上的点击）：点亮屏幕，夜间顺延 30 秒
  void activity(uint32_t now_ms);

  // 正在用当前页面（AI 聊天）：自动轮播从现在重新计时，不会切走
  void stay(uint32_t now_ms) { page_shown_at_ = now_ms; }

  // 人体感应的结果，主循环每次调用；从没调用过（没有雷达）就当一直有人。
  // 雷达只对动作敏感，坐着不动也可能一阵子没信号，所以离开要等 away_min 分钟
  void set_presence(bool present, uint32_t now_ms);

  // 立即点亮屏幕（夜间时保持正常亮度 30 秒），用于番茄钟提醒
  void wake(uint32_t now_ms) { woken_until_ = now_ms + kWakeMs; }

  // 跳到指定页面（带动画）
  void go_to(Page p, uint32_t now_ms) { show(p, now_ms, true); }

  bool started() const { return started_; }
  int brightness() const { return current_ < 0 ? cfg_.day_brightness : current_; }
  Page page() const { return page_; }
  bool night() const { return night_now_; }
  bool dimmed() const { return dimmed_; }
  bool away() const { return away_; }  // 没人，屏幕已熄灭

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
  bool has_presence_ = false;
  uint32_t last_present_ = 0;
  bool away_ = false;
  bool dimmed_ = false;
  int current_ = -1;
  uint32_t last_step_ = 0;
  bool pinned_ = false;
  Page pinned_page_ = PAGE_CLOCK;
};
