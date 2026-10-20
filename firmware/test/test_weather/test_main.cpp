// 天气预报和下雨提醒的单元测试：在电脑上运行：pio test -e native
#include <ArduinoJson.h>
#include <string.h>
#include <unity.h>

#include "weather.h"

void setUp() {}
void tearDown() {}

// Open-Meteo 返回的样子（按文档 https://open-meteo.com/en/docs 的格式写的，省掉了 units 等字段）
static const char *kSample = R"({
  "timezone": "Asia/Shanghai",
  "current": {"time": "2026-10-20T15:15", "interval": 900, "temperature_2m": 18.6,
              "relative_humidity_2m": 72, "weather_code": 3},
  "hourly": {
    "time": ["2026-10-20T15:00","2026-10-20T16:00","2026-10-20T17:00","2026-10-20T18:00",
             "2026-10-20T19:00","2026-10-20T20:00","2026-10-20T21:00","2026-10-20T22:00",
             "2026-10-20T23:00","2026-10-21T00:00","2026-10-21T01:00","2026-10-21T02:00"],
    "precipitation_probability": [10, 35, 70, 85, 60, 20, 5, 0, 0, null, 0, 0],
    "precipitation": [0.0, 0.0, 0.6, 2.1, 0.3, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0],
    "weather_code": [3, 3, 61, 63, 61, 3, 2, 1, 0, 0, 0, 0]
  },
  "daily": {
    "time": ["2026-10-20","2026-10-21","2026-10-22","2026-10-23"],
    "weather_code": [63, 2, 80, 0],
    "temperature_2m_max": [21.4, 23.0, 19.2, 24.8],
    "temperature_2m_min": [15.1, 14.0, 13.6, 15.9],
    "precipitation_probability_max": [85, 10, 65, 0],
    "precipitation_sum": [3.0, 0.0, 4.2, 0.0],
    "sunrise": ["2026-10-20T06:04","2026-10-21T06:05","2026-10-22T06:06","2026-10-23T06:06"],
    "sunset": ["2026-10-20T17:21","2026-10-21T17:20","2026-10-22T17:19","2026-10-23T17:18"]
  }
})";

static WeatherData parse(const char *json) {
  JsonDocument doc;
  TEST_ASSERT_FALSE(deserializeJson(doc, json));
  WeatherData w = {};
  w.error = 99;
  TEST_ASSERT_TRUE(weather_parse(doc, w));
  return w;
}

static void test_parse_current_and_days() {
  WeatherData w = parse(kSample);
  TEST_ASSERT_EQUAL(0, w.error);
  TEST_ASSERT_FLOAT_WITHIN(0.01, 18.6, w.temp);
  TEST_ASSERT_EQUAL(72, w.humidity);
  TEST_ASSERT_EQUAL(3, w.code);
  TEST_ASSERT_FLOAT_WITHIN(0.01, 21.4, w.temp_max);
  TEST_ASSERT_FLOAT_WITHIN(0.01, 15.1, w.temp_min);
  TEST_ASSERT_EQUAL(4, w.ndays);
  TEST_ASSERT_EQUAL(2, w.days[0].wday);  // 2026-10-20 是周二
  TEST_ASSERT_EQUAL(3, w.days[1].wday);
  TEST_ASSERT_EQUAL(5, w.days[3].wday);
  TEST_ASSERT_EQUAL(80, w.days[2].code);
  TEST_ASSERT_EQUAL(65, w.days[2].rain_prob);
  TEST_ASSERT_FLOAT_WITHIN(0.01, 4.2, w.days[2].rain_mm);
  TEST_ASSERT_FLOAT_WITHIN(0.01, 24.8, w.days[3].temp_max);
  TEST_ASSERT_FLOAT_WITHIN(0.01, 13.6, w.days[2].temp_min);
  TEST_ASSERT_EQUAL(6 * 60 + 4, w.days[0].sunrise);
  TEST_ASSERT_EQUAL(17 * 60 + 21, w.days[0].sunset);
  TEST_ASSERT_EQUAL(17 * 60 + 18, w.days[3].sunset);
}

static void test_parse_hours() {
  WeatherData w = parse(kSample);
  TEST_ASSERT_EQUAL(12, w.nhours);
  TEST_ASSERT_EQUAL(15, w.hours[0].hour);
  TEST_ASSERT_EQUAL(0, w.hours[9].hour);
  TEST_ASSERT_EQUAL(0, w.hours[9].prob);  // null 当成 0
  TEST_ASSERT_EQUAL(85, w.hours[3].prob);
  TEST_ASSERT_FLOAT_WITHIN(0.01, 2.1, w.hours[3].mm);
  TEST_ASSERT_EQUAL(63, w.hours[3].code);
}

// 返回的逐小时数据要是从半夜开始的（没按 forecast_hours 截），从现在这个小时开始取
static void test_parse_hours_from_now() {
  JsonDocument doc;
  deserializeJson(doc, kSample);
  doc["current"]["time"] = "2026-10-20T18:45";
  WeatherData w = {};
  TEST_ASSERT_TRUE(weather_parse(doc, w));
  TEST_ASSERT_EQUAL(9, w.nhours);  // 18 点到 2 点
  TEST_ASSERT_EQUAL(18, w.hours[0].hour);
  TEST_ASSERT_EQUAL(85, w.hours[0].prob);
}

static void test_parse_missing() {
  JsonDocument doc;
  deserializeJson(doc, R"({"current":{"temperature_2m":1}})");
  WeatherData w = {};
  TEST_ASSERT_FALSE(weather_parse(doc, w));
  deserializeJson(doc, R"({"error":true,"reason":"Cannot initialize WeatherVariable"})");
  TEST_ASSERT_FALSE(weather_parse(doc, w));
  // 没有逐小时数据也能用，只是没有下雨提醒
  deserializeJson(doc, R"({"current":{"temperature_2m":1},"daily":{"time":["2026-10-20"],"temperature_2m_max":[5],"temperature_2m_min":[1]}})");
  TEST_ASSERT_TRUE(weather_parse(doc, w));
  TEST_ASSERT_EQUAL(1, w.ndays);
  TEST_ASSERT_EQUAL(0, w.nhours);
  TEST_ASSERT_EQUAL(-1, weather_rain_index(w));
  TEST_ASSERT_EQUAL(-1, w.days[0].sunset);  // 没有日出日落，夜间模式按设定的时间
}

static void test_wet() {
  TEST_ASSERT_FALSE(weather_hour_wet({15, 10, 0.0f, 3}));
  TEST_ASSERT_FALSE(weather_hour_wet({15, 90, 0.0f, 3}));  // 概率高但天气代码是阴，不算
  TEST_ASSERT_TRUE(weather_hour_wet({15, 60, 0.0f, 61}));
  TEST_ASSERT_TRUE(weather_hour_wet({15, 0, 0.2f, 3}));
  TEST_ASSERT_FALSE(weather_hour_wet({15, 59, 0.1f, 80}));
  TEST_ASSERT_FALSE(weather_is_wet_code(48));
  TEST_ASSERT_TRUE(weather_is_wet_code(95));
  WeatherData w = parse(kSample);
  TEST_ASSERT_EQUAL(2, weather_rain_index(w));  // 17 点开始下
  w.error = -1;
  TEST_ASSERT_EQUAL(-1, weather_rain_index(w));
}

// 把 hours 设成：wet 里为 true 的小时下雨
static WeatherData hours(const bool *wet, int n) {
  WeatherData w = {};
  w.nhours = n;
  for (int i = 0; i < n; i++) w.hours[i] = {i, wet[i] ? 80 : 0, wet[i] ? 1.0f : 0.0f, wet[i] ? 61 : 1};
  return w;
}

static void test_rain_alert() {
  RainAlert a;
  const bool later[] = {false, false, true, true};   // 两小时后才下
  const bool next[] = {false, true, true, false};    // 下一个小时下
  const bool now[] = {true, true, false, false};     // 正在下
  const bool dry[] = {false, false, false, false};
  TEST_ASSERT_FALSE(a.update(hours(later, 4), false));
  TEST_ASSERT_TRUE(a.update(hours(next, 4), false));   // 快下了，提醒一次
  TEST_ASSERT_FALSE(a.update(hours(now, 4), false));   // 同一场雨不再提醒
  TEST_ASSERT_FALSE(a.update(hours(later, 4), false));  // 雨停了一会儿，3 小时内还有雨，算同一场
  TEST_ASSERT_FALSE(a.update(hours(dry, 4), false));    // 雨停了
  TEST_ASSERT_TRUE(a.update(hours(now, 4), false));     // 新的一场
}

static void test_rain_alert_quiet() {
  RainAlert a;
  const bool now[] = {true, true, true};
  TEST_ASSERT_FALSE(a.update(hours(now, 3), true));   // 夜里不提醒
  TEST_ASSERT_FALSE(a.update(hours(now, 3), true));
  TEST_ASSERT_TRUE(a.update(hours(now, 3), false));   // 到了白天还在下，补一次
  TEST_ASSERT_FALSE(a.update(hours(now, 3), false));
  a.reset();
  TEST_ASSERT_TRUE(a.update(hours(now, 3), false));
  WeatherData bad = hours(now, 3);
  bad.error = -1;
  a.reset();
  TEST_ASSERT_FALSE(a.update(bad, false));  // 获取失败时不提醒
}

static void test_summary() {
  char buf[96];
  WeatherData w = parse(kSample);
  weather_rain_summary(w, buf, sizeof(buf));
  TEST_ASSERT_EQUAL_STRING("17:00 左右开始下小雨", buf);
  weather_rain_short(w, buf, sizeof(buf));
  TEST_ASSERT_EQUAL_STRING("17:00 有雨", buf);
  weather_rain_alert_text(w, buf, sizeof(buf));
  TEST_ASSERT_EQUAL_STRING("大约 17:00 开始下小雨，降水概率 70%\n出门记得带伞", buf);

  // 从 18 点看：正在下中雨，20 点停
  JsonDocument doc;
  deserializeJson(doc, kSample);
  doc["current"]["time"] = "2026-10-20T18:10";
  weather_parse(doc, w);
  weather_rain_summary(w, buf, sizeof(buf));
  TEST_ASSERT_EQUAL_STRING("正在下中雨，大约 20:00 停", buf);
  weather_rain_short(w, buf, sizeof(buf));
  TEST_ASSERT_EQUAL_STRING("正在下雨", buf);
  weather_rain_alert_text(w, buf, sizeof(buf));
  TEST_ASSERT_EQUAL_STRING("外面正在下中雨\n出门记得带伞", buf);

  // 一直下
  const bool wet[] = {true, true, true};
  w = hours(wet, 3);
  weather_rain_summary(w, buf, sizeof(buf));
  TEST_ASSERT_EQUAL_STRING("正在下小雨，3 小时内不会停", buf);

  // 不下雨
  const bool dry[] = {false, false};
  w = hours(dry, 2);
  weather_rain_summary(w, buf, sizeof(buf));
  TEST_ASSERT_EQUAL_STRING("未来 2 小时不会下雨", buf);
  weather_rain_short(w, buf, sizeof(buf));
  TEST_ASSERT_EQUAL_STRING("", buf);
  w.nhours = 0;
  weather_rain_summary(w, buf, sizeof(buf));
  TEST_ASSERT_EQUAL_STRING("", buf);

  // 降水量够了但天气代码不是雨，就说「雨」
  HourRain h = {9, 30, 0.5f, 3};
  w.nhours = 2;
  w.hours[0] = {8, 0, 0.0f, 1};
  w.hours[1] = h;
  weather_rain_summary(w, buf, sizeof(buf));
  TEST_ASSERT_EQUAL_STRING("9:00 左右开始下雨", buf);
}

static void test_day_names() {
  WeatherData w = parse(kSample);
  TEST_ASSERT_EQUAL_STRING("今天", weather_day_name(w.days[0], 0));
  TEST_ASSERT_EQUAL_STRING("明天", weather_day_name(w.days[1], 1));
  TEST_ASSERT_EQUAL_STRING("后天", weather_day_name(w.days[2], 2));
  TEST_ASSERT_EQUAL_STRING("周五", weather_day_name(w.days[3], 3));
}

static void test_text() {
  TEST_ASSERT_EQUAL_STRING("小雨", weather_text(61));
  TEST_ASSERT_EQUAL_STRING("阵雨", weather_text(80));
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_parse_current_and_days);
  RUN_TEST(test_parse_hours);
  RUN_TEST(test_parse_hours_from_now);
  RUN_TEST(test_parse_missing);
  RUN_TEST(test_wet);
  RUN_TEST(test_rain_alert);
  RUN_TEST(test_rain_alert_quiet);
  RUN_TEST(test_summary);
  RUN_TEST(test_day_names);
  RUN_TEST(test_text);
  return UNITY_END();
}
