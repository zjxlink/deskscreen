#pragma once

// 两种板子都按 320x240 横屏使用，UI 代码不用区分板子
#define SCREEN_W 320
#define SCREEN_H 240

#if defined(BOARD_BOX3)
  // 乐鑫 ESP32-S3-BOX-3，引脚与 LovyanGFX 自动识别配置一致
  #define LCD_PANEL_ILI9342
  #define LCD_SPI_FREQ      40000000
  #define PIN_LCD_SCLK      7
  #define PIN_LCD_MOSI      6
  #define PIN_LCD_DC        4
  #define PIN_LCD_CS        5
  #define PIN_LCD_RST       -1   // 复位脚 GPIO48 与触摸芯片共用，固件不能驱动它
  #define PIN_LCD_RST_SHARED 48  // 只上拉，保持屏幕不在复位状态
  #define PIN_LCD_BL        47
  #define PIN_BUTTON        0    // 侧面的 BOOT 键

#elif defined(BOARD_DESK_V1)
  // 自己画的板子：2.0 寸 IPS ST7789（240x320）
  // 引脚尽量和 BOX-3 一样，画 PCB 时如有改动，只需改这里
  #define LCD_PANEL_ST7789
  #define LCD_SPI_FREQ      80000000
  #define PIN_LCD_SCLK      7
  #define PIN_LCD_MOSI      6
  #define PIN_LCD_DC        4
  #define PIN_LCD_CS        5
  #define PIN_LCD_RST       48
  #define PIN_LCD_BL        47
  #define PIN_BUTTON        0

#else
  #error "请在 platformio.ini 中选择板子：BOARD_BOX3 或 BOARD_DESK_V1"
#endif
