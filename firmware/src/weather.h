#pragma once

#include <ArduinoJson.h>
#include <stddef.h>
#include <stdint.h>

static constexpr int kForecastDays = 4;  // 今天 + 未来 3 天
static constexpr int kRainHours = 12;    // 逐小时降水看未来 12 小时

struct DayForecast {
  int code;        // WMO 天气代码
  float temp_max;
  float temp_min;
  int rain_prob;   // 当天最大降水概率 %
  float rain_mm;   // 当天降水量 mm
  int wday;        // 星期几，周日为 0
  int sunrise;     // 日出，当天第几分钟（当地时间），没有数据时为 -1
  int sunset;      // 日落
};

struct HourRain {
  int hour;   // 几点（0~23）
  int prob;   // 降水概率 %
  float mm;   // 这一小时的降水量 mm
  int code;   // WMO 天气代码
};

struct WeatherData {
  float temp;      // 当前气温 °C
  int humidity;    // 相对湿度 %
  int code;        // WMO 天气代码
  float temp_max;  // 今日最高
  float temp_min;  // 今日最低
  int error;       // 0 表示成功；否则是失败原因，见 weather.cpp 的 fetch()
  DayForecast days[kForecastDays];  // days[0] 是今天
  int ndays;
  HourRain hours[kRainHours];       // hours[0] 是现在这个小时
  int nhours;
};

// 启动后台任务，每 15 分钟从 Open-Meteo 拉取一次天气（免费，无需 API Key）
void weather_begin();

// 设置变更后调用，立即重新拉取
void weather_request_refresh();

// 有新数据时返回 true 并写入 out，只在主循环中调用
bool weather_take_update(WeatherData &out);

const char *weather_text(int code);
uint32_t weather_color(int code);  // 0xRRGGBB
uint32_t weather_color_light(int code);  // 浅色主题用，白底上调深一点

// 以下不依赖硬件，可在电脑上测试（weather_parse.cpp）

// Open-Meteo 请求的参数（加在经纬度后面）
extern const char *const kWeatherQuery;

// 解析 Open-Meteo 的返回，成功时填好 out（error 置 0）。缺少当前天气或每日预报时返回 false
bool weather_parse(JsonDocument &doc, WeatherData &out);

// 这个天气代码是不是下雨、下雪或雷雨
bool weather_is_wet_code(int code);

// 这一小时算不算下雨：降水量至少 0.2mm，或者降水概率至少 60% 且天气代码是雨雪
bool weather_hour_wet(const HourRain &h);

// 未来几小时里第一个下雨的小时（hours 的下标），12 小时内都不下雨时返回 -1
int weather_rain_index(const WeatherData &w);

// 天气预报页上的一句话：「17:00 左右开始下小雨」「正在下小雨，大约 20:00 停」
// 「未来 12 小时不会下雨」。没有逐小时数据时写空字符串
void weather_rain_summary(const WeatherData &w, char *out, size_t size);

// 时钟页天气卡片上的短提示：「17:00 有雨」「正在下雨」，不下雨时写空字符串
void weather_rain_short(const WeatherData &w, char *out, size_t size);

// 下雨提醒弹窗的正文（快下雨或正在下时才有意义）
void weather_rain_alert_text(const WeatherData &w, char *out, size_t size);

// 预报里第 i 天的名字：今天、明天、后天，再往后是「周五」
const char *weather_day_name(const DayForecast &d, int i);

// 下雨提醒：快下雨时（现在或下一个小时）提醒一次，同一场雨不重复提醒；
// 之后接下来 3 个小时都不下雨，算这场雨结束了，下一场再提醒
class RainAlert {
 public:
  // 每次拿到新的天气数据时调用。quiet 为 true 时（夜间、提醒时段外）先不提醒，
  // 到了能提醒的时候雨还在下就补一次（早上出门前也能看到）。返回 true 表示现在提醒
  bool update(const WeatherData &w, bool quiet);

  // 关掉提醒或者换了城市时调用，重新开始
  void reset() { in_spell_ = false; }

 private:
  bool in_spell_ = false;
};
