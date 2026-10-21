#pragma once

#include <stddef.h>
#include <stdint.h>

#include "avi.h"

// 放 MJPEG 视频（带声音时一起放），相册里的视频和影片页都用它。
// 有声音时按喇叭放到哪里来对时间，画面跟着声音走；解码跟不上时跳帧，声音不断。
// 在后台任务里调用，一直放到结尾或者 stop 返回 true

struct AvPlayHooks {
  // 解码第 frame 帧的 JPG 并显示出来（可以等屏幕取走上一帧），解不出来时返回 false
  bool (*show)(void *ctx, const uint8_t *jpg, size_t len, uint32_t frame);
  // 要停下来时返回 true（离开页面、暂停、快进快退）
  bool (*stop)(void *ctx);
  // 读卡前后拿、放 TF 卡的锁（AviReader 的读回调里不拿锁）
  void (*lock)(void *ctx, bool take);
  void *ctx;
};

struct AvPlayResult {
  uint32_t frame;   // 最后显示的是第几帧（一帧都没显示时是开始的那一帧）
  int shown;        // 显示了几帧
  bool ended;       // 放到了结尾
};

// 从 avi 现在的位置开始放。sound 为 false 或者喇叭正被 AI 聊天占着时不放声音
AvPlayResult avplay_run(AviReader &avi, const AvPlayHooks &hooks, bool sound);
