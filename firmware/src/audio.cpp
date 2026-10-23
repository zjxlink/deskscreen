#include "audio.h"

#include <Arduino.h>

#include "board.h"

#ifndef HAS_AUDIO

bool audio_available() { return false; }
void audio_begin() {}
void audio_set_volume(int) {}
void audio_play(SoundId) {}
void audio_stream_begin() {}
bool audio_stream_try_begin() { return false; }
void audio_stream_write(const int16_t *, size_t) {}
void audio_stream_done() {}
void audio_stream_end() {}
void audio_stream_flush() {}
size_t audio_stream_pending() { return 0; }
uint32_t audio_stream_played() { return 0; }
uint32_t audio_output_delay() { return 0; }
bool audio_mic_available() { return false; }
void audio_set_capture_sink(CaptureSink, void *) {}
size_t audio_mic_listen(int16_t *, size_t) { return 0; }
bool audio_music_begin(uint32_t) { return false; }
void audio_music_write(const int16_t *, size_t) {}
size_t audio_music_pending() { return 0; }
void audio_music_flush() {}
void audio_music_end() {}
bool audio_music_wanted() { return false; }

#else

#include <LovyanGFX.hpp>
#include <driver/i2c.h>
#include <driver/i2s.h>
#include <esp_heap_caps.h>

#include "audio_sync.h"
#include "resample.h"

static constexpr int kRate = 16000;            // 提示音用 16kHz 足够
static constexpr int kMclk = kRate * 256;      // 4.096MHz，ES8311 从 MCLK 引脚取时钟
static constexpr i2s_port_t kI2s = I2S_NUM_0;
static constexpr int kI2cPort = I2C_NUM_0;     // 和触摸芯片共用，走 LovyanGFX 的 I2C 实现，避免两套驱动冲突

static constexpr int kDmaBufs = 6;
static constexpr int kDmaLen = 256;                       // 每个 DMA 缓冲的采样数（16ms）
static constexpr uint32_t kTxQueue = kDmaBufs * kDmaLen;  // 喇叭写进去到放出来隔着这么多采样
static constexpr uint32_t kOutRing = 1 << 19;             // 待播放的声音，32 秒（1MB PSRAM），回答收得比放得快，整段先存着
static constexpr uint32_t kRefRing = 16384;               // 放过的声音（回声消除的参考信号），1 秒
static constexpr uint32_t kListenRing = 4096;             // 给音乐律动的麦克风声音
static constexpr int32_t kRefLead = 320;  // 参考信号比回声早 20ms 给，量偏移的误差不会让回声跑到参考信号前面
static constexpr uint32_t kPrebufferMin = kRate * 3 / 10;  // 语音回答先攒 300ms 再放
static constexpr uint32_t kPrebufferMax = kRate * 3 / 2;   // 网络再慢也最多攒 1.5 秒

static QueueHandle_t queue;
static SemaphoreHandle_t out_lock;  // 提示音和语音回答轮流用喇叭
static volatile int volume = 60;
static bool ready = false;
static bool mic_ready = false;
static volatile bool speaker_on = false;  // 功放开着（在出声），音乐律动这时不听，免得听到自己的声音

// 放音乐时喇叭和麦克风切到歌的采样率（共用一组时钟），平时是 16kHz。只在功放关着（没声音）时切，免得啪一声
static volatile uint32_t cur_rate = kRate;
static volatile uint32_t want_rate = kRate;
static volatile uint32_t rate_changes = 0;  // 切过几次，采集任务据此重新对齐
static volatile bool music_active = false;  // 音乐播放器拿着喇叭
static volatile bool music_yield = false;   // 别人（语音回答、影片声音）要用喇叭，播放器该让出来了
// 放音乐时来了提示音（整点报时、番茄钟……）：按音乐的采样率合成好，喇叭任务把它叠在压低的音乐上
static int16_t *volatile overlay = nullptr;
static volatile uint32_t overlay_len = 0, overlay_pos = 0;

// 喇叭：一个任务一直往 I2S 写（没声音时写静音），这样放出的每个采样都有编号，能和麦克风对齐。
// 待播放的声音放在 out_ring，放过的存进 ref_ring 当回声消除的参考信号
static int16_t *out_ring;
static volatile uint32_t out_w = 0, out_r = 0;
static volatile bool out_flush = false;
static volatile bool out_streaming = false;  // 语音回答还在下载
static volatile bool out_hold = false;       // 在攒声音，先不放
static volatile uint32_t out_target = kPrebufferMin;  // 要攒够多少采样才放：放空一次就多攒一点，一直顺就慢慢减回来
static volatile uint32_t out_underruns = 0;           // 这次回答中途放空了几次
static volatile bool out_local = false;  // 在放视频的声音：只攒最少的，放空了也不多攒
static int16_t *ref_ring;
static volatile uint32_t tx_count = 0;  // 已经交给 I2S 的采样数
static volatile uint32_t out_played = 0;  // 从 out_ring 取走去放的采样数（扔掉的不算）

// 麦克风：一个任务一直读，转给 AI 聊天（回调）和音乐律动（listen_ring）
static int16_t *listen_ring;
static volatile uint32_t listen_w = 0;
static uint32_t listen_r = 0;
static volatile CaptureSink capture_sink = nullptr;
static void *volatile capture_ctx = nullptr;

static bool codec_write(uint8_t reg, uint8_t val) {
  return lgfx::i2c::writeRegister8(kI2cPort, ES8311_ADDR, reg, val).has_value();
}

static bool codec_update(uint8_t reg, uint8_t keep_mask, uint8_t set_bits) {
  auto r = lgfx::i2c::readRegister8(kI2cPort, ES8311_ADDR, reg);
  if (!r.has_value()) return false;
  return codec_write(reg, (r.value() & keep_mask) | set_bits);
}

// ES8311 初始化：从模式、标准 I2S、16 位、MCLK = 256 x 采样率、只用 DAC。
// 寄存器顺序和取值移植自乐鑫 esp-bsp 的 components/es8311/es8311.c（Apache-2.0），
// 时钟分频取自其中 {4096000, 16000} 这一行系数
static bool codec_init() {
  bool ok = codec_write(0x00, 0x1F);  // 复位
  delay(20);
  ok = ok && codec_write(0x00, 0x00) && codec_write(0x00, 0x80);  // 上电

  ok = ok && codec_write(0x01, 0x3F);             // 打开全部时钟，MCLK 来自 MCLK 引脚，不反相
  ok = ok && codec_update(0x06, ~0x20, 0x00);     // SCLK 不反相
  ok = ok && codec_update(0x02, 0x07, 0x00);      // pre_div = 1，pre_multi = 1x
  ok = ok && codec_write(0x03, 0x10);             // 单速模式，ADC OSR
  ok = ok && codec_write(0x04, 0x10);             // DAC OSR
  ok = ok && codec_write(0x05, 0x00);             // ADC、DAC 分频都是 1
  ok = ok && codec_update(0x06, 0xE0, 4 - 1);     // BCLK 分频 4
  ok = ok && codec_update(0x07, 0xC0, 0x00);      // LRCK 分频高位
  ok = ok && codec_write(0x08, 0xFF);             // LRCK 分频低位：256

  ok = ok && codec_update(0x00, 0xBF, 0x00);      // 从模式
  ok = ok && codec_write(0x09, 0x0C);             // 输入 16 位标准 I2S
  ok = ok && codec_write(0x0A, 0x0C);             // 输出 16 位标准 I2S

  ok = ok && codec_write(0x0D, 0x01);             // 模拟电路上电
  ok = ok && codec_write(0x0E, 0x02);
  ok = ok && codec_write(0x12, 0x00);             // DAC 上电
  ok = ok && codec_write(0x13, 0x10);             // 输出驱动
  ok = ok && codec_write(0x1C, 0x6A);
  ok = ok && codec_write(0x37, 0x08);             // 绕过 DAC 均衡器

  ok = ok && codec_write(0x32, 0xBF);             // DAC 音量 0dB，实际音量在合成时控制
  ok = ok && codec_update(0x31, ~0x60, 0x00);     // 取消静音
  return ok;
}

static bool i2s_init() {
  i2s_config_t cfg = {};
#ifdef HAS_MIC
  cfg.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX | I2S_MODE_RX);  // 喇叭和麦克风共用一组时钟
#else
  cfg.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX);
#endif
  cfg.sample_rate = kRate;
  cfg.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
  cfg.channel_format = I2S_CHANNEL_FMT_RIGHT_LEFT;
  cfg.communication_format = I2S_COMM_FORMAT_STAND_I2S;
  cfg.dma_buf_count = kDmaBufs;
  cfg.dma_buf_len = kDmaLen;
  cfg.tx_desc_auto_clear = true;  // 没数据时输出静音，不会有杂音
  cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
  cfg.bits_per_chan = I2S_BITS_PER_CHAN_16BIT;
  if (i2s_driver_install(kI2s, &cfg, 0, nullptr) != ESP_OK) return false;

  i2s_pin_config_t pins = {};
  pins.mck_io_num = PIN_I2S_MCLK;
  pins.bck_io_num = PIN_I2S_BCLK;
  pins.ws_io_num = PIN_I2S_LRCK;
  pins.data_out_num = PIN_I2S_DOUT;
#ifdef HAS_MIC
  pins.data_in_num = PIN_I2S_DIN;
#else
  pins.data_in_num = I2S_PIN_NO_CHANGE;
#endif
  return i2s_set_pin(kI2s, &pins) == ESP_OK;
}

#ifdef HAS_MIC
static bool mic_write(uint8_t reg, uint8_t val) {
  return lgfx::i2c::writeRegister8(kI2cPort, ES7210_ADDR, reg, val).has_value();
}

// 只改 mask 里的位
static bool mic_update(uint8_t reg, uint8_t mask, uint8_t val) {
  auto r = lgfx::i2c::readRegister8(kI2cPort, ES7210_ADDR, reg);
  if (!r.has_value()) return false;
  return mic_write(reg, (r.value() & ~mask) | (val & mask));
}

// 打开 MIC1、MIC2 两路，增益 30dB
static bool mic_select() {
  static constexpr uint8_t kGain = 10;  // 每档 3dB
  bool ok = true;
  for (int i = 0; i < 4; i++) ok = ok && mic_update(0x43 + i, 0x10, 0x00);
  ok = ok && mic_write(0x4B, 0xFF) && mic_write(0x4C, 0xFF);
  ok = ok && mic_update(0x01, 0x0B, 0x00) && mic_write(0x4B, 0x00);
  ok = ok && mic_update(0x43, 0x10, 0x10) && mic_update(0x43, 0x0F, kGain);
  ok = ok && mic_update(0x44, 0x10, 0x10) && mic_update(0x44, 0x0F, kGain);
  ok = ok && mic_write(0x12, 0x00);  // 两路用标准 I2S 的左右声道，不用 TDM
  return ok;
}

// ES7210 初始化：从模式、标准 I2S、16 位。寄存器顺序和取值移植自乐鑫 esp-adf 的
// components/esp_codec_dev/device/es7210/es7210.c（Apache-2.0）的 open、set_fs、enable 三步
static bool mic_init() {
  bool ok = mic_write(0x00, 0xFF) && mic_write(0x00, 0x41);  // 复位
  ok = ok && mic_write(0x01, 0x3F);
  ok = ok && mic_write(0x09, 0x30) && mic_write(0x0A, 0x30);  // 上电时序
  ok = ok && mic_write(0x23, 0x2A) && mic_write(0x22, 0x0A);  // 高通滤波，去掉直流
  ok = ok && mic_write(0x20, 0x0A) && mic_write(0x21, 0x2A);
  ok = ok && mic_update(0x08, 0x01, 0x00);  // 从模式，时钟来自 I2S
  ok = ok && mic_write(0x40, 0x43);         // 模拟电路 3.3V
  ok = ok && mic_write(0x41, 0x70) && mic_write(0x42, 0x70);  // 麦克风偏置 2.87V
  ok = ok && mic_write(0x07, 0x20);
  ok = ok && mic_write(0x02, 0xC1);
  ok = ok && mic_select();
  auto clock_off = lgfx::i2c::readRegister8(kI2cPort, ES7210_ADDR, 0x01);
  ok = ok && clock_off.has_value();
  // 16 位、标准 I2S 格式
  ok = ok && mic_update(0x11, 0xE0, 0x60) && mic_update(0x11, 0x03, 0x00);
  // 开始工作
  ok = ok && mic_write(0x01, clock_off.value_or(0x3F)) && mic_write(0x06, 0x00) && mic_write(0x40, 0x43);
  for (int i = 0; i < 4; i++) ok = ok && mic_write(0x47 + i, 0x08);
  ok = ok && mic_select();
  ok = ok && mic_write(0x40, 0x43) && mic_write(0x00, 0x71) && mic_write(0x00, 0x41);
  return ok;
}
#endif

// 放进待播放的缓冲，满了就等着（喇叭任务每 16ms 取走 256 个）
static void out_write(const int16_t *mono, size_t n) {
  while (n > 0) {
    uint32_t space = kOutRing - (out_w - out_r);
    if (space == 0) {
      vTaskDelay(pdMS_TO_TICKS(5));
      continue;
    }
    size_t take = n < space ? n : space;
    uint32_t w = out_w;
    for (size_t i = 0; i < take; i++) out_ring[(w + i) & (kOutRing - 1)] = mono[i];
    out_w = w + take;
    mono += take;
    n -= take;
  }
}

// 等缓冲里的声音全部放完（包括 DMA 里排着的 96ms）
static void out_drain() {
  while (out_w != out_r) vTaskDelay(pdMS_TO_TICKS(5));
  vTaskDelay(pdMS_TO_TICKS(kTxQueue * 1000 / kRate + 20));
}

static void speaker_task(void *) {
  static int16_t mono[kDmaLen];
  static int16_t stereo[kDmaLen * 2];
  bool pa = false;
  uint32_t last_sound = 0;
  for (;;) {
    if (want_rate != cur_rate && !pa) {
      i2s_set_clk(kI2s, want_rate, I2S_BITS_PER_SAMPLE_16BIT, I2S_CHANNEL_STEREO);
      cur_rate = want_rate;
      rate_changes = rate_changes + 1;
      log_i("audio: speaker now %u Hz", (unsigned)cur_rate);
    }
    if (out_flush) {
      out_r = out_w;
      out_hold = true;  // 打断扔掉的不算放空
      out_flush = false;
    }
    uint32_t r = out_r;
    uint32_t avail = out_w - r;
    // 语音回答边下边放：放空了就停下来攒，攒够或者下载完了再接着放，免得一卡一卡的
    if (out_streaming && avail == 0 && !out_hold) {
      out_hold = true;
      out_underruns = out_underruns + 1;
      uint32_t t = out_target + kPrebufferMin;
      if (!out_local) out_target = t < kPrebufferMax ? t : kPrebufferMax;
    }
    uint32_t target = out_local ? kPrebufferMin : out_target;
    if (out_hold && (avail >= target || !out_streaming)) out_hold = false;
    if (out_hold) avail = 0;
    size_t n = avail < (uint32_t)kDmaLen ? avail : kDmaLen;
    for (size_t i = 0; i < n; i++) mono[i] = out_ring[(r + i) & (kOutRing - 1)];
    out_r = r + n;
    out_played = out_played + n;
    for (size_t i = n; i < (size_t)kDmaLen; i++) mono[i] = 0;
    // 提示音叠在音乐上，音乐压到四分之一
    int16_t *ov = overlay;
    uint32_t ov_left = ov ? overlay_len - overlay_pos : 0;
    if (ov_left > 0) {
      uint32_t at = overlay_pos;
      for (int i = 0; i < kDmaLen; i++) {
        int32_t v = mono[i] / 4 + (i < (int)ov_left ? ov[at + i] : 0);
        mono[i] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
      }
      overlay_pos = at + (ov_left < (uint32_t)kDmaLen ? ov_left : kDmaLen);
      n = kDmaLen;
    }
    uint32_t t = tx_count;
    if (n > 0) {
      last_sound = t + kDmaLen;
      // 功放只在出声时打开，平时关着避免底噪。这些采样还要过 96ms 才放出来，够功放稳定
      if (!pa) {
        pa = true;
        speaker_on = true;
        digitalWrite(PIN_AUDIO_PA, HIGH);
      }
    } else if (pa && t - last_sound > kTxQueue + cur_rate / 5) {
      pa = false;
      digitalWrite(PIN_AUDIO_PA, LOW);
      speaker_on = false;
    }
    for (int i = 0; i < kDmaLen; i++) {
      ref_ring[(t + i) & (kRefRing - 1)] = mono[i];
      stereo[2 * i] = mono[i];
      stereo[2 * i + 1] = mono[i];
    }
    size_t written;
    i2s_write(kI2s, stereo, sizeof(stereo), &written, portMAX_DELAY);
    tx_count = t + kDmaLen;
  }
}

// 等喇叭切回 16kHz（拿到 out_lock 之后调用：音乐刚让出来时可能还没切回来）
static void wait_base_rate() {
  for (int i = 0; i < 400 && (cur_rate != kRate || want_rate != kRate); i++) vTaskDelay(pdMS_TO_TICKS(5));
}

// 按音乐现在的采样率合成提示音，叠在音乐上放，放完才返回
static void play_over_music(const Note *notes, int count) {
  uint32_t rate = cur_rate;
  size_t n = sound_length(notes, count, rate);
  auto *pcm = (int16_t *)heap_caps_malloc(n * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!pcm) return;
  sound_render(notes, count, rate, volume, pcm, n);
  overlay_pos = 0;
  overlay_len = n;
  overlay = pcm;
  while (overlay_pos < overlay_len) vTaskDelay(pdMS_TO_TICKS(20));
  vTaskDelay(pdMS_TO_TICKS(kTxQueue * 1000 / rate + 20));  // DMA 里的那一截也放完
  overlay = nullptr;
  overlay_len = 0;
  free(pcm);
}

static void audio_task(void *) {
  for (;;) {
    SoundId id;
    if (xQueueReceive(queue, &id, portMAX_DELAY) != pdTRUE) continue;
    int count;
    const Note *notes = sound_melody(id, count);
    size_t n = sound_length(notes, count, kRate);
    auto *pcm = (int16_t *)heap_caps_malloc(n * sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!pcm) pcm = (int16_t *)malloc(n * sizeof(int16_t));
    if (!pcm) continue;
    sound_render(notes, count, kRate, volume, pcm, n);
    for (;;) {
      if (xSemaphoreTake(out_lock, pdMS_TO_TICKS(20)) == pdTRUE) {
        wait_base_rate();
        out_write(pcm, n);
        xSemaphoreGive(out_lock);
        break;
      }
      if (music_active) {
        play_over_music(notes, count);
        break;
      }
    }
    free(pcm);
  }
}

#ifdef HAS_MIC
static void capture_task(void *) {
  static int16_t buf[kDmaLen * 2];
  static int16_t mic[kDmaLen];
  static int16_t ref[kDmaLen];
  static int16_t mic16[kDmaLen];
  static int16_t ref16[kDmaLen];
  static Resampler16k mic_rs, ref_rs;
  RefAligner aligner;
  uint32_t rx_count = 0;
  uint32_t seen_changes = 0;
  uint32_t rate = kRate;
  for (;;) {
    size_t got = 0;
    if (i2s_read(kI2s, buf, sizeof(buf), &got, pdMS_TO_TICKS(100)) != ESP_OK || got == 0) continue;
    size_t frames = got / 4;
    if (rate_changes != seen_changes) {
      // 换了采样率：重新量喇叭和麦克风差多少，重新开始转 16kHz
      seen_changes = rate_changes;
      rate = cur_rate;
      aligner = RefAligner();
      mic_rs.reset(rate);
      ref_rs.reset(rate);
    }
    const int32_t lead = kRefLead * (int32_t)rate / kRate;
    uint32_t t = tx_count;
    rx_count += frames;
    // 刚读到的最后一个采样录下时，喇叭大约正放到 t - kTxQueue
    aligner.add((int32_t)(t - kTxQueue - rx_count));
    int32_t offset = aligner.offset();
    uint32_t first = rx_count - frames;
    for (size_t i = 0; i < frames; i++) {
      mic[i] = (int16_t)(((int32_t)buf[2 * i] + buf[2 * i + 1]) / 2);  // 两个麦克风取平均
      ref[i] = ring_at(ref_ring, kRefRing, t, first + i + offset + lead);
    }
    // 唤醒词、回声消除、音乐律动都要 16kHz
    const int16_t *m = mic, *r = ref;
    size_t n = frames;
    if (rate != kRate) {
      n = mic_rs.process(mic, frames, mic16, kDmaLen);
      ref_rs.process(ref, frames, ref16, kDmaLen);
      m = mic16;
      r = ref16;
    }
    // 音乐律动：放音乐时直接用放出去的声音，平时听麦克风（喇叭响着时不听，免得听到自己）
    const int16_t *heard = music_active ? r : speaker_on ? nullptr : m;
    if (heard) {
      uint32_t w = listen_w;
      for (size_t i = 0; i < n; i++) listen_ring[(w + i) & (kListenRing - 1)] = heard[i];
      listen_w = w + n;
    }
    CaptureSink sink = capture_sink;
    if (sink && n > 0) sink(m, r, n, capture_ctx);
  }
}
#endif

bool audio_available() { return ready; }

void audio_begin() {
  pinMode(PIN_AUDIO_PA, OUTPUT);
  digitalWrite(PIN_AUDIO_PA, LOW);
  lgfx::i2c::init(kI2cPort, PIN_TOUCH_SDA, PIN_TOUCH_SCL);
  // 先让 I2S 输出 MCLK，再配置解码芯片
  if (!i2s_init()) {
    log_w("audio: i2s init failed");
    return;
  }
  if (!codec_init()) {
    log_w("audio: ES8311 not responding");
    return;
  }
#ifdef HAS_MIC
  mic_ready = mic_init();
  if (!mic_ready) log_w("audio: ES7210 not responding, no microphone");
#endif
  out_ring = (int16_t *)heap_caps_calloc(kOutRing, sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  ref_ring = (int16_t *)heap_caps_calloc(kRefRing, sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  listen_ring = (int16_t *)heap_caps_calloc(kListenRing, sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!out_ring || !ref_ring || !listen_ring) {
    log_w("audio: no memory for buffers");
    return;
  }
  out_lock = xSemaphoreCreateMutex();
  queue = xQueueCreate(4, sizeof(SoundId));
  // 喇叭任务优先级高一些：断了供数 I2S 会自己放静音，放出的声音和编号就对不上了
  xTaskCreatePinnedToCore(speaker_task, "speaker", 3072, nullptr, 10, nullptr, 1);
  xTaskCreate(audio_task, "audio", 4096, nullptr, 2, nullptr);
#ifdef HAS_MIC
  if (mic_ready) xTaskCreatePinnedToCore(capture_task, "capture", 8192, nullptr, 6, nullptr, 0);
#endif
  ready = true;
  log_i("audio ready");
}

void audio_set_volume(int v) {
  volume = v < 0 ? 0 : v > 100 ? 100 : v;
}

void audio_play(SoundId id) {
  if (ready) xQueueSend(queue, &id, 0);
}

void audio_stream_begin() {
  if (!ready) return;
  if (music_active) music_yield = true;  // 音乐让出来
  xSemaphoreTake(out_lock, portMAX_DELAY);
  music_yield = false;
  wait_base_rate();
  out_local = false;
  out_hold = true;
  out_underruns = 0;
  out_streaming = true;
}

bool audio_stream_try_begin() {
  if (!ready) return false;
  // 音乐在放就请它让出来（播放器暂停），等一会儿；AI 聊天在说话时不等
  bool music = music_active;
  if (music) music_yield = true;
  bool ok = xSemaphoreTake(out_lock, music ? pdMS_TO_TICKS(1500) : 0) == pdTRUE;
  music_yield = false;
  if (!ok) return false;
  wait_base_rate();
  out_local = true;
  out_hold = true;
  out_underruns = 0;
  out_streaming = true;
  return true;
}

void audio_stream_write(const int16_t *mono, size_t n) {
  if (!ready) return;
  // 默认音量 60 时原样播放，调大最多放大到 1.5 倍
  int gain = volume * 256 / 60;
  if (gain > 384) gain = 384;
  int16_t buf[256];
  while (n > 0) {
    size_t take = n < 256 ? n : 256;
    for (size_t i = 0; i < take; i++) {
      int v = mono[i] * gain / 256;
      buf[i] = v > 32767 ? 32767 : v < -32768 ? -32768 : v;
    }
    out_write(buf, take);
    mono += take;
    n -= take;
  }
}

void audio_stream_flush() {
  if (!ready) return;
  out_flush = true;
  while (out_flush) vTaskDelay(1);
}

size_t audio_stream_pending() { return ready ? out_w - out_r : 0; }

uint32_t audio_stream_played() { return out_played; }

uint32_t audio_output_delay() { return kTxQueue; }

void audio_stream_done() { out_streaming = false; }

void audio_stream_end() {
  if (!ready) return;
  out_streaming = false;
  out_drain();
  if (out_local) {
    log_i("audio: video sound done, ran dry %u times", (unsigned)out_underruns);
    out_local = false;
  } else {
    uint32_t t = out_target;
    if (out_underruns == 0 && t > kPrebufferMin) out_target = t - kRate / 10;
    log_i("audio: answer done, ran dry %u times, now buffering %u ms", (unsigned)out_underruns,
          (unsigned)(out_target * 1000 / kRate));
  }
  xSemaphoreGive(out_lock);
}

bool audio_mic_available() { return mic_ready && ready; }

void audio_set_capture_sink(CaptureSink sink, void *ctx) {
  capture_ctx = ctx;
  capture_sink = sink;
}

size_t audio_mic_listen(int16_t *mono, size_t max) {
  static bool paused = true;
  if (!mic_ready || !ready) return 0;
  // 最多等 100ms 攒够声音
  auto quiet = []() { return !speaker_on || music_active; };
  for (int i = 0; i < 20 && listen_w - listen_r < max && quiet(); i++) vTaskDelay(pdMS_TO_TICKS(5));
  if (!quiet()) {
    paused = true;
    return 0;
  }
  uint32_t w = listen_w;
  if (paused || w - listen_r > kListenRing) listen_r = w - max;  // 停过一阵，旧的声音不要了
  paused = false;
  uint32_t avail = w - listen_r;
  size_t n = avail < max ? avail : max;
  for (size_t i = 0; i < n; i++) mono[i] = listen_ring[(listen_r + i) & (kListenRing - 1)];
  listen_r += n;
  return n;
}

bool audio_music_begin(uint32_t rate) {
  if (!ready || rate < 8000 || rate > 48000) return false;
  if (xSemaphoreTake(out_lock, pdMS_TO_TICKS(1000)) != pdTRUE) return false;
  music_yield = false;
  music_active = true;
  want_rate = rate;
  // 等喇叭停下来切好采样率（刚响过提示音时要等它放完，大约半秒）
  for (int i = 0; i < 400 && cur_rate != rate; i++) vTaskDelay(pdMS_TO_TICKS(5));
  if (cur_rate != rate) {
    log_w("audio: could not switch to %u Hz", (unsigned)rate);
    audio_music_end();
    return false;
  }
  return true;
}

void audio_music_write(const int16_t *mono, size_t n) {
  if (!music_active) return;
  // 歌一般已经录得很响：音量 100 时原样放，小于 100 按比例调小
  int gain = volume * 256 / 100;
  int16_t buf[256];
  while (n > 0) {
    size_t take = n < 256 ? n : 256;
    for (size_t i = 0; i < take; i++) buf[i] = (int16_t)(mono[i] * gain / 256);
    out_write(buf, take);
    mono += take;
    n -= take;
  }
}

size_t audio_music_pending() { return music_active ? out_w - out_r : 0; }

void audio_music_flush() {
  if (music_active) audio_stream_flush();
}

void audio_music_end() {
  if (!music_active) return;
  audio_stream_flush();
  want_rate = kRate;  // 功放关了（静下来 0.3 秒）就切回 16kHz
  music_active = false;
  xSemaphoreGive(out_lock);
}

bool audio_music_wanted() { return music_yield; }

#endif
