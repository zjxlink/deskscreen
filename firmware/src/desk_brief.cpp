#include "desk_brief.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>

static const char *const kWeekdays[] = {"周日", "周一", "周二", "周三", "周四", "周五", "周六"};

static void add(std::string &out, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void add(std::string &out, const char *fmt, ...) {
  char buf[160];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  out += buf;
}

static void weather_lines(std::string &out, const WeatherData &w) {
  add(out, "天气：现在 %.0f°C，%s，湿度 %d%%", w.temp, weather_text(w.code), w.humidity);
  for (int i = 0; i < w.ndays && i < kForecastDays; i++) {
    const DayForecast &d = w.days[i];
    const char *name = i == 0 ? "今天" : i == 1 ? "明天" : i == 2 ? "后天" : kWeekdays[d.wday % 7];
    add(out, "；%s%s %.0f~%.0f°C", name, weather_text(d.code), roundf(d.temp_min), roundf(d.temp_max));
    if (d.rain_prob > 0) add(out, " 降水概率 %d%%", d.rain_prob);
  }
  out += "。\n";
  char rain[96];
  weather_rain_summary(w, rain, sizeof(rain));
  if (rain[0]) add(out, "接下来几小时：%s。\n", rain);
}

static void pomo_line(std::string &out, const Pomodoro &p, uint32_t now_ms, int today) {
  if (!p.active()) {
    if (today > 0) add(out, "番茄钟：没在计时，今天完成了 %d 个。\n", today);
    return;
  }
  const char *phase = p.phase() == PomoPhase::Focus ? "专注" : p.phase() == PomoPhase::ShortBreak ? "短休息" : "长休息";
  unsigned left = (p.remaining_ms(now_ms) + 59999) / 60000;
  add(out, "番茄钟：%s%s，还剩 %u 分钟，今天完成了 %d 个。\n", phase, p.running() ? "中" : "（暂停了）", left, today);
}

static void stock_line(std::string &out, const StockData &s) {
  if (s.status != StockStatus::Ok || s.count == 0) return;
  add(out, "自选股（%s 更新）：", s.updated);
  bool first = true;
  for (int i = 0; i < s.count && i < kMaxStocks; i++) {
    const StockQuote &q = s.quotes[i];
    if (!q.valid) continue;
    add(out, "%s%s %.2f（%+.2f%%）", first ? "" : "；", q.name, q.price, q.change_pct);
    first = false;
  }
  out += "。\n";
}

std::string desk_brief(const DeskInfo &d) {
  std::string out;
  if (d.weather && !d.weather->error) weather_lines(out, *d.weather);
  if (d.room_valid) add(out, "室内：%.1f°C，湿度 %.0f%%。\n", d.room_temp, d.room_humidity);
  if (d.pomo) pomo_line(out, *d.pomo, d.now_ms, d.pomo_today);
  if (d.stocks) stock_line(out, *d.stocks);
  if (d.github && d.github->status == GithubStatus::Ok) {
    add(out, "GitHub：今天 %u 次贡献，已经连续 %u 天，过去一年共 %u 次。\n", d.github->today, d.github->streak,
        d.github->total);
  }
  return out;
}
