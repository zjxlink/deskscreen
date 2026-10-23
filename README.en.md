<div align="center">

# DeskScreen

**Open-source desk display firmware for the ESP32-S3-BOX-3: clock & weather, GitHub contribution graph, pomodoro, a pixel cat, AI voice chat, photos, movies and music on one tiny screen.**

[![Build](https://github.com/zjxlink/test-repo/actions/workflows/build.yml/badge.svg)](https://github.com/zjxlink/test-repo/actions/workflows/build.yml)
[![Release](https://img.shields.io/github/v/release/zjxlink/test-repo)](https://github.com/zjxlink/test-repo/releases/latest)
[![License: GPL-3.0](https://img.shields.io/github/license/zjxlink/test-repo)](LICENSE)
![ESP32-S3](https://img.shields.io/badge/ESP32--S3-BOX--3-E7352C?logo=espressif&logoColor=white)
![PlatformIO](https://img.shields.io/badge/PlatformIO-Arduino-F5822A?logo=platformio&logoColor=white)
![LVGL](https://img.shields.io/badge/UI-LVGL%208-343839)

[简体中文](README.md) · **English**

[Web flasher](https://zjxlink.github.io/test-repo/) · [Features (zh)](docs/features.md) · [Changelog (zh)](CHANGELOG.md) · [Developer guide (zh)](docs/development.md)

<img src="docs/images/hero.png" alt="DeskScreen screens" width="860">

</div>

> The on-screen UI and the detailed docs are in Simplified Chinese. This page is an English overview.

## ✨ Highlights

- **Flash from the browser**: open the [web flasher](https://zjxlink.github.io/test-repo/) in desktop Chrome or Edge, plug in USB, click once. No toolchain needed.
- **No app required**: Wi-Fi is set up from your phone through a captive portal; everything else is configured in a mobile-friendly web page at `http://deskscreen.local`.
- **Made for developers**: GitHub contribution heatmap, stock watchlist (CN/HK/US), news and RSS, weather forecast, plus pomodoro and break reminders.
- **A pixel cat that lives on your desk**: 小橘 ("Orange") reacts to your commits, pomodoros, stocks and to being shaken, dances to music, and has a runner mini-game.
- **Talk to the cat**: say the wake word 「喵喵同学」 for realtime voice chat with barge-in (Alibaba Cloud Bailian Qwen-Omni Realtime, bring your own API key).
- **A tiny media center**: microSD photo album (JPG, PNG, GIF, MJPEG video, phone upload), a movie player with sound, and a music player with playlists, cover art and lyrics.
- **Low maintenance**: automatic OTA updates with rollback, display self-recovery, and a diagnostics page in the web UI.

## 📸 Screenshots

<table>
  <tr>
    <td align="center"><img src="docs/images/screen-clock.png" width="260"><br>Clock, moon phase, sunrise/sunset</td>
    <td align="center"><img src="docs/images/screen-weather.png" width="260"><br>Rain chart and 3-day forecast</td>
    <td align="center"><img src="docs/images/screen-github.png" width="260"><br>GitHub contributions</td>
  </tr>
  <tr>
    <td align="center"><img src="docs/images/screen-stocks.png" width="260"><br>Stock watchlist</td>
    <td align="center"><img src="docs/images/screen-news.png" width="260"><br>News and RSS</td>
    <td align="center"><img src="docs/images/screen-pomodoro.png" width="260"><br>Pomodoro</td>
  </tr>
  <tr>
    <td align="center"><img src="docs/images/screen-pet.png" width="260"><br>Pixel cat</td>
    <td align="center"><img src="docs/images/screen-runner.png" width="260"><br>Runner mini-game</td>
    <td align="center"><img src="docs/images/screen-chat.png" width="260"><br>AI voice chat</td>
  </tr>
  <tr>
    <td align="center"><img src="docs/images/screen-music.png" width="260"><br>Music player</td>
    <td align="center"><img src="docs/images/screen-movie.png" width="260"><br>Movies</td>
    <td align="center"><img src="docs/images/screen-light.png" width="260"><br>Light theme</td>
  </tr>
</table>

## 🔧 Hardware

- [Espressif ESP32-S3-BOX-3](https://github.com/espressif/esp-box) (16 MB flash, 16 MB PSRAM, 2.4" 320×240 touch screen, mics, speaker, IMU). The firmware needs the 16 MB PSRAM.
- Optional BOX-3 SENSOR dock: temperature/humidity, mmWave presence radar (screen off when nobody is around), and a microSD slot for photos, movies and music. Features that need it turn off automatically without it.

## 🚀 Quick start

1. **Flash**: open <https://zjxlink.github.io/test-repo/> in desktop Chrome or Edge, connect the BOX-3 over USB-C and follow the page.
2. **Wi-Fi**: on first boot the board opens a `DeskScreen-XXXX` hotspot. Join it from your phone and pick your Wi-Fi (or browse to `192.168.4.1`). Hold BOOT for 3 s to redo this later.
3. **Settings**: on the same network, open `http://deskscreen.local` (or the IP on screen) and enter the 6-digit pairing code shown on the display. You can also long-press the screen for 2 s and scan the QR code.
4. **Optional services**: city, GitHub token, stocks, news sources and the Bailian API key for voice chat live under 「服务」 (Services).

## 🔄 OTA updates

The board checks [GitHub Releases](https://github.com/zjxlink/test-repo/releases) daily and installs new versions between 3 and 5 a.m. (can be turned off). A new firmware must run for 30 s before it is accepted, otherwise the board rolls back. If GitHub is slow where you are, run the [OTA mirror](server/ota-mirror/README.md) on your own server and set its URL in the web UI.

## 🛠️ Build from source

```bash
git clone https://github.com/zjxlink/test-repo.git
cd test-repo/firmware
pio run -e box3 -t upload    # build and flash the BOX-3
pio device monitor           # serial log
pio test -e native           # unit tests on your computer
```

Requires [PlatformIO](https://platformio.org). See the [developer guide](docs/development.md) (Chinese) for the source layout, release process and font generation.

## 🤝 Contributing

Issues and pull requests are welcome, in English or Chinese. Please read [CONTRIBUTING](CONTRIBUTING.md) first.

## 📄 License

[GPL-3.0](LICENSE). Third-party components keep their own licenses: LVGL (MIT), LovyanGFX (FreeBSD), WiFiManager (MIT), ArduinoJson (MIT), minimp3 (CC0), dr_flac (public domain / MIT-0), Noto Sans SC (SIL OFL 1.1), Espressif esp-sr wake word models (Espressif license, Espressif chips only). Weather, market, news and Bing image data come from public endpoints for personal use only.
