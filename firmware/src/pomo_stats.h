#pragma once

#include <stdint.h>

// 番茄钟完成数统计：最近 7 天每天完成几个，重启不丢。
// 不依赖硬件，日期由调用方传入，可在电脑上测试。数据结构可以直接整块存进 NVS

static constexpr int kPomoStatDays = 7;

struct PomoStatsData {
  int32_t last_day = -1;              // counts[0] 对应的日期（1970-01-01 起第几天），-1 表示还没有记录
  uint8_t counts[kPomoStatDays] = {};  // counts[i] 是 last_day 往前第 i 天
};

class PomoStats {
 public:
  void load(const PomoStatsData &d) { d_ = d; }
  const PomoStatsData &data() const { return d_; }

  // 完成一个番茄，未对时（day < 0）不记录
  void record(int day);

  // 某一天完成几个，超出最近 7 天返回 0
  int count(int day) const;

  // 从 day 往前数 7 天（含 day）一共完成几个
  int week_total(int day) const;

 private:
  void advance(int day);

  PomoStatsData d_;
};
