#include "sound_synth.h"

#include <math.h>

// E6、C6、G5 三个音，像门铃一样柔和
static const Note kFocusDone[] = {{1319, 180}, {1047, 180}, {784, 420}};
static const Note kBreakDone[] = {{784, 180}, {1047, 180}, {1319, 420}};
static const Note kMessage[] = {{1319, 220}, {1047, 480}};                   // E6、C6，门铃"叮咚"
static const Note kChime[] = {{1568, 160}, {1175, 520}};                    // G6、D6
static const Note kAlarm[] = {{1760, 140}, {0, 70}, {1760, 140}, {0, 70}, {1760, 140}};  // A6 x3
static const Note kReminder[] = {{1047, 120}, {0, 60}, {1047, 120}, {1568, 360}};  // C6、C6、G6
static const Note kChatOn[] = {{1175, 90}, {1568, 160}};                    // D6、G6，短促上扬
static const Note kChatOff[] = {{1568, 90}, {1175, 160}};                   // G6、D6，下行

static constexpr float kPi = 3.14159265f;
static constexpr float kAttackMs = 5.0f;
static constexpr float kReleaseMs = 8.0f;
static constexpr float kHarmonic = 0.3f;   // 加一点二次谐波，听起来更像铃声
static constexpr float kHeadroom = 0.75f;  // 基波 + 谐波最大 1.3 倍，乘上它不会削顶

const Note *sound_melody(SoundId id, int &count) {
  switch (id) {
    case SoundId::Message:
      count = sizeof(kMessage) / sizeof(kMessage[0]);
      return kMessage;
    case SoundId::Chime:
      count = sizeof(kChime) / sizeof(kChime[0]);
      return kChime;
    case SoundId::Alarm:
      count = sizeof(kAlarm) / sizeof(kAlarm[0]);
      return kAlarm;
    case SoundId::Reminder:
      count = sizeof(kReminder) / sizeof(kReminder[0]);
      return kReminder;
    case SoundId::ChatOn:
      count = sizeof(kChatOn) / sizeof(kChatOn[0]);
      return kChatOn;
    case SoundId::ChatOff:
      count = sizeof(kChatOff) / sizeof(kChatOff[0]);
      return kChatOff;
    case SoundId::BreakDone:
      count = sizeof(kBreakDone) / sizeof(kBreakDone[0]);
      return kBreakDone;
    case SoundId::FocusDone:
    default:
      count = sizeof(kFocusDone) / sizeof(kFocusDone[0]);
      return kFocusDone;
  }
}

size_t sound_length(const Note *notes, int count, int sample_rate) {
  size_t total = 0;
  for (int i = 0; i < count; i++) total += (size_t)notes[i].ms * sample_rate / 1000;
  return total;
}

size_t sound_render(const Note *notes, int count, int sample_rate, int volume, int16_t *out,
                    size_t max_samples) {
  if (volume < 0) volume = 0;
  if (volume > 100) volume = 100;
  float gain = (volume / 100.0f) * (volume / 100.0f) * kHeadroom / (1.0f + kHarmonic) * 32767.0f;

  size_t pos = 0;
  for (int i = 0; i < count && pos < max_samples; i++) {
    size_t n = (size_t)notes[i].ms * sample_rate / 1000;
    float attack = kAttackMs * sample_rate / 1000.0f;
    float release = kReleaseMs * sample_rate / 1000.0f;
    float decay = n / 3.0f;  // 指数衰减，到音符末尾大约剩 5%
    float w = 2.0f * kPi * notes[i].freq_hz / sample_rate;
    for (size_t k = 0; k < n && pos < max_samples; k++) {
      float env = 0.0f;
      if (notes[i].freq_hz > 0) {
        env = expf(-(float)k / decay);
        if (k < attack) env *= k / attack;
        if (k + release > n) env *= (n - k) / release;
      }
      float s = sinf(w * k) + kHarmonic * sinf(2.0f * w * k);
      out[pos++] = (int16_t)lroundf(s * env * gain);
    }
  }
  return pos;
}
