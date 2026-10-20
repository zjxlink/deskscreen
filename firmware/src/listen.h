#pragma once

#include "music.h"

// 音乐律动的后台任务：一直听麦克风，交给 MusicMeter 分析。
// 只在板子上分析，不录下来也不上传；喇叭响的时候自动停下

struct MusicView {
  bool playing;               // 正在放音乐
  bool beat;                  // 上次取之后有新的节拍
  float bands[kMusicBands];   // 频谱条高度 0～1
};

// 有麦克风时启动后台任务（在 audio_begin 之后调用）
void listen_begin();
bool listen_available();

// 设置里关掉时不听麦克风
void listen_set_enabled(bool on);

// 取最新的分析结果，只在主循环里调用
void listen_get(MusicView &out);
