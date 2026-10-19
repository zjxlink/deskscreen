#pragma once

// 两种板子都按 320x240 横屏使用，UI 代码不用区分板子
#define SCREEN_W 320
#define SCREEN_H 240

#if defined(BOARD_BOX3)
  // 乐鑫 ESP32-S3-BOX-3，引脚与 LovyanGFX 自动识别配置一致
  #define BOARD_NAME        "box3"  // 和 platformio.ini 的环境名一致，OTA 用它找对应的固件
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
  #define HAS_TOUCH                // GT911 电容触摸
  #define PIN_TOUCH_SDA     8
  #define PIN_TOUCH_SCL     18
  #define PIN_TOUCH_INT     3
  // 喇叭：ES8311 解码芯片（I2C 和触摸共用）+ 功放，引脚来自乐鑫 esp-bsp
  #define HAS_AUDIO
  #define PIN_I2S_MCLK      2
  #define PIN_I2S_BCLK      17
  #define PIN_I2S_LRCK      45
  #define PIN_I2S_DOUT      15
  #define PIN_AUDIO_PA      46   // 功放使能，高电平打开
  #define ES8311_ADDR       0x18

#elif defined(BOARD_DESK_V1)
  // 自己画的板子：2.0 寸 IPS ST7789（240x320）
  // 引脚尽量和 BOX-3 一样，画 PCB 时如有改动，只需改这里
  #define BOARD_NAME        "desk_v1"
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
