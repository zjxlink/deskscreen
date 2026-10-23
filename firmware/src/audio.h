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
// 放视频里的声音（从卡上读，不是边下边放）：和 audio_stream_begin 一样，但喇叭正被别人用着（AI 聊天在说话）
// 时不等，返回 false。只攒 300ms 就放，也不跟着语音回答学「网络慢要多攒」：那个值最多 1.5 秒，
// 比视频预读的声音还多时会一直攒不够，画面就卡住不动了
bool audio_stream_try_begin();
void audio_stream_write(const int16_t *mono, size_t n);
// 这次回答的声音都收完了：缓冲里剩的不用再攒，直接放完
void audio_stream_done();
void audio_stream_end();
// 打断：缓冲里还没放的声音马上扔掉（DMA 里排着的 96ms 照常放完）
void audio_stream_flush();
// 缓冲里还有多少采样没放（不含 DMA 里的 96ms）
size_t audio_stream_pending();
// 开机以来喇叭从缓冲里取走去放的采样数（一直往上加），视频按它对时间，画面和声音对得上
uint32_t audio_stream_played();
// 交给 I2S 到真正放出来还要多少个采样
uint32_t audio_output_delay();

// 麦克风（BOX-3 的 ES7210），采样率 16kHz。没有麦克风的板子上 available 返回 false
bool audio_mic_available();

// AI 聊天用：麦克风一直在读，每读到一段就调用回调。mic 是两个麦克风的平均，
// ref 是同一时刻喇叭放出的声音（已经对齐好，回声消除用的参考信号）。回调在采集任务里运行，要尽快返回；
// 传 nullptr 取消
using CaptureSink = void (*)(const int16_t *mic, const int16_t *ref, size_t n, void *ctx);
void audio_set_capture_sink(CaptureSink sink, void *ctx);

// 音乐律动用：读最多 max 个采样，最多等 100ms；喇叭在响时返回 0（免得听到自己的声音）。
// 音乐播放器在放歌时给的是放出去的音乐（16kHz）
size_t audio_mic_listen(int16_t *mono, size_t max);

// 音乐播放器用：拿到喇叭，把喇叭和麦克风切到歌的采样率 rate（8000～48000，单声道）。
// 喇叭正被语音回答占着时等最多 1 秒，拿不到返回 false。拿着的时候：
// - 提示音（整点报时、番茄钟……）叠在音乐上放，音乐压低；
// - 唤醒词、回声消除照常工作（麦克风的声音转回 16kHz 给它们），音乐律动直接分析放出去的音乐；
// - 别人要用喇叭（语音回答、影片声音）时 audio_music_wanted() 变成 true，播放器要尽快 audio_music_end()
bool audio_music_begin(uint32_t rate);
// 放进喇叭的缓冲（按音量调小），满了就等
void audio_music_write(const int16_t *mono, size_t n);
// 缓冲里还有多少采样没放
size_t audio_music_pending();
// 暂停、跳转时扔掉缓冲里还没放的
void audio_music_flush();
// 让出喇叭（没放完的扔掉），静下来以后切回 16kHz
void audio_music_end();
bool audio_music_wanted();
