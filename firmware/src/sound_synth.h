#pragma once

#include <stddef.h>
#include <stdint.h>

// 提示音合成：几个音符组成的短旋律，渲染成 16 位单声道 PCM。不依赖硬件，可在电脑上测试

enum class SoundId : uint8_t {
  FocusDone,  // 专注结束，该休息了：下行三个音
  BreakDone,  // 休息结束，回来专注：上行三个音
  Message,    // 收到留言：叮咚两声
  Reminder,   // 久坐、喝水提醒：轻快的三连音
};

struct Note {
  uint16_t freq_hz;  // 0 表示休止
  uint16_t ms;
};

// 取某个提示音的旋律
const Note *sound_melody(SoundId id, int &count);

// 渲染整段旋律需要多少个采样
size_t sound_length(const Note *notes, int count, int sample_rate);

// 渲染成单声道 PCM。volume 为 0~100，按人耳感受做了平方曲线；
// 每个音符有很短的起音和收尾，避免"咔哒"声。返回写入的采样数（out 不够时截断）
size_t sound_render(const Note *notes, int count, int sample_rate, int volume, int16_t *out,
                    size_t max_samples);
