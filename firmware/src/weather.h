#pragma once

#include <Arduino.h>

struct WeatherData {
  float temp;      // 当前气温 °C
  int humidity;    // 相对湿度 %
  int code;        // WMO 天气代码
  float temp_max;  // 今日最高
  float temp_min;  // 今日最低
};

// 启动后台任务，每 15 分钟从 Open-Meteo 拉取一次天气（免费，无需 API Key）
void weather_begin();

// 设置变更后调用，立即重新拉取
void weather_request_refresh();

// 有新数据时返回 true 并写入 out，只在主循环中调用
bool weather_take_update(WeatherData &out);

const char *weather_text(int code);
uint32_t weather_color(int code);  // 0xRRGGBB
