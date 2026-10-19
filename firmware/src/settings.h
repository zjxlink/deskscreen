#pragma once

#include <Arduino.h>

#include "pomo_stats.h"

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
  uint8_t pomo_focus;        // 番茄钟专注分钟数
  uint8_t pomo_short;        // 短休息分钟数
  uint8_t pomo_long;         // 长休息分钟数
  uint8_t pomo_every;        // 每几个番茄后长休息
  bool sound_on;             // 番茄钟提示音
  uint8_t sound_volume;      // 0~100
  bool sound_quiet_night;    // 夜间时段不响
  bool sit_on;               // 久坐提醒
  uint8_t sit_min;           // 久坐提醒间隔（分钟）
  bool water_on;             // 喝水提醒
  uint8_t water_min;         // 喝水提醒间隔（分钟）
  uint16_t remind_start;     // 提醒时段，当天第几分钟，默认 9:00
  uint16_t remind_end;       // 默认 21:00
};

// 时区固定为北京时间，POSIX 格式
#define TIMEZONE "CST-8"

void settings_begin();

// 返回当前设置的副本，可在任意任务中调用
Settings settings_get();

void settings_save(const Settings &s);

// 番茄钟统计单独存一块，只在主循环里读写
void settings_load_pomo_stats(PomoStatsData &d);
void settings_save_pomo_stats(const PomoStatsData &d);
