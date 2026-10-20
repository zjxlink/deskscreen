#pragma once

#include <lvgl.h>

// 切换屏幕，代替 lv_scr_load / lv_scr_load_anim。
// LVGL 8.3.11 在上一次切换动画还没播完时再切换会访问空指针，板子直接重启
// （快速连点屏幕就会触发）。上游在 #5062 修好了，但 8.3 没有再发版，所以这里先把没播完的动画收尾再切换
// back 为 true 时新屏幕从左边滑进来（往回翻页）
void screen_load(lv_obj_t *scr, bool animate, bool back = false);
