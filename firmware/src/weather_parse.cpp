#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "weather.h"

const char *const kWeatherQuery =
    "&current=temperature_2m,relative_humidity_2m,weather_code"
    "&hourly=precipitation_probability,precipitation,weather_code"
    "&daily=weather_code,temperature_2m_max,temperature_2m_min,precipitation_probability_max,precipitation_sum"
    "&timezone=auto&forecast_days=4&forecast_hours=12";

// "2026-10-20" 是星期几（周日为 0），格式不对时返回 -1
static int weekday_of(const char *date) {
  if (!date || strlen(date) < 10) return -1;
  int y = atoi(date), m = atoi(date + 5), d = atoi(date + 8);
  if (m < 1 || m > 12 || d < 1 || d > 31) return -1;
  // Sakamoto 算法
  static const int t[] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
  if (m < 3) y--;
  return (y + y / 4 - y / 100 + y / 400 + t[m - 1] + d) % 7;
}

bool weather_parse(JsonDocument &doc, WeatherData &out) {
  JsonObject cur = doc["current"];
  JsonObject daily = doc["daily"];
  if (cur.isNull() || daily.isNull() || daily["temperature_2m_max"][0].isNull()) return false;

  out.temp = cur["temperature_2m"] | 0.0f;
  out.humidity = cur["relative_humidity_2m"] | 0;
  out.code = cur["weather_code"] | 0;
  out.temp_max = daily["temperature_2m_max"][0] | 0.0f;
  out.temp_min = daily["temperature_2m_min"][0] | 0.0f;
  out.error = 0;

  out.ndays = 0;
  JsonArray dates = daily["time"];
  for (int i = 0; i < kForecastDays && i < (int)dates.size(); i++) {
    if (daily["temperature_2m_max"][i].isNull()) break;
    DayForecast &d = out.days[out.ndays++];
    d.code = daily["weather_code"][i] | 0;
    d.temp_max = daily["temperature_2m_max"][i] | 0.0f;
    d.temp_min = daily["temperature_2m_min"][i] | 0.0f;
    d.rain_prob = daily["precipitation_probability_max"][i] | 0;
    d.rain_mm = daily["precipitation_sum"][i] | 0.0f;
    d.wday = weekday_of(dates[i]);
  }

  // 逐小时：从现在这个小时开始。请求里限制了 12 个小时，万一返回的是整天的，就按当前时间找起点
  out.nhours = 0;
  JsonObject hourly = doc["hourly"];
  JsonArray times = hourly["time"];
  int start = 0;
  const char *now = cur["time"];  // "2026-10-20T15:15"
  if (now && strlen(now) >= 13) {
    for (int i = 0; i < (int)times.size(); i++) {
      const char *t = times[i];
      if (t && strncmp(t, now, 13) == 0) {
        start = i;
        break;
      }
    }
  }
  for (int i = start; i < (int)times.size() && out.nhours < kRainHours; i++) {
    const char *t = times[i];
    HourRain &h = out.hours[out.nhours++];
    h.hour = t && strlen(t) >= 13 ? atoi(t + 11) : 0;
    h.prob = hourly["precipitation_probability"][i] | 0;
    h.mm = hourly["precipitation"][i] | 0.0f;
    h.code = hourly["weather_code"][i] | 0;
  }
  return true;
}

bool weather_is_wet_code(int code) {
  return code >= 51;  // 51 以后都是毛毛雨、雨、雪、阵雨和雷雨
}

bool weather_hour_wet(const HourRain &h) {
  return h.mm >= 0.2f || (h.prob >= 60 && weather_is_wet_code(h.code));
}

int weather_rain_index(const WeatherData &w) {
  if (w.error) return -1;
  for (int i = 0; i < w.nhours; i++) {
    if (weather_hour_wet(w.hours[i])) return i;
  }
  return -1;
}

bool RainAlert::update(const WeatherData &w, bool quiet) {
  if (w.error || w.nhours == 0) return false;
  bool soon = false;  // 接下来 3 个小时里有雨
  for (int i = 0; i < 3 && i < w.nhours; i++) soon = soon || weather_hour_wet(w.hours[i]);
  if (!soon) {
    in_spell_ = false;
    return false;
  }
  int idx = weather_rain_index(w);
  if (in_spell_ || quiet || idx > 1) return false;  // 下一个小时之后才下，先不提醒
  in_spell_ = true;
  return true;
}

// 下的是什么：天气代码是雨雪时用它的名字（小雨、雷阵雨……），否则就说「雨」
static const char *rain_word(const HourRain &h) {
  return weather_is_wet_code(h.code) ? weather_text(h.code) : "雨";
}

void weather_rain_summary(const WeatherData &w, char *out, size_t size) {
  if (size) out[0] = '\0';
  if (w.error || w.nhours == 0) return;
  int i = weather_rain_index(w);
  if (i < 0) {
    snprintf(out, size, "未来 %d 小时不会下雨", w.nhours);
  } else if (i == 0) {
    int stop = -1;
    for (int k = 1; k < w.nhours && stop < 0; k++) {
      if (!weather_hour_wet(w.hours[k])) stop = k;
    }
    if (stop > 0)
      snprintf(out, size, "正在下%s，大约 %d:00 停", rain_word(w.hours[0]), w.hours[stop].hour);
    else
      snprintf(out, size, "正在下%s，%d 小时内不会停", rain_word(w.hours[0]), w.nhours);
  } else {
    snprintf(out, size, "%d:00 左右开始下%s", w.hours[i].hour, rain_word(w.hours[i]));
  }
}

void weather_rain_short(const WeatherData &w, char *out, size_t size) {
  if (size) out[0] = '\0';
  int i = weather_rain_index(w);
  if (i == 0) snprintf(out, size, "正在下雨");
  else if (i > 0) snprintf(out, size, "%d:00 有雨", w.hours[i].hour);
}

void weather_rain_alert_text(const WeatherData &w, char *out, size_t size) {
  int i = weather_rain_index(w);
  if (i < 0) i = 0;
  const HourRain &h = w.hours[i];
  if (i == 0)
    snprintf(out, size, "外面正在下%s\n出门记得带伞", rain_word(h));
  else
    snprintf(out, size, "大约 %d:00 开始下%s，降水概率 %d%%\n出门记得带伞", h.hour, rain_word(h), h.prob);
}

const char *weather_day_name(const DayForecast &d, int i) {
  static const char *const kNames[] = {"今天", "明天", "后天"};
  static const char *const kWeek[] = {"周日", "周一", "周二", "周三", "周四", "周五", "周六"};
  if (i >= 0 && i < 3) return kNames[i];
  return d.wday >= 0 && d.wday < 7 ? kWeek[d.wday] : "";
}
