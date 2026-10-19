#pragma once

#include "github.h"
#include "pages.h"
#include "stock.h"
#include "weather.h"

void ui_init();

// 全屏提示页，detail 可以包含换行
void ui_show_status(const char *title, const char *detail);

// 带进度条的全屏提示页（升级固件用），percent 为 0~100
void ui_show_progress(const char *title, const char *detail, int percent);

// 首次配网提示页
void ui_show_provision(const char *ap_name);

// 切换到某个页面，animate 为 true 时有左滑动画
void ui_show_page(Page page, bool animate);

// 每次主循环调用，秒数变化时才真正刷新
void ui_tick_clock();

void ui_set_city(const char *city);
void ui_set_weather(const WeatherData &w);
void ui_set_github(const GithubData &g);
void ui_set_stocks(const StockData &s);
void ui_set_wifi(bool connected);

// 立即把当前界面画到屏幕上（主循环阻塞前使用）
void ui_refresh_now();
