#include "listen.h"

#include <Arduino.h>
#include <esp_heap_caps.h>

#include <new>

#include "audio.h"

static TaskHandle_t task;
static SemaphoreHandle_t lock;
static volatile bool enabled = true;
static MusicView latest;

static void publish(const MusicMeter &m, bool beat) {
  xSemaphoreTake(lock, portMAX_DELAY);
  latest.playing = m.playing();
  latest.beat = latest.beat || beat;
  memcpy(latest.bands, m.bands(), sizeof(latest.bands));
  xSemaphoreGive(lock);
}

static void listen_task(void *mem) {
  MusicMeter &meter = *new (mem) MusicMeter();
  static int16_t frame[kMusicFrame];
  size_t n = 0;
  for (;;) {
    if (!enabled) {
      meter.idle(millis());
      publish(meter, false);
      n = 0;
      vTaskDelay(pdMS_TO_TICKS(500));
      continue;
    }
    size_t got = audio_mic_listen(frame + n, kMusicFrame - n);
    if (got == 0) {  // 喇叭在响，或者读失败了
      meter.idle(millis());
      publish(meter, false);
      n = 0;
      vTaskDelay(pdMS_TO_TICKS(50));
      continue;
    }
    n += got;
    if (n < kMusicFrame) continue;
    n = 0;
    meter.feed(frame, millis());
    publish(meter, meter.take_beat());
  }
}

void listen_begin() {
  if (!audio_mic_available()) return;
  // FFT 的缓冲有好几 KB，放在 PSRAM
  void *mem = heap_caps_malloc(sizeof(MusicMeter), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!mem) mem = malloc(sizeof(MusicMeter));
  if (!mem) return;
  lock = xSemaphoreCreateMutex();
  // 和照片解码、联网一样放在 0 号核，低优先级，不影响界面
  xTaskCreatePinnedToCore(listen_task, "listen", 4096, mem, 1, &task, 0);
}

bool listen_available() { return task != nullptr; }

void listen_set_enabled(bool on) { enabled = on; }

void listen_get(MusicView &out) {
  if (!task) {
    out = {};
    return;
  }
  xSemaphoreTake(lock, portMAX_DELAY);
  out = latest;
  latest.beat = false;
  xSemaphoreGive(lock);
}
