#include "clock_events.h"

bool Alarm::tick(int day, int minutes, int wday) {
  if (!cfg_.on || day < 0 || minutes != cfg_.time || fired_day_ == day) return false;
  if (wday < 0 || wday > 6 || !(cfg_.days & (1 << wday))) return false;
  fired_day_ = day;
  return true;
}

bool HourlyChime::tick(int day, int minutes) {
  if (!on_ || day < 0 || minutes < 0 || minutes % 60 != 0) return false;
  int32_t key = (int32_t)day * 24 + minutes / 60;
  if (key == fired_) return false;
  fired_ = key;
  return true;
}
