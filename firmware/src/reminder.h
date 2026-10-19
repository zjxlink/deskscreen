#pragma once

#include <stdint.h>

// 定时提醒（久坐、喝水）：在每天的提醒时段内，每隔一段时间提醒一次。
// 不依赖硬件，时间由调用方传入，可在电脑上测试。

struct ReminderConfig {
  bool on = false;
  uint16_t interval_min = 45;
  uint16_t start = 9 * 60;  // 提醒时段，当天第几分钟，支持跨午夜
  uint16_t end = 21 * 60;

  bool operator==(const ReminderConfig &o) const {
    return on == o.on && interval_min == o.interval_min && start == o.start && end == o.end;
  }
};

class Reminder {
 public:
  // 设置没变就保持计时；变了从现在重新计时
  void configure(const ReminderConfig &cfg);

  // 主循环调用。minutes 为当天第几分钟，未对时传 -1。
  // hold 为 true 时（番茄钟专注中）到点也先不提醒，等 hold 解除后再提醒。返回 true 表示该提醒了
  bool tick(uint32_t now_ms, int minutes, bool hold);

  // 从现在重新计时（例如番茄钟开始休息，已经起身了）
  void restart(uint32_t now_ms);

  // 是否在计时（提醒时段内且已打开）
  bool armed() const { return armed_; }

  // 距离下次提醒还有多久，没在计时返回 0
  uint32_t remaining_ms(uint32_t now_ms) const;

  const ReminderConfig &config() const { return cfg_; }

 private:
  uint32_t interval_ms() const { return (uint32_t)cfg_.interval_min * 60 * 1000; }

  ReminderConfig cfg_;
  bool armed_ = false;
  uint32_t due_ms_ = 0;
};
