#pragma once

#include "pomodoro.h"

// 局域网设置页面：城市、经纬度、亮度、重新配网
void web_config_begin();
void web_config_loop();

// 设置保存后由主循环处理（更新屏幕上的城市名和亮度）
bool web_config_take_changed();

// 网页上传固件时主循环会被阻塞，上传处理每收到一段数据就调用这个回调刷新屏幕
void web_config_set_progress_hook(void (*hook)());

// 番茄钟：设置页面显示状态，并通过 cmd 回调发出开始/暂停、跳过、重置
void web_config_set_pomodoro(const Pomodoro *p, void (*cmd)(PomoCmd));
