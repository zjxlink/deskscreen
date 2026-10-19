#include "pomo_stats.h"

// 窗口移到 day：中间空出来的日子记 0。日期往回走（改了时间）就从头开始
void PomoStats::advance(int day) {
  if (d_.last_day < 0 || day < d_.last_day || day - d_.last_day >= kPomoStatDays) {
    for (uint8_t &c : d_.counts) c = 0;
  } else {
    int shift = day - d_.last_day;
    for (int i = kPomoStatDays - 1; i >= 0; i--) d_.counts[i] = i >= shift ? d_.counts[i - shift] : 0;
  }
  d_.last_day = day;
}

void PomoStats::record(int day) {
  if (day < 0) return;
  if (day != d_.last_day) advance(day);
  if (d_.counts[0] < 255) d_.counts[0]++;
}

int PomoStats::count(int day) const {
  int ago = d_.last_day - day;
  if (d_.last_day < 0 || ago < 0 || ago >= kPomoStatDays) return 0;
  return d_.counts[ago];
}

int PomoStats::week_total(int day) const {
  int total = 0;
  for (int i = 0; i < kPomoStatDays; i++) total += count(day - i);
  return total;
}
