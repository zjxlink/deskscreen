#pragma once

// 日出日落和月相，不依赖硬件，可在电脑上测试。
// 日出日落时间来自天气预报（Open-Meteo 按当地时区给出），月相按日期算

// 月相：0 是新月，0.25 上弦，0.5 满月，0.75 下弦，接近 1 又回到新月。按当天中午算，误差在半天以内
float moon_phase(int year, int month, int day);

// 月相的名字：新月、蛾眉月、上弦月、盈凸月、满月、亏凸月、下弦月、残月
const char *moon_phase_name(float phase);

// 画月亮用：圆心在 (0,0)、半径 r 的月亮上，(x, y) 这一点是不是亮的（北半球看到的样子，盈月右边亮）
bool moon_lit(float phase, float x, float y, float r);

// 解析 "2026-10-20T06:12" 里的时间，返回当天第几分钟，格式不对返回 -1
int astro_parse_time(const char *iso);
