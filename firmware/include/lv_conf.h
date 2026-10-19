// LVGL 8.3 配置，只列出与默认值不同的项，其余见 lvgl/src/lv_conf_internal.h
#if 1
#ifndef LV_CONF_H
#define LV_CONF_H

#include <stdint.h>

#define LV_COLOR_DEPTH 16
#define LV_COLOR_16_SWAP 0

// 用系统 malloc，大块内存会自动放到 PSRAM
#define LV_MEM_CUSTOM 1

#define LV_TICK_CUSTOM 1
#define LV_TICK_CUSTOM_INCLUDE "Arduino.h"
#define LV_TICK_CUSTOM_SYS_TIME_EXPR (millis())

#define LV_USE_LOG 0
#define LV_USE_PERF_MONITOR 0

// Montserrat 用于数字；中文字体在 src/fonts，由 tools/gen_fonts.py 生成
#define LV_FONT_MONTSERRAT_14 1
#define LV_FONT_MONTSERRAT_20 1
#define LV_FONT_MONTSERRAT_48 1

#endif
#endif
