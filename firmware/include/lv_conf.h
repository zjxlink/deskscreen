// LVGL 8.3 配置，只列出与默认值不同的项，其余见 lvgl/src/lv_conf_internal.h
#if 1
#ifndef LV_CONF_H
#define LV_CONF_H

#include <stdint.h>

#define LV_COLOR_DEPTH 16
#define LV_COLOR_16_SWAP 0

// 界面对象优先放到 PSRAM。几百个小对象如果用系统 malloc 会全部落在内部 RAM，占掉几十 KB，
// 而 HTTPS（mbedTLS）只能用内部 RAM，每个连接要一块 16KB 的连续内存。没有 PSRAM 的板子自动退回内部 RAM
#define LV_MEM_CUSTOM 1
#define LV_MEM_CUSTOM_INCLUDE <esp_heap_caps.h>
#define LV_MEM_CUSTOM_ALLOC(size) \
  heap_caps_malloc_prefer(size, 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT, MALLOC_CAP_DEFAULT)
#define LV_MEM_CUSTOM_FREE heap_caps_free
#define LV_MEM_CUSTOM_REALLOC(ptr, size) \
  heap_caps_realloc_prefer(ptr, size, 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT, MALLOC_CAP_DEFAULT)

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
