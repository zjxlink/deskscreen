#include "avplay.h"

#include <Arduino.h>
#include <esp_heap_caps.h>

#include "audio.h"

static constexpr int kQueue = 64;                // 先读好位置、等着显示的帧
static constexpr uint32_t kAudioAheadMs = 1000;  // 声音先放进缓冲这么多
static constexpr uint32_t kStallMs = 800;        // 帧都排满了声音还不走，就不等声音了
static constexpr int kMaxBadFrames = 10;         // 开头连着这么多帧解不出来就算放不了
static constexpr size_t kMaxFrameBytes = 512 * 1024;  // 一帧 JPG 再大就不是正常的视频了

namespace {

struct Pending {
  uint32_t off;
  uint32_t len;
  uint32_t frame;
};

// 按需要变大的 PSRAM 缓冲
struct Buf {
  uint8_t *p = nullptr;
  size_t size = 0;
  bool fit(size_t n) {
    if (n <= size) return true;
    free(p);
    size = n + 8 * 1024;
    p = (uint8_t *)heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!p) size = 0;
    return p != nullptr;
  }
  ~Buf() { free(p); }
};

}  // namespace

AvPlayResult avplay_run(AviReader &avi, const AvPlayHooks &h, bool sound) {
  AvPlayResult res = {avi.frame_pos(), 0, false};
  const uint32_t start_frame = avi.frame_pos();
  const uint32_t frame_us = avi.frame_us();
  const uint32_t start_ms = (uint32_t)((uint64_t)start_frame * frame_us / 1000);
  // 第 f 帧在视频里的时间（毫秒）
  auto frame_ms = [&](uint32_t f) { return (uint32_t)((uint64_t)f * frame_us / 1000); };

  bool audio = sound && avi.has_audio() && audio_stream_try_begin();
  PcmTo16k conv;
  conv.reset(avi.audio_rate(), avi.audio_channels());
  const uint32_t played0 = audio_stream_played();
  const uint32_t delay = audio_output_delay();
  const uint32_t t0 = millis();
  uint32_t last_clock = 0, clock_moved_at = t0;
  // 现在放到视频的哪里：有声音时看喇叭放了多少，没有时看过了多久
  auto media_now = [&]() -> uint32_t {
    if (!audio) return start_ms + (millis() - t0);
    uint32_t n = audio_stream_played() - played0;
    return start_ms + (n > delay ? (n - delay) / 16 : 0);
  };

  Pending q[kQueue];
  int head = 0, count = 0;
  bool eof = false;
  int bad = 0;
  Buf jpg, raw, pcm;

  while (!h.stop(h.ctx)) {
    // 先往前读：视频帧只记位置，声音直接放进喇叭的缓冲
    while (!eof && count < kQueue && (!audio || audio_stream_pending() < kAudioAheadMs * 16)) {
      uint32_t off, len;
      h.lock(h.ctx, true);
      AviChunk c = avi.next_chunk(off, len);
      bool ok = true;
      if (c == AviChunk::Audio) ok = raw.fit(len) && avi.read(off, raw.p, len);
      h.lock(h.ctx, false);
      if (c == AviChunk::End) {
        eof = true;
        if (audio) audio_stream_done();  // 剩下的声音不用再攒，直接放完
      } else if (c == AviChunk::Video) {
        q[(head + count) % kQueue] = {off, len, avi.frame_pos() - 1};
        count++;
      } else if (audio && ok) {
        size_t cap = conv.max_out(len);
        if (pcm.fit(cap * 2)) audio_stream_write((int16_t *)pcm.p, conv.convert(raw.p, len, (int16_t *)pcm.p, cap));
      }
    }
    if (count == 0) {
      if (eof) {
        res.ended = true;
        break;
      }
      vTaskDelay(pdMS_TO_TICKS(5));
      continue;
    }
    uint32_t now = media_now();
    if (now != last_clock) {
      last_clock = now;
      clock_moved_at = millis();
    }
    // 帧都排满了声音却不走（声音比画面少很多的文件）：不等了，免得卡住
    bool stalled = audio && count == kQueue && millis() - clock_moved_at > kStallMs;
    Pending &p = q[head];
    if (frame_ms(p.frame) > now && !stalled) {
      vTaskDelay(pdMS_TO_TICKS(5));
      continue;
    }
    // 慢了：后面一帧也到时间了，这一帧就不解了
    if (count > 1 && frame_ms(q[(head + 1) % kQueue].frame) <= now && res.shown > 0) {
      head = (head + 1) % kQueue;
      count--;
      continue;
    }
    bool ok = p.len <= kMaxFrameBytes && jpg.fit(p.len);
    if (ok) {
      h.lock(h.ctx, true);
      ok = avi.read(p.off, jpg.p, p.len);
      h.lock(h.ctx, false);
    }
    if (ok && h.show(h.ctx, jpg.p, p.len, p.frame)) {
      res.frame = p.frame;
      res.shown++;
    } else if (res.shown == 0 && ++bad >= kMaxBadFrames) {
      break;
    }
    head = (head + 1) % kQueue;
    count--;
  }
  if (audio) {
    if (res.ended) {
      audio_stream_end();  // 等声音放完
    } else {
      audio_stream_flush();
      audio_stream_end();
    }
  }
  // 没放到结尾：下次从显示的下一帧接着放
  if (!res.ended && res.shown == 0) res.frame = start_frame;
  return res;
}
