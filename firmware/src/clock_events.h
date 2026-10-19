#pragma once

#include <stdint.h>

// 整点报时和闹钟：到点触发一次。不依赖硬件，日期和时间由调用方传入，可在电脑上测试。
// day 为 1970-01-01 起第几天，minutes 为当天第几分钟，wday 为星期几（周日为 0），未对时 day 传 -1

struct AlarmConfig {
  bool on = false;
  uint16_t time = 7 * 60 + 30;  // 当天第几分钟
  uint8_t days = 0x3E;          // 第 i 位表示星期 i 响，周日为第 0 位；默认周一到周五
};

class Alarm {
 public:
  void configure(const AlarmConfig &cfg) { cfg_ = cfg; }

  // 到了闹钟时间返回 true，同一天只响一次
  bool tick(int day, int minutes, int wday);

  const AlarmConfig &config() const { return cfg_; }

 private:
  AlarmConfig cfg_;
  int fired_day_ = -1;
};

class HourlyChime {
 public:
  void set_on(bool on) { on_ = on; }

  // 每到整点返回 true 一次
  bool tick(int day, int minutes);

 private:
  bool on_ = false;
  int32_t fired_ = -1;  // 上次报时的 day * 24 + 小时
};
