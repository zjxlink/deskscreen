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

// 语音回答：边下载边播放。begin 和 end 之间提示音会排队等着；write 会阻塞到声音送进 I2S 为止。
// 采样率 16kHz 单声道，音量按 audio_set_volume 调整
void audio_stream_begin();
void audio_stream_write(const int16_t *mono, size_t n);
void audio_stream_end();

// 麦克风（BOX-3 的 ES7210），采样率 16kHz。没有麦克风的板子上 available 返回 false
bool audio_mic_available();
// 开始录音前调用，丢掉缓冲区里的旧数据
void audio_mic_begin();
// 读最多 max 个单声道采样（两个麦克风取平均），最多等 100ms，返回读到的个数
size_t audio_mic_read(int16_t *mono, size_t max);
