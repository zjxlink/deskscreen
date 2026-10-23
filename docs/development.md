# 开发指南

编译、测试、发布和代码结构。普通用户刷机看 [README](../README.md#快速上手) 就够了。

## 编译和烧录

需要安装 [PlatformIO](https://platformio.org)（VS Code 插件或命令行都可以）。

```bash
cd firmware
pio run -e box3 -t upload    # 编译并烧录到 BOX-3
pio device monitor           # 查看串口日志
```

BOX-3 用 USB-C 连电脑就能烧录。如果烧录失败，按住 BOOT 键再按一下 RESET 键进入下载模式后重试。


## 单元测试

不依赖硬件的逻辑（解析、状态机、算法）都有单元测试，在电脑上就能跑：

```bash
cd firmware
pio test -e native
```

每次推送和 PR，GitHub Actions 都会跑单元测试并编译 `box3`、`desk_v1` 两个环境的固件。

## 刷开发版


每次推送代码，GitHub Actions 都会自动编译，并生成一个合并好的 `firmware-merged.bin`（在 Actions 页面对应运行记录底部的 Artifacts 里下载，`firmware-box3` 是 BOX-3 用的）。

1. 用 Chrome 或 Edge 打开 https://espressif.github.io/esptool-js/
2. 用 USB-C 连上设备，点 **Connect**，选择设备的串口
3. Flash Address 填 `0x0`，选择 `firmware-merged.bin`，点 **Program**
4. 烧完按一下 RESET 键

本地编译时，这个文件也会生成在 `firmware/.pio/build/<环境名>/firmware-merged.bin`。


## 发布新版本


两种方式任选一种：

- **网页（手机也行）**：打开仓库的 Actions → Release → Run workflow，填写版本号（如 `v0.5.0`），点运行。会在 `main` 的最新提交上创建标签
- **命令行**：`git tag v0.5.0 && git push origin v0.5.0`

之后 GitHub Actions 会检查版本号、跑单元测试、编译两个板子的固件、生成 `manifest.json` 并创建 Release。设备从 `releases/latest/download/manifest.json` 检查更新，只有正式版本号（vX.Y.Z）之间才会自动升级；本地编译的开发版不会被自动覆盖，但可以在设置页面手动装上最新正式版。

Release 里每个板子有两个文件：

| 文件 | 用途 |
|---|---|
| `firmware-box3-merged.bin` | 第一次烧录，用网页工具写到 `0x0` |
| `firmware-box3.bin` | 已经装过的设备，在设置页面上传升级（OTA 也下载这个） |

国内访问 GitHub 慢、或者所在网络连不上 GitHub 的下载地址时，可以在自己的服务器（比如家里的 NAS）上跑 [`server/ota-mirror`](../server/ota-mirror/README.md)（飞牛 fnOS 部署见 [docs/ota-server-fnos.md](ota-server-fnos.md)），它会自动同步 Release 里的固件；然后在设置网页「系统 › 固件升级 › 自建升级服务器」填上它的 manifest.json 地址。板子先试自建服务器，连不上再用 GitHub。


## 目录结构


```
firmware/
  platformio.ini       编译配置（box3 / desk_v1 固件，native 单元测试）
  include/board.h      各板子的引脚定义
  include/lv_conf.h    LVGL 配置
  src/main.cpp         启动流程、主循环
  src/pages.cpp        页面名字和「显示哪些页面」设置的读写（有单元测试）
  src/display.cpp      屏幕驱动（LovyanGFX）和 LVGL 对接
  src/ui.cpp           界面
  src/weather.cpp      后台拉取天气
  src/weather_parse.cpp 天气预报解析、下雨判断和提醒（有单元测试）
  src/weather_codes.cpp WMO 天气代码对应的文字和颜色（深色、浅色两套）
  src/web_config.cpp   局域网设置页面
  src/github.cpp       后台拉取 GitHub 贡献日历（GraphQL API）
  src/github_parse.cpp 解析贡献日历、计算连续天数（不依赖硬件，有单元测试）
  src/root_ca.h        访问 GitHub 用的根证书
  src/ota.cpp          在线升级和网页上传升级
  src/ota_util.cpp     版本号比较、manifest 解析、固件格式检查（有单元测试）
  src/stock.cpp        后台拉取自选股行情
  src/stock_parse.cpp  解析股票代码和行情（不依赖硬件，有单元测试）
  src/night.cpp        夜间模式的时间计算（有单元测试）
  src/astro.cpp        月相计算、日出日落时间解析（有单元测试）
  src/screen_ctrl.cpp  亮度渐变、夜间点亮、页面轮播的状态机（不依赖硬件，有按时间线模拟的场景测试）
  src/pomodoro.cpp     番茄钟状态机（有单元测试）
  src/sound_synth.cpp  提示音合成（有单元测试）
  src/audio.cpp        BOX-3 的 ES8311 解码芯片和 I2S 播放
  src/settings.cpp     设置保存（NVS）
  src/screen_load.cpp  切换页面（先收尾没播完的滑动动画，避开 LVGL 8.3 快速连点会重启的问题）
  src/panel_watch.cpp  屏幕控制芯片看门狗，白屏、花屏时重新初始化（有单元测试）
  src/pomo_stats.cpp   番茄钟近 7 天完成数（有单元测试）
  src/reminder.cpp     久坐、喝水提醒的计时（有单元测试）
  src/clock_events.cpp 整点报时和闹钟（有单元测试）
  src/pet.cpp          像素小猫的心情和图案（有单元测试）
  src/sensors.cpp      SENSOR 底座的温湿度传感器和人体感应雷达
  src/climate.cpp      温湿度解析和「有点热」「有点闷」提醒（有单元测试）
  src/net.cpp          网络和内存诊断
  src/imu.cpp          板载加速度计
  src/motion.cpp       认出摇一摇、急刹急转、倒过来（有单元测试）
  src/runner.cpp       小游戏「小橘跑酷」（有单元测试）
  src/voice.cpp        AI 语音聊天：唤醒词、回声消除、WebSocket 连接、播放、打断和挂断
  src/sr_model.cpp     把编进固件的唤醒词模型交给 esp-sr（tools/esp_sr.py 编译前下载库和模型）
  src/music.cpp        音乐律动：频谱、节拍和音乐识别（有单元测试）
  src/listen.cpp       音乐律动的后台任务，和 AI 聊天轮流用麦克风
  src/realtime.cpp     百炼实时语音接口的事件拼装和解析、麦克风自动增益（有单元测试）
  src/ws.cpp           WebSocket 帧的收发拆包（有单元测试）
  src/omni.cpp         base64、重采样等语音聊天的小工具（有单元测试）
  src/dashscope_ca.h   访问百炼用的根证书
  src/photos.cpp       电子相册：读 TF 卡、解码、轮播、屏保
  src/album.cpp        相册里不依赖硬件的部分：文件名、文件类型、轮播顺序（有单元测试）
  src/gif.cpp          GIF 动图解码、缩放到屏幕（有单元测试）
  src/avi.cpp          MJPEG 视频（.avi）文件解析、索引跳转、声音转 16kHz（有单元测试）
  src/avplay.cpp       放视频：画面跟着声音走，相册和影片页共用
  src/movie.cpp        影片页：读 movies 文件夹、播放、暂停、快进、记住看到哪里，网页上传和删除
  src/movie_util.cpp   影片页里不依赖硬件的部分：看到哪里的记录、时间格式、上传的文件名（有单元测试）
  src/player.cpp       音乐页：读 music 文件夹、后台播放、播放顺序、睡眠定时、记住放到哪里，网页上传和删除
  src/song_decoder.cpp 解码 MP3（minimp3）、FLAC（dr_flac）、WAV，跳到某一秒（可在电脑上测试）
  src/track.cpp        音乐里不依赖硬件的部分：歌曲标签和封面、.lrc 歌词、目录缓存、播放顺序、上传的文件名（有单元测试）
  src/text_conv.cpp    GBK、UTF-16、Latin-1 转 UTF-8（老 MP3 标签和歌词常是 GBK，有单元测试）
  src/resample.cpp     放歌时把麦克风和喇叭的声音转回 16kHz，给唤醒词和音乐律动用（有单元测试）
  src/bing.cpp         必应每日一图的下载和存储
  src/bing_parse.cpp   解析必应接口返回（有单元测试）
  src/bing_ca.h        访问 cn.bing.com 用的根证书
  src/news.cpp         资讯：按设置的来源轮流拉新闻（共用联网任务）
  src/news_parse.cpp   解析新浪、百度热搜、知乎热榜、RSS / Atom（有单元测试）
  src/fonts/           中文字体（自动生成，不要手改）
  lib/minimp3/         MP3 解码库（改了一处，见里面的 README）
  lib/dr_flac/         FLAC 解码库
  web/index.html       设置网页（编译时压缩进固件）
  tools/gen_fonts.py   中文字体生成脚本
  tools/merge_bin.py   编译后生成单文件固件
  tools/version.py     编译前从 git 标签生成版本号
  tools/check_image.py 检查编译出的固件格式（CI 用）
  tools/make_manifest.py 发布时生成 OTA 的 manifest.json
  tools/web_gz.py      编译前把 web/index.html 压缩成固件里的数据
  tools/gen_gbk.py     生成 GBK 转 Unicode 的对照表 src/gbk_table.h
  test/                单元测试（pio test -e native，在电脑上运行）
```


## 中文字体


LVGL 自带的中文字体以繁体和日文为主，缺很多简体字，所以用 [Noto Sans SC](https://fonts.google.com/noto/specimen/Noto+Sans+SC)（SIL OFL 协议，可以商用）自己生成：

- `font_cn_16`：GB2312 全部汉字（6763 个）加上界面用到的字，用于普通文字、城市名和股票名
- `font_cn_24`：只含界面用到的字，用于标题和天气描述

**改了界面上的中文后要重新生成字体**，否则新的字会显示成方框：

```bash
cd firmware
npm install lv_font_conv@1.5.2 @expo-google-fonts/noto-sans-sc@0.4.4
python3 tools/gen_fonts.py node_modules/@expo-google-fonts/noto-sans-sc/400Regular/NotoSansSC_400Regular.ttf
```
