#pragma once

#include <stddef.h>

// 夜间模式的时间计算，不依赖硬件，可在电脑上测试。时间都用"当天第几分钟"表示（0~1439）

// now 是否落在 [start, end) 内，支持跨午夜（如 23:00~07:00）；start == end 视为不启用
bool night_is_active(int now, int start, int end);

// 解析 "HH:MM"，格式不对返回 -1
int night_parse_hhmm(const char *s);

// 格式化成 "HH:MM"
void night_format_hhmm(int minutes, char *buf, size_t len);
