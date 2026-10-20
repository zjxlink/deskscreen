#pragma once

#include <stdint.h>

// AI 语音聊天：按住说话、松手发送，小橘用语音回答（阿里云百炼 Qwen-Omni）。
// 录音、联网和播放都在单独的任务里，主循环只管按下、松开和读状态

enum class VoiceState : uint8_t {
  Off,        // 没有麦克风或喇叭
  Idle,       // 等着按住说话
  Recording,  // 正在录音
  Thinking,   // 录音已发出，等回答
  Speaking,   // 正在播放回答
  Error,      // 出错了，error 里是原因；再按一次重新开始
};

struct VoiceStatus {
  VoiceState state;
  uint8_t level;      // 录音时的音量 0~100，用来画音量条
  uint32_t rec_ms;    // 已经录了多久
  char reply[384];    // 最近一次回答的文字
  char error[128];
};

static constexpr uint32_t kVoiceMaxRecordMs = 12000;  // 一次最多录 12 秒
static constexpr uint32_t kVoiceMinRecordMs = 500;    // 太短当成误触

bool voice_available();
void voice_begin();

// 按下开始录音；正在等回答或播放回答时按下就停下来
void voice_press();
// 松手：录音结束并发送
void voice_release();
// 不说了：停止录音或回答
void voice_cancel();

VoiceStatus voice_status();
