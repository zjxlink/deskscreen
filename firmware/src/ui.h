#pragma once

#include "weather.h"

void ui_init();

// 全屏提示页，detail 可以包含换行
void ui_show_status(const char *title, const char *detail);

// 首次配网提示页
void ui_show_provision(const char *ap_name);

void ui_show_main();

// 每次主循环调用，秒数变化时才真正刷新
void ui_tick_clock();

void ui_set_city(const char *city);
void ui_set_weather(const WeatherData &w);
void ui_set_wifi(bool connected);

// 立即把当前界面画到屏幕上（主循环阻塞前使用）
void ui_refresh_now();
