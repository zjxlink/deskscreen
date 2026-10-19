#pragma once

// 局域网设置页面：城市、经纬度、亮度、重新配网
void web_config_begin();
void web_config_loop();

// 设置保存后由主循环处理（更新屏幕上的城市名和亮度）
bool web_config_take_changed();
