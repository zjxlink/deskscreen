#pragma once

#include <Arduino.h>

struct Settings {
  String city;         // 只用于屏幕显示
  float lat;
  float lon;
  uint8_t brightness;  // 10~255
  String github_token;
  String stocks;         // 自选股代码，逗号分隔
  uint8_t page_seconds;  // 自动切换页面的间隔，0 表示不自动切换
  bool night_on;           // 夜间自动调暗
  uint16_t night_start;    // 当天第几分钟，默认 23:00
  uint16_t night_end;      // 默认 07:00
  uint8_t night_brightness;  // 0 表示关闭背光
  bool auto_update;          // 发现新版本后在凌晨 3~5 点自动安装
};

// 时区固定为北京时间，POSIX 格式
#define TIMEZONE "CST-8"

void settings_begin();

// 返回当前设置的副本，可在任意任务中调用
Settings settings_get();

void settings_save(const Settings &s);
