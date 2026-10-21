#pragma once

#include <stddef.h>

// 夜间模式的时间计算，不依赖硬件，可在电脑上测试。时间都用"当天第几分钟"表示（0~1439）

// now 是否落在 [start, end) 内，支持跨午夜（如 23:00~07:00）；start == end 视为不启用
bool night_is_active(int now, int start, int end);

// 解析 "HH:MM"，格式不对返回 -1
int night_parse_hhmm(const char *s);

// 格式化成 "HH:MM"
void night_format_hhmm(int minutes, char *buf, size_t len);

// 屏幕主题：深色、浅色、自动（白天浅色，日落后深色）
enum ScreenTheme { THEME_DARK = 0, THEME_LIGHT = 1, THEME_AUTO = 2 };

// 现在该不该用浅色。自动时日出到日落用浅色，还没拿到日出日落（-1）时按 7:00～19:00；
// now 为 -1（还没对时）时先用深色
bool theme_is_light(int mode, int now, int sunrise, int sunset);
