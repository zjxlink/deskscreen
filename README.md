# 桌面信息屏

基于 ESP32-S3 的桌面小屏幕，显示时间、日期和天气。目标用户是程序员和科技爱好者，后续会加入 GitHub、股票行情、番茄钟等功能。

![界面预览](docs/preview.png)

## 当前功能（v0.1）

- 手机配网：首次开机设备会开一个 WiFi 热点，手机连上后选择家里的 WiFi 即可，不用装 App
- 时间日期：NTP 自动对时（北京时间）
- 天气：当前气温、天气、今日最高和最低气温、湿度。数据来自 [Open-Meteo](https://open-meteo.com)，免费，不需要 API Key，每 15 分钟刷新一次
- 网页设置：同一 WiFi 下用浏览器打开 `http://deskscreen.local`（或屏幕上显示的 IP），可以设置城市（支持按城市名查坐标）和屏幕亮度
- 重新配网：长按 BOOT 键 3 秒，或者在设置页面里点「重新配置 WiFi」

## 硬件

| 板子 | 屏幕 | 编译环境 |
|---|---|---|
| 乐鑫 ESP32-S3-BOX-3（开发用） | 2.4 寸 ILI9342C，320×240 | `box3` |
| 自己画的 PCB（量产用） | 2.0 寸 IPS ST7789，240×320 横屏使用 | `desk_v1` |

两种板子的界面都按 320×240 横屏设计，UI 代码通用。量产板的引脚在 `firmware/include/board.h` 中，目前和 BOX-3 保持一致，画 PCB 时如有改动，改这里就行。

## 编译和烧录

需要安装 [PlatformIO](https://platformio.org)（VS Code 插件或命令行都可以）。

```bash
cd firmware
pio run -e box3 -t upload    # 编译并烧录到 BOX-3
pio device monitor           # 查看串口日志
```

BOX-3 用 USB-C 连电脑就能烧录。如果烧录失败，按住 BOOT 键再按一下 RESET 键进入下载模式后重试。

### 不装软件，用网页烧录

每次推送代码，GitHub Actions 都会自动编译，并生成一个合并好的 `firmware-merged.bin`（在 Actions 页面对应运行记录底部的 Artifacts 里下载，`firmware-box3` 是 BOX-3 用的）。

1. 用 Chrome 或 Edge 打开 https://espressif.github.io/esptool-js/
2. 用 USB-C 连上设备，点 **Connect**，选择设备的串口
3. Flash Address 填 `0x0`，选择 `firmware-merged.bin`，点 **Program**
4. 烧完按一下 RESET 键

本地编译时，这个文件也会生成在 `firmware/.pio/build/<环境名>/firmware-merged.bin`。

## 目录结构

```
firmware/
  platformio.ini       编译配置（box3 / desk_v1 两个环境）
  include/board.h      各板子的引脚定义
  include/lv_conf.h    LVGL 配置
  src/main.cpp         启动流程、主循环
  src/display.cpp      屏幕驱动（LovyanGFX）和 LVGL 对接
  src/ui.cpp           界面
  src/weather.cpp      后台拉取天气
  src/web_config.cpp   局域网设置页面
  src/settings.cpp     设置保存（NVS）
  src/fonts/           中文字体（自动生成，不要手改）
  tools/gen_fonts.py   中文字体生成脚本
```

## 中文字体

LVGL 自带的中文字体以繁体和日文为主，缺很多简体字，所以用 [Noto Sans SC](https://fonts.google.com/noto/specimen/Noto+Sans+SC)（SIL OFL 协议，可以商用）自己生成：

- `font_cn_16`：GB2312 一级汉字（3755 个常用字）加上界面用到的字，用于普通文字和城市名
- `font_cn_24`：只含界面用到的字，用于标题和天气描述

**改了界面上的中文后要重新生成字体**，否则新的字会显示成方框：

```bash
cd firmware
npm install lv_font_conv@1.5.2 @expo-google-fonts/noto-sans-sc@0.4.4
python3 tools/gen_fonts.py node_modules/@expo-google-fonts/noto-sans-sc/400Regular/NotoSansSC_400Regular.ttf
```

## 路线图

- [x] v0.1 时间、天气、配网、网页设置
- [ ] GitHub 提交热力图、Star 数
- [ ] 自选股和基金行情
- [ ] 多页面切换（触摸或按键）
- [ ] 番茄钟、待办事项
- [ ] OTA 在线升级
- [ ] 夜间自动调暗
- [ ] 量产板 PCB 和外壳
