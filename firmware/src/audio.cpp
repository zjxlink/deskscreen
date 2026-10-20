#include "audio.h"

#include <Arduino.h>

#include "board.h"

#ifndef HAS_AUDIO

bool audio_available() { return false; }
void audio_begin() {}
void audio_set_volume(int) {}
void audio_play(SoundId) {}
void audio_stream_begin() {}
void audio_stream_write(const int16_t *, size_t) {}
void audio_stream_end() {}
bool audio_mic_available() { return false; }
void audio_mic_begin() {}
size_t audio_mic_read(int16_t *, size_t) { return 0; }
void audio_mic_end() {}
size_t audio_mic_listen(int16_t *, size_t) { return 0; }

#else

#include <LovyanGFX.hpp>
#include <driver/i2c.h>
#include <driver/i2s.h>
#include <esp_heap_caps.h>

static constexpr int kRate = 16000;            // 提示音用 16kHz 足够
static constexpr int kMclk = kRate * 256;      // 4.096MHz，ES8311 从 MCLK 引脚取时钟
static constexpr i2s_port_t kI2s = I2S_NUM_0;
static constexpr int kI2cPort = I2C_NUM_0;     // 和触摸芯片共用，走 LovyanGFX 的 I2C 实现，避免两套驱动冲突

static QueueHandle_t queue;
static SemaphoreHandle_t out_lock;  // 提示音和语音回答轮流用喇叭
static volatile int volume = 60;
static bool ready = false;
static bool mic_ready = false;
static SemaphoreHandle_t mic_lock;        // AI 聊天录音时一直占着，音乐律动每次读之前试一下
static volatile bool speaker_on = false;  // 喇叭在响时音乐律动不听，免得听到自己的声音

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
  cfg.dma_buf_count = 6;
  cfg.dma_buf_len = 256;
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

static void write_stereo(const int16_t *mono, size_t n) {
  static int16_t buf[256 * 2];
  size_t pos = 0;
  while (pos < n) {
    size_t frames = n - pos < 256 ? n - pos : 256;
    for (size_t i = 0; i < frames; i++) {
      int16_t s = mono ? mono[pos + i] : 0;
      buf[2 * i] = s;
      buf[2 * i + 1] = s;
    }
    size_t written;
    i2s_write(kI2s, buf, frames * 4, &written, portMAX_DELAY);
    pos += frames;
  }
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

    // 功放只在播放时打开，平时关着，避免底噪
    xSemaphoreTake(out_lock, portMAX_DELAY);
    speaker_on = true;
    digitalWrite(PIN_AUDIO_PA, HIGH);
    delay(20);
    write_stereo(pcm, n);
    write_stereo(nullptr, kRate / 10);  // 再送 100ms 静音，确保最后一个音放完
    delay(50);
    digitalWrite(PIN_AUDIO_PA, LOW);
    speaker_on = false;
    xSemaphoreGive(out_lock);
    free(pcm);
  }
}

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
  out_lock = xSemaphoreCreateMutex();
  mic_lock = xSemaphoreCreateMutex();
  queue = xQueueCreate(4, sizeof(SoundId));
  xTaskCreate(audio_task, "audio", 4096, nullptr, 2, nullptr);
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
  xSemaphoreTake(out_lock, portMAX_DELAY);
  speaker_on = true;
  digitalWrite(PIN_AUDIO_PA, HIGH);
  delay(20);
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
    write_stereo(buf, take);
    mono += take;
    n -= take;
  }
}

void audio_stream_end() {
  if (!ready) return;
  write_stereo(nullptr, kRate / 10);
  delay(50);
  digitalWrite(PIN_AUDIO_PA, LOW);
  speaker_on = false;
  xSemaphoreGive(out_lock);
}

bool audio_mic_available() { return mic_ready; }

// 没人读的时候 DMA 缓冲里攒着旧的录音，先读掉
static void mic_flush() {
  static int16_t junk[256 * 2];
  size_t got = 0;
  for (int i = 0; i < 16; i++) {
    if (i2s_read(kI2s, junk, sizeof(junk), &got, 0) != ESP_OK || got == 0) break;
  }
}

void audio_mic_begin() {
  if (!mic_ready) return;
  xSemaphoreTake(mic_lock, portMAX_DELAY);  // 等音乐律动读完手上这一小段
  mic_flush();
}

void audio_mic_end() {
  if (mic_ready) xSemaphoreGive(mic_lock);
}

static size_t mic_read(int16_t *mono, size_t max) {
  static int16_t buf[256 * 2];
  if (max > 256) max = 256;
  size_t got = 0;
  if (i2s_read(kI2s, buf, max * 4, &got, pdMS_TO_TICKS(100)) != ESP_OK) return 0;
  size_t frames = got / 4;
  for (size_t i = 0; i < frames; i++) mono[i] = (int16_t)(((int32_t)buf[2 * i] + buf[2 * i + 1]) / 2);
  return frames;
}

size_t audio_mic_read(int16_t *mono, size_t max) {
  return mic_ready ? mic_read(mono, max) : 0;
}

size_t audio_mic_listen(int16_t *mono, size_t max) {
  static bool paused = true;
  if (!mic_ready) return 0;
  if (speaker_on || xSemaphoreTake(mic_lock, 0) != pdTRUE) {
    paused = true;
    return 0;
  }
  if (paused) mic_flush();  // 停过一阵，缓冲里是喇叭的声音或者旧的录音
  paused = false;
  size_t n = mic_read(mono, max);
  xSemaphoreGive(mic_lock);
  return n;
}

#endif
