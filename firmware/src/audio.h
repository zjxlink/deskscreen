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

// 语音回答：边下载边播放。begin 和 end 之间提示音会排队等着；write 放进 32 秒的播放缓冲，一般不用等；
// 先攒够 300ms 再开始放，中途放空了就多攒一些（最多 1.5 秒）再接着放，网络时快时慢时声音不会一卡一卡的。
// end 等声音全部放完才返回。采样率 16kHz 单声道，音量按 audio_set_volume 调整
void audio_stream_begin();
void audio_stream_write(const int16_t *mono, size_t n);
// 这次回答的声音都收完了：缓冲里剩的不用再攒，直接放完
void audio_stream_done();
void audio_stream_end();
// 打断：缓冲里还没放的声音马上扔掉（DMA 里排着的 96ms 照常放完）
void audio_stream_flush();
// 缓冲里还有多少采样没放（不含 DMA 里的 96ms）
size_t audio_stream_pending();

// 麦克风（BOX-3 的 ES7210），采样率 16kHz。没有麦克风的板子上 available 返回 false
bool audio_mic_available();

// AI 聊天用：麦克风一直在读，每读到一段就调用回调。mic 是两个麦克风的平均，
// ref 是同一时刻喇叭放出的声音（已经对齐好，回声消除用的参考信号）。回调在采集任务里运行，要尽快返回；
// 传 nullptr 取消
using CaptureSink = void (*)(const int16_t *mic, const int16_t *ref, size_t n, void *ctx);
void audio_set_capture_sink(CaptureSink sink, void *ctx);

// 音乐律动用：读最多 max 个采样，最多等 100ms；喇叭在响时返回 0（免得听到自己的声音）
size_t audio_mic_listen(int16_t *mono, size_t max);
