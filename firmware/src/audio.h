#pragma once

#include "sound_synth.h"

// 喇叭播放提示音。没有喇叭的板子（board.h 里没定义 HAS_AUDIO）上这些函数什么都不做

bool audio_available();

// 初始化解码芯片和 I2S，启动播放任务；要在 display_init() 之后调用（I2C 和触摸共用）
void audio_begin();

// 音量 0~100
void audio_set_volume(int volume);

// 排队播放，立即返回
void audio_play(SoundId id);
