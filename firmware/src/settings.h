#pragma once

#include <Arduino.h>

struct Settings {
  String city;         // 只用于屏幕显示
  float lat;
  float lon;
  uint8_t brightness;  // 10~255
};

// 时区固定为北京时间，POSIX 格式
#define TIMEZONE "CST-8"

void settings_begin();

// 返回当前设置的副本，可在任意任务中调用
Settings settings_get();

void settings_save(const Settings &s);
