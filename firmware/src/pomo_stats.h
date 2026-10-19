#pragma once

#include <stdint.h>

// 番茄钟统计：最近 7 天每天完成几个番茄，以及连续达标天数。
// 不依赖硬件，日期由调用方传入，可在电脑上测试。数据结构可以直接整块存进 NVS

static constexpr int kPomoStatDays = 7;

struct PomoStatsData {
  int32_t last_day = -1;              // counts[0] 对应的日期（1970-01-01 起第几天），-1 表示还没有记录
  uint8_t counts[kPomoStatDays] = {};  // counts[i] 是 last_day 往前第 i 天
  uint16_t streak = 0;                // 截至 streak_day 连续达标的天数
  int32_t streak_day = -1;            // 最近一次达标的日期
};

class PomoStats {
 public:
  void load(const PomoStatsData &d) { d_ = d; }
  const PomoStatsData &data() const { return d_; }

  // 完成一个番茄。goal 为每日目标，当天第一次达到目标时连续天数加一
  void record(int day, int goal);

  // 某一天完成几个，超出最近 7 天返回 0
  int count(int day) const;

  // 从 day 往前数 7 天（含 day）一共完成几个
  int week_total(int day) const;

  // 连续达标天数：今天或昨天达标才算连续，更早就断了
  int streak(int day) const;

  // 最近 7 天有没有任何记录（决定统计页要不要参与轮播）
  bool any(int day) const { return week_total(day) > 0; }

 private:
  void advance(int day);

  PomoStatsData d_;
};
