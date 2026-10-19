#pragma once

#include <stdint.h>

// 初始化屏幕和 LVGL，之后即可创建 LVGL 控件
void display_init();

void display_set_brightness(uint8_t level);

// 屏幕当前是否被按住，按住时给出坐标（没有触摸屏的板子始终返回 false）
bool display_touch(int &x, int &y);

// 主循环调用：屏幕控制芯片被意外复位（白屏、花屏）时重新初始化并整屏重画，见 panel_watch.h；
// 另外每分钟整屏重画一次
void display_check_panel();

// 能否读出屏幕状态；读不出时改为每 10 分钟重新初始化一次
bool display_panel_watching();

// 开机以来检测到白屏、花屏并恢复的次数
uint32_t display_panel_recoveries();
