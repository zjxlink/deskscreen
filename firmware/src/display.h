#pragma once

#include <stdint.h>

// 初始化屏幕和 LVGL，之后即可创建 LVGL 控件
void display_init();

void display_set_brightness(uint8_t level);
