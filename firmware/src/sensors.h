#pragma once

// SENSOR 底座上的温湿度传感器和人体感应雷达。没有底座的板子上这些函数什么也不做

// 开机时探测底座并初始化雷达，要在 audio_begin() 之后调用
void sensors_begin();

// 主循环调用：每隔几秒读一次温湿度（不阻塞）
void sensors_loop();

// 插了底座
bool sensors_dock();

// 雷达这一刻是否检测到有人（动作）
bool sensors_presence();

// 有新的温湿度读数时返回 true
bool sensors_take_climate(float &temp, float &humidity);
