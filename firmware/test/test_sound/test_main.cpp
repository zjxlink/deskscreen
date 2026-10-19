// 提示音合成的单元测试，在电脑上运行：pio test -e native
#include <stdlib.h>
#include <unity.h>

#include <vector>

#include "sound_synth.h"

void setUp() {}
void tearDown() {}

static const int kRate = 16000;

static std::vector<int16_t> render(SoundId id, int volume) {
  int n;
  const Note *notes = sound_melody(id, n);
  std::vector<int16_t> buf(sound_length(notes, n, kRate));
  size_t got = sound_render(notes, n, kRate, volume, buf.data(), buf.size());
  TEST_ASSERT_EQUAL(buf.size(), got);
  return buf;
}

static int peak(const std::vector<int16_t> &v, size_t from, size_t to) {
  int p = 0;
  for (size_t i = from; i < to && i < v.size(); i++) p = abs(v[i]) > p ? abs(v[i]) : p;
  return p;
}

static void test_length() {
  int n;
  const Note *notes = sound_melody(SoundId::FocusDone, n);
  TEST_ASSERT_EQUAL(3, n);
  // 180 + 180 + 420 毫秒
  TEST_ASSERT_EQUAL(780 * kRate / 1000, sound_length(notes, n, kRate));
}

static void test_no_clipping_and_volume_scales() {
  auto loud = render(SoundId::FocusDone, 100);
  auto soft = render(SoundId::FocusDone, 50);
  int p_loud = peak(loud, 0, loud.size());
  int p_soft = peak(soft, 0, soft.size());
  TEST_ASSERT_TRUE(p_loud < 32767);          // 不削顶
  TEST_ASSERT_TRUE(p_loud > 15000);          // 最大音量足够响
  // 平方曲线：音量 50% 时幅度约为 25%
  TEST_ASSERT_INT_WITHIN(p_loud / 20, p_loud / 4, p_soft);

  auto silent = render(SoundId::FocusDone, 0);
  TEST_ASSERT_EQUAL(0, peak(silent, 0, silent.size()));
}

static void test_no_clicks_at_note_edges() {
  auto v = render(SoundId::BreakDone, 100);
  size_t edges[] = {0, 180 * kRate / 1000, 360 * kRate / 1000, v.size()};
  int p = peak(v, 0, v.size());
  // 每个音符的开头和结尾都接近 0，相邻采样跳变不大
  for (size_t e : edges) {
    if (e < v.size()) TEST_ASSERT_TRUE(abs(v[e]) < p / 50);
    if (e > 0) TEST_ASSERT_TRUE(abs(v[e - 1]) < p / 20);
  }
}

// 用过零次数估算频率，确认音高对
static void test_pitch() {
  auto v = render(SoundId::FocusDone, 100);
  size_t start = 20 * kRate / 1000, end = 160 * kRate / 1000;  // 第一个音 E6，避开起止
  int crossings = 0;
  for (size_t i = start + 1; i < end; i++) {
    if ((v[i - 1] < 0) != (v[i] < 0)) crossings++;
  }
  // 基波加二次谐波后，每个周期仍然是 2 次过零
  float freq = crossings / 2.0f / ((end - start) / (float)kRate);
  TEST_ASSERT_FLOAT_WITHIN(30.0f, 1319.0f, freq);
}

static void test_truncates_to_buffer() {
  int n;
  const Note *notes = sound_melody(SoundId::FocusDone, n);
  int16_t small[100];
  TEST_ASSERT_EQUAL(100, sound_render(notes, n, kRate, 80, small, 100));
}

static void test_descending_vs_ascending() {
  int n;
  const Note *down = sound_melody(SoundId::FocusDone, n);
  TEST_ASSERT_TRUE(down[0].freq_hz > down[n - 1].freq_hz);
  const Note *up = sound_melody(SoundId::BreakDone, n);
  TEST_ASSERT_TRUE(up[0].freq_hz < up[n - 1].freq_hz);
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_length);
  RUN_TEST(test_no_clipping_and_volume_scales);
  RUN_TEST(test_no_clicks_at_note_edges);
  RUN_TEST(test_pitch);
  RUN_TEST(test_truncates_to_buffer);
  RUN_TEST(test_descending_vs_ascending);
  return UNITY_END();
}
