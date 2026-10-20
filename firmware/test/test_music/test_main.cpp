// 音乐律动的单元测试：用合成的声音试试能不能分清音乐、说话、风扇声和安静。在电脑上运行：pio test -e native
#include <math.h>
#include <stdlib.h>
#include <unity.h>

#include "music.h"

void setUp() {}
void tearDown() {}

static MusicMeter *meter;
static uint32_t now_ms;
static uint32_t seed = 1;

static float noise() {  // -1～1 的白噪声
  seed = seed * 1664525 + 1013904223;
  return (int32_t)seed / 2147483648.0f;
}

// 生成 ms 毫秒的声音喂进去，sample(t) 返回 -1～1；返回这段时间里 playing() 为 true 的帧数和节拍数
struct Result {
  int playing_frames = 0;
  int frames = 0;
  int beats = 0;
  bool playing_at_end = false;
};

template <typename F>
static Result feed(int ms, F sample) {
  Result r;
  int16_t buf[kMusicFrame];
  int frames = ms * kMusicRate / 1000 / kMusicFrame;
  static uint64_t n = 0;
  for (int f = 0; f < frames; f++) {
    for (int i = 0; i < kMusicFrame; i++, n++) {
      float v = sample((double)n / kMusicRate);
      v = v > 1 ? 1 : v < -1 ? -1 : v;
      buf[i] = (int16_t)(v * 32000);
    }
    now_ms += kMusicFrame * 1000 / kMusicRate;
    meter->feed(buf, now_ms);
    r.frames++;
    r.playing_frames += meter->playing();
    r.beats += meter->take_beat();
  }
  r.playing_at_end = meter->playing();
  return r;
}

static float room(double) { return 0.001f * noise(); }  // 安静的房间，约 -60dBFS

// 120 拍的流行音乐：每拍一下底鼓，加上一直响的和弦和反拍的镲
static float music(double t) {
  double beat = fmod(t, 0.5);
  float kick = beat < 0.15 ? 0.5f * expf(-beat * 25) * sinf(2 * M_PI * 55 * beat) : 0;
  float chord = 0.04f * (sinf(2 * M_PI * 262 * t) + sinf(2 * M_PI * 330 * t) + sinf(2 * M_PI * 392 * t));
  double off = fmod(t + 0.25, 0.5);
  float hat = off < 0.03 ? 0.05f * noise() : 0;
  return kick + chord + hat + room(t);
}

// 说话：200～300ms 的音节，中间停 100～500ms，偶尔停顿久一点
static double syl_end = 0, syl_next = 0;
static float speech(double t) {
  if (t >= syl_next) {
    syl_end = t + 0.2 + 0.1 * (noise() + 1) / 2;
    syl_next = syl_end + 0.1 + 0.4 * (noise() + 1) / 2 + ((noise() > 0.7f) ? 0.6 : 0);
  }
  if (t >= syl_end) return room(t);
  double x = (t - (syl_end - 0.25));
  float env = (float)(x < 0.03 ? x / 0.03 : 1);
  return env * (0.15f * sinf(2 * M_PI * 140 * t) + 0.08f * sinf(2 * M_PI * 420 * t) + 0.03f * noise()) + room(t);
}

static float fan(double t) { return 0.05f * noise() + 0.02f * sinf(2 * M_PI * 100 * t); }  // 一直响的风扇

static void reset() {
  delete meter;
  meter = new MusicMeter();
  now_ms = 1000;
  feed(2000, room);
}

static void test_silence() {
  reset();
  Result r = feed(15000, room);
  TEST_ASSERT_EQUAL(0, r.playing_frames);
  TEST_ASSERT_EQUAL(0, r.beats);
  for (int b = 0; b < kMusicBands; b++) TEST_ASSERT_TRUE(meter->bands()[b] < 0.3f);
}

static void test_music_detected_and_stops() {
  reset();
  Result r = feed(10000, music);
  TEST_ASSERT_TRUE(r.playing_at_end);
  // 2.5 秒确认 + 前面攒够 4 秒的样子，7 秒内一定开始
  TEST_ASSERT_TRUE(r.playing_frames > (10000 - 7000) * kMusicRate / 1000 / kMusicFrame);
  // 10 秒 120 拍，大约 20 个节拍
  TEST_ASSERT_INT_WITHIN(3, 20, r.beats);
  // 有和弦的那几条（262～392Hz）一直立着，最高的几条（5kHz 以上）只有镲，矮得多
  float mid = 0, high = 0;
  for (int b = 4; b <= 6; b++) mid += meter->bands()[b] / 3;
  for (int b = 14; b < kMusicBands; b++) high += meter->bands()[b] / 2;
  TEST_ASSERT_TRUE(mid > 0.35f);
  TEST_ASSERT_TRUE(mid > high + 0.2f);

  // 一直放 60 秒也不会被当成底噪
  r = feed(60000, music);
  TEST_ASSERT_TRUE(r.playing_at_end);
  TEST_ASSERT_EQUAL(r.frames, r.playing_frames);

  // 音乐停了，几秒后不算在放了
  r = feed(8000, room);
  TEST_ASSERT_FALSE(r.playing_at_end);
}

static void test_speech_is_not_music() {
  reset();
  Result r = feed(30000, speech);
  TEST_ASSERT_EQUAL(0, r.playing_frames);
}

static void test_fan_is_not_music() {
  reset();
  Result r = feed(30000, fan);
  TEST_ASSERT_EQUAL(0, r.playing_frames);
  TEST_ASSERT_TRUE(r.beats <= 3);
}

static void test_music_over_fan() {
  reset();
  feed(10000, fan);
  Result r = feed(12000, [](double t) { return music(t) + fan(t); });
  TEST_ASSERT_TRUE(r.playing_at_end);
}

// 开机时音乐已经在放，也能认出来
static void test_music_from_start() {
  delete meter;
  meter = new MusicMeter();
  now_ms = 1000;
  Result r = feed(10000, music);
  TEST_ASSERT_TRUE(r.playing_at_end);
}

// 小声的音乐（手机外放、离得远）也行
static void test_quiet_music() {
  reset();
  Result r = feed(10000, [](double t) { return music(t) * 0.08f + room(t); });
  TEST_ASSERT_TRUE(r.playing_at_end);
}

static void test_idle_clears() {
  reset();
  feed(10000, music);
  TEST_ASSERT_TRUE(meter->playing());
  meter->idle(now_ms);
  TEST_ASSERT_FALSE(meter->playing());
  TEST_ASSERT_FALSE(meter->take_beat());
  for (int i = 0; i < 30; i++) meter->idle(now_ms);
  for (int b = 0; b < kMusicBands; b++) TEST_ASSERT_TRUE(meter->bands()[b] < 0.05f);
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_silence);
  RUN_TEST(test_music_detected_and_stops);
  RUN_TEST(test_speech_is_not_music);
  RUN_TEST(test_fan_is_not_music);
  RUN_TEST(test_music_over_fan);
  RUN_TEST(test_music_from_start);
  RUN_TEST(test_quiet_music);
  RUN_TEST(test_idle_clears);
  return UNITY_END();
}
