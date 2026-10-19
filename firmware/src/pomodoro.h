#pragma once

#include <stdint.h>

// 番茄钟状态机：专注 → 短休息，每 N 个番茄换一次长休息。
// 不依赖硬件，时间由调用方传入，可在电脑上测试。

enum class PomoPhase : uint8_t {
  Idle,        // 还没开始
  Focus,
  ShortBreak,
  LongBreak,
};

// 触摸、按键、设置页面发出的操作
enum class PomoCmd : uint8_t {
  Toggle,  // 开始 / 暂停 / 继续
  Skip,
  Reset,
};

struct PomoConfig {
  uint16_t focus_min = 25;
  uint16_t short_min = 5;
  uint16_t long_min = 15;
  uint8_t long_every = 4;  // 每几个番茄后长休息
};

class Pomodoro {
 public:
  void configure(const PomoConfig &cfg) { cfg_ = cfg; }

  // 没开始时开始专注；运行中暂停；暂停时继续
  void toggle(uint32_t now_ms);

  // 回到初始状态，本轮计数清零（今日完成数保留）
  void reset();

  // 提前结束当前阶段，进入下一阶段；跳过的专注不计入完成数
  void skip(uint32_t now_ms);

  // 每次主循环调用。day 为当天日期标识（如一年中的第几天），变了就清零今日完成数，未对时传 -1。
  // 返回 true 表示刚刚有一个阶段结束：专注结束后自动开始休息；休息结束后下一个专注等用户点开始
  bool tick(uint32_t now_ms, int day);

  PomoPhase phase() const { return phase_; }
  bool running() const { return running_; }
  bool active() const { return phase_ != PomoPhase::Idle; }
  uint32_t remaining_ms(uint32_t now_ms) const;
  uint32_t total_ms() const;
  int done_today() const { return done_today_; }
  int cycle_done() const { return cycle_done_; }  // 本轮已完成几个，0 ~ long_every-1
  const PomoConfig &config() const { return cfg_; }

 private:
  void enter(PomoPhase p, uint32_t now_ms, bool run);

  PomoConfig cfg_;
  PomoPhase phase_ = PomoPhase::Idle;
  bool running_ = false;
  uint32_t end_ms_ = 0;     // 运行中：阶段结束的时刻
  uint32_t paused_ms_ = 0;  // 暂停中：剩余时间
  int done_today_ = 0;
  int cycle_done_ = 0;
  int day_ = -1;
};
