#include "display.h"

#include <Arduino.h>
#include <LovyanGFX.hpp>
#include <driver/i2c.h>
#include <esp_heap_caps.h>
#include <lvgl.h>

#include "board.h"
#include "panel_watch.h"

class LGFX : public lgfx::LGFX_Device {
#if defined(LCD_PANEL_ILI9342)
  lgfx::Panel_ILI9342 _panel;
#else
  lgfx::Panel_ST7789 _panel;
#endif
  lgfx::Bus_SPI _bus;
  lgfx::Light_PWM _light;
#ifdef HAS_TOUCH
  lgfx::Touch_GT911 _touch;
#endif

 public:
  LGFX() {
    {
      auto cfg = _bus.config();
      cfg.spi_host = SPI2_HOST;
      cfg.spi_mode = 0;
      cfg.freq_write = LCD_SPI_FREQ;
      cfg.freq_read = 16000000;
      cfg.spi_3wire = true;
      cfg.use_lock = true;
      cfg.dma_channel = SPI_DMA_CH_AUTO;
      cfg.pin_sclk = PIN_LCD_SCLK;
      cfg.pin_mosi = PIN_LCD_MOSI;
      cfg.pin_miso = -1;
      cfg.pin_dc = PIN_LCD_DC;
      _bus.config(cfg);
      _panel.setBus(&_bus);
    }
    {
      auto cfg = _panel.config();
      cfg.pin_cs = PIN_LCD_CS;
      cfg.pin_rst = PIN_LCD_RST;
      cfg.pin_busy = -1;
      cfg.readable = false;
      cfg.bus_shared = false;
#if defined(LCD_PANEL_ILI9342)
      cfg.offset_rotation = 1;
#else
      cfg.panel_width = 240;
      cfg.panel_height = 320;
      cfg.memory_width = 240;
      cfg.memory_height = 320;
      cfg.invert = true;  // IPS 屏一般需要反色
#endif
      _panel.config(cfg);
    }
    {
      auto cfg = _light.config();
      cfg.pin_bl = PIN_LCD_BL;
      cfg.invert = false;
      cfg.freq = 12000;
      cfg.pwm_channel = 0;
      _light.config(cfg);
      _panel.setLight(&_light);
    }
#ifdef HAS_TOUCH
    {
      // 参数与 LovyanGFX 对 BOX-3 的自动识别配置一致，I2C 地址 0x14/0x5D 会自动探测
      auto cfg = _touch.config();
      cfg.pin_int = PIN_TOUCH_INT;
      cfg.pin_sda = PIN_TOUCH_SDA;
      cfg.pin_scl = PIN_TOUCH_SCL;
      cfg.i2c_addr = 0x14;
      cfg.i2c_port = I2C_NUM_0;
      cfg.x_min = 0;
      cfg.x_max = 319;
      cfg.y_min = 0;
      cfg.y_max = 279;  // 屏幕下方还有一个红色触摸点
      cfg.offset_rotation = 2;
      cfg.bus_shared = false;
      _touch.config(cfg);
      _panel.setTouch(&_touch);
    }
#endif
    setPanel(&_panel);
  }
};

static LGFX tft;

// 每次刷新 1/6 屏，缓冲放在内部 DMA 内存里。flush_cb 是同步写屏的，双缓冲没有好处，只用一块省下 25KB 内部 RAM
static constexpr uint32_t kBufLines = SCREEN_H / 6;
static lv_disp_draw_buf_t draw_buf;
static lv_disp_drv_t disp_drv;
static PanelWatch watch;

static constexpr uint32_t kRedrawMs = 60000;  // 每分钟整屏重画一次，显存偶尔被写坏也能自己好

// 读屏幕控制芯片的电源状态、扫描方向、像素格式（0Ah、0Bh、0Ch），拼成一个读数。
// BOX-3 的屏是三线 SPI，数据线复用来读
static uint32_t read_panel_state() {
  auto *panel = tft.getPanel();
  uint32_t pm = panel->readCommand(0x0A, 0, 1) & 0xFF;
  uint32_t madctl = panel->readCommand(0x0B, 0, 1) & 0xFF;
  uint32_t colmod = panel->readCommand(0x0C, 0, 1) & 0xFF;
  return colmod << 16 | madctl << 8 | pm;
}

static void flush_cb(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_p) {
  uint32_t w = area->x2 - area->x1 + 1;
  uint32_t h = area->y2 - area->y1 + 1;
  tft.startWrite();
  tft.setAddrWindow(area->x1, area->y1, w, h);
  tft.writePixels((lgfx::rgb565_t *)&color_p->full, w * h);
  tft.endWrite();
  lv_disp_flush_ready(drv);
}

void display_init() {
#ifdef PIN_LCD_RST_SHARED
  pinMode(PIN_LCD_RST_SHARED, INPUT_PULLUP);
#endif
  tft.init();
  tft.setRotation(1);
  tft.fillScreen(0);
  uint32_t st1 = read_panel_state();
  uint32_t st2 = read_panel_state();
  watch.begin(st1, st2, millis());
  log_i("panel state 0x%06X/0x%06X, %s", (unsigned)st1, (unsigned)st2,
        watch.readable() ? "watching for resets" : "unreadable, re-init every 10 min");

  lv_init();

  size_t buf_bytes = SCREEN_W * kBufLines * sizeof(lv_color_t);
  auto *buf = (lv_color_t *)heap_caps_malloc(buf_bytes, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
  lv_disp_draw_buf_init(&draw_buf, buf, nullptr, SCREEN_W * kBufLines);

  lv_disp_drv_init(&disp_drv);
  disp_drv.hor_res = SCREEN_W;
  disp_drv.ver_res = SCREEN_H;
  disp_drv.flush_cb = flush_cb;
  disp_drv.draw_buf = &draw_buf;
  lv_disp_drv_register(&disp_drv);
}

void display_set_brightness(uint8_t level) {
  tft.setBrightness(level);
}

bool display_touch(int &x, int &y) {
#ifdef HAS_TOUCH
  lgfx::touch_point_t tp;
  if (tft.getTouch(&tp, 1) == 0) return false;
  x = tp.x;
  y = tp.y;
  return true;
#else
  (void)x;
  (void)y;
  return false;
#endif
}

void display_check_panel() {
  static uint32_t last_redraw = 0;
  uint32_t now = millis();
  if (now - last_redraw >= kRedrawMs) {
    last_redraw = now;
    lv_obj_invalidate(lv_scr_act());
  }
  if (!watch.due(now)) return;
  uint32_t st = watch.readable() ? read_panel_state() : 0;
  if (!watch.check(st, now)) return;
  if (watch.readable()) log_w("panel state 0x%06X, expected 0x%06X, re-initializing", (unsigned)st, (unsigned)watch.baseline());

  // 不碰复位脚，只重新发一遍初始化命令；亮度和方向由 LovyanGFX 恢复
  tft.init_without_reset();
  watch.recovered(watch.readable() ? read_panel_state() : 0, millis());
  // 芯片复位后显存是乱的，整屏重画
  last_redraw = millis();
  lv_obj_invalidate(lv_scr_act());
}

uint32_t display_panel_recoveries() {
  return watch.recoveries();
}

bool display_panel_watching() {
  return watch.readable();
}
