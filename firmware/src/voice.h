#pragma once

#include <stdint.h>

// AI 语音聊天：点一下或喊「喵喵同学」开始，之后像打电话一样连续对话，小橘用语音回答（阿里云百炼 Qwen-Omni-Realtime）。
// 服务器自己判断一句话说完了；小橘说话时点一下或直接开口就打断它；一阵子没人说话就自动挂断。
// 唤醒词和回声消除用乐鑫 esp-sr（只有 BOX-3 编进去，见 tools/esp_sr.py）。
// 连网、录音和播放都在单独的任务里，主循环只管点按和读状态

enum class VoiceState : uint8_t {
  Off,         // 没有麦克风或喇叭
  Idle,        // 没在聊天，点一下开始
  Connecting,  // 正在连百炼
  Listening,   // 在听，等主人说话
  Hearing,     // 主人正在说
  Thinking,    // 说完了，等回答
  Speaking,    // 正在播放回答
  Error,       // 出错了，error 里是原因；再点一下重新开始
};

struct VoiceStatus {
  VoiceState state;
  uint8_t level;        // 麦克风音量 0~100，用来画音量条
  uint32_t latency_ms;  // 上一句从说完到小橘出声用了多久，0 表示还没有
  uint32_t idle_left_ms;  // 再过多久没人说话就挂断（在听的时候才有）
  char heard[128];      // 主人最近说的话（服务器识别的）
  char reply[384];      // 小橘最近一次回答的文字
  char error[128];      // 出错原因；挂断后也用来说明为什么挂断
  bool wake_word;       // 唤醒词开着（没聊天时一直在等「喵喵同学」）
  bool full_duplex;     // 有回声消除：小橘说话时主人开口就能打断
};

static constexpr uint32_t kVoiceIdleHangupMs = 20000;  // 这么久没人说话就挂断

bool voice_available();
void voice_begin();

// 点一下聊天按钮：没在聊就开始；在听就挂断；小橘在想或在说就打断它，接着听
void voice_tap();
// 不聊了：挂断（离开聊天页时）
void voice_cancel();
// 设置：wake 打开唤醒词，barge 允许说话打断（都要 esp-sr 才有用）
void voice_set_options(bool wake, bool barge);
// 升级固件期间暂停：挂断正在进行的聊天，也不再听唤醒词。回声消除和唤醒词很吃 CPU，
// 写 Flash 时会把网页上传拖垮
void voice_pause(bool on);
// 刚被唤醒词叫醒时返回一次 true，主循环据此点亮屏幕、切到聊天页
bool voice_take_wake();

VoiceStatus voice_status();
