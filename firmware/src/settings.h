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
  bool chime_on;             // 整点报时（夜间时段跟提示音一样默认不响）
  bool alarm_on;             // 闹钟
  uint16_t alarm_time;       // 当天第几分钟，默认 7:30
  uint8_t alarm_days;        // 第 i 位表示星期 i 响，周日为第 0 位，默认周一到周五
  // 以下只在插了 SENSOR 底座时有用
  bool climate_on;           // 太热、太闷时提醒
  uint8_t hot_c;             // 温度达到多少度提醒，默认 30
  uint8_t humid_pct;         // 湿度达到多少提醒，默认 80%
  int8_t temp_offset;        // 温度校准，单位 0.1°C，测得的温度加上它再显示
  bool away_off;             // 没人时熄屏，有人回来再亮
  uint8_t away_min;          // 多少分钟没人算离开，默认 5
  // AI 语音聊天（阿里云百炼）
  String ai_key;             // 百炼 API Key，空表示不用
  String ai_model;           // 默认 qwen3-omni-flash
  String ai_voice;           // 音色，默认 Cherry
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

// 小游戏最高分
int settings_load_game_best();
void settings_save_game_best(int best);
