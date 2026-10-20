#pragma once

#include "github.h"
#include "pages.h"
#include "pet.h"
#include "pomodoro.h"
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

// 时钟页上的室内温湿度（SENSOR 底座），超过阈值的那一项显示成橙色；没调用过就不显示
void ui_set_room(float temp, float humidity, bool hot, bool humid);
void ui_set_weather(const WeatherData &w);
void ui_set_github(const GithubData &g);
void ui_set_stocks(const StockData &s);

// 番茄钟页：msg 不为空时在顶栏显示提示（如"专注完成，休息一下吧"）；done_today 为今日完成数（来自统计，重启不丢）
void ui_set_pomodoro(const Pomodoro &p, uint32_t now_ms, const char *msg, int done_today);

// 像素小猫页：心情或眨眼变化时重画小猫；line 是小猫说的话，status 是底部的一行小字
void ui_set_pet(PetMood mood, bool blink, const char *line, const char *status);

// 小猫的动画：bob 为上下浮动的像素数，zzz 为睡觉时"Zzz"的帧（0 或 1），不睡觉时传 -1
void ui_pet_anim(int bob, int zzz);

// 全屏提醒（留言、久坐、喝水、闹钟），盖在当前页面上，关掉后用 ui_show_page 回到原来的页面
enum class AlertIcon { Message, Sit, Water, Alarm, Climate };
void ui_show_alert(AlertIcon icon, const char *title, const char *body, const char *foot);

// 触摸点是否落在番茄钟的圆环上（圆环居中，屏幕上下或左右翻转时判断结果也不变）
bool ui_pomodoro_hit(int x, int y);
void ui_set_wifi(bool connected);

// 立即把当前界面画到屏幕上（主循环阻塞前使用）
void ui_refresh_now();
