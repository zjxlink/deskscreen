#include "astro.h"

#include <math.h>
#include <string.h>

static constexpr double kSynodicMonth = 29.530588853;  // 朔望月（天）
static constexpr double kNewMoonJd = 2451550.26;       // 2000-01-06 18:14 UTC 的新月

// 公历日期中午的儒略日
static double julian_day(int y, int m, int d) {
  if (m <= 2) {
    y--;
    m += 12;
  }
  int a = y / 100;
  int b = 2 - a + a / 4;
  return floor(365.25 * (y + 4716)) + floor(30.6001 * (m + 1)) + d + b - 1524.5 + 0.5;
}

float moon_phase(int year, int month, int day) {
  double age = fmod(julian_day(year, month, day) - kNewMoonJd, kSynodicMonth);
  if (age < 0) age += kSynodicMonth;
  return (float)(age / kSynodicMonth);
}

const char *moon_phase_name(float p) {
  // 新月、上弦、满月、下弦前后各一天算正点，中间是蛾眉、盈凸、亏凸、残月
  const float day = (float)(1 / kSynodicMonth);
  if (p < day || p > 1 - day) return "新月";
  if (p < 0.25f - day) return "蛾眉月";
  if (p <= 0.25f + day) return "上弦月";
  if (p < 0.5f - day) return "盈凸月";
  if (p <= 0.5f + day) return "满月";
  if (p < 0.75f - day) return "亏凸月";
  if (p <= 0.75f + day) return "下弦月";
  return "残月";
}

bool moon_lit(float p, float x, float y, float r) {
  if (x * x + y * y > r * r) return false;
  // 这一行月亮的半宽，明暗交界线在 w*cos(2πp) 处
  float w = sqrtf(r * r - y * y);
  if (p < 0.5f) return x > w * cosf(2 * (float)M_PI * p);  // 盈：右边亮，越来越多
  return x < w * cosf(2 * (float)M_PI * (p - 0.5f));       // 亏：左边亮，越来越少
}

int astro_parse_time(const char *iso) {
  if (!iso || strlen(iso) < 16 || iso[10] != 'T' || iso[13] != ':') return -1;
  int h = (iso[11] - '0') * 10 + (iso[12] - '0');
  int m = (iso[14] - '0') * 10 + (iso[15] - '0');
  if (h < 0 || h > 23 || m < 0 || m > 59) return -1;
  return h * 60 + m;
}
