#pragma once

#include <stdint.h>

// 初始化屏幕和 LVGL，之后即可创建 LVGL 控件
void display_init();

void display_set_brightness(uint8_t level);

// 屏幕当前是否被按住，按住时给出坐标（没有触摸屏的板子始终返回 false）
bool display_touch(int &x, int &y);
