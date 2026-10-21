#pragma once

#include <Arduino.h>

#include "pomo_stats.h"
#include "pomodoro.h"

// 局域网设置网页（web/index.html）和它用到的 JSON 接口
void web_config_begin();
void web_config_loop();

// 设置保存后由主循环处理（更新屏幕上的城市名和亮度）
bool web_config_take_changed();

// 网页上传固件时主循环会被阻塞，上传处理每收到一段数据就调用这个回调刷新屏幕
void web_config_set_progress_hook(void (*hook)());

// 网页上传影片时每收一段调一次：done / total 是收到和一共的字节数，total 为 0 表示收完了（成功或失败）
void web_config_set_movie_hook(void (*hook)(uint64_t done, uint64_t total));
// 网页上传歌曲时的进度（和影片一样），total 为 0 表示收完了或者出错了
void web_config_set_music_hook(void (*hook)(uint64_t done, uint64_t total));

// 番茄钟：设置页面显示状态，并通过 cmd 回调发出开始/暂停、跳过、重置
void web_config_set_pomodoro(const Pomodoro *p, void (*cmd)(PomoCmd));

// 番茄钟统计：设置页面显示今日完成、近 7 天和连续达标天数。day 返回今天是第几天，未对时返回 -1
void web_config_set_pomo_stats(const PomoStats *s, int (*day)());

// 留言板：设置页面提交留言后调用，sound 表示要不要响一声
void web_config_set_message_hook(void (*hook)(const String &text, bool sound));

// 设置页上显示的室内温湿度和人体感应状态
struct RoomInfo {
  bool dock;      // 插了 SENSOR 底座
  bool valid;     // 温湿度读数有效
  float temp_c;
  int humidity;
  bool presence;  // 雷达感应到动作
  bool away;      // 没人已熄屏
};
void web_config_set_room_info(void (*hook)(RoomInfo &info));
