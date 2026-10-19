# 桌面信息屏

基于 ESP32-S3 的桌面小屏幕，显示时间、日期和天气。目标用户是程序员和科技爱好者，后续会加入 GitHub、股票行情、番茄钟等功能。

![界面预览](docs/preview.png)

![GitHub 热力图](docs/preview-github.png)

![自选股](docs/preview-stocks.png)

![番茄钟](docs/preview-pomodoro.png)

## 当前功能（v0.7）

- 手机配网：首次开机设备会开一个 WiFi 热点，手机连上后选择家里的 WiFi 即可，不用装 App
- 时间日期：NTP 自动对时（北京时间）
- 天气：当前气温、天气、今日最高和最低气温、湿度。数据来自 [Open-Meteo](https://open-meteo.com)，免费，不需要 API Key，每 15 分钟刷新一次
- 网页设置：同一 WiFi 下用浏览器打开 `http://deskscreen.local`（或屏幕上显示的 IP），可以设置城市（支持按城市名查坐标）和屏幕亮度。页面底部的「网络诊断」显示天气、GitHub、自选股、在线升级最近一次请求的结果和失败原因，出问题时不用接串口也能排查
- 重新配网：长按 BOOT 键 3 秒，或者在设置页面里点「重新配置 WiFi」
- GitHub 贡献热力图：最近 24 周的贡献格子（配色和 GitHub 一致），以及今日贡献、连续天数、过去一年总数，每 30 分钟刷新
- 自选股行情：最多 6 只，支持 A 股、港股、美股和 ETF，红涨绿跌。交易时段每分钟刷新，其余时间 10 分钟一次。数据来自东方财富的公开接口（非官方接口，适合个人使用）；东方财富会按 IP 限流，失败后改用腾讯行情，并暂停请求东方财富 5～30 分钟
- 夜间自动调暗：默认 23:00～7:00 把屏幕调到很暗（亮度可调，可以直接关掉背光），亮度渐变不突兀；夜间固定显示时钟页，点一下屏幕或按 BOOT 键临时恢复正常亮度 30 秒。时间段可在设置页面修改或关闭
- 番茄钟：专注 25 分钟、短休息 5 分钟，每 4 个番茄长休息 15 分钟（都可以改）。点屏幕中间的圆环开始/暂停，按住 1.5 秒重置；设置页面也有控制按钮（没有触摸屏的板子用这个）。阶段结束时屏幕自动点亮并跳到番茄钟页提示；专注结束自动开始休息，休息结束等你点开始。进行中时自动轮播停在番茄钟页
- 番茄钟统计：每完成一个番茄自动记录，统计页显示最近 7 天的柱状图、今日完成数（对比每日目标，默认 4 个）、近 7 天总数和连续达标天数。数据存在设备上，重启和升级都不会丢。用过番茄钟之后统计页会加入自动轮播
- 留言板：在设置页面最上方写一句话（最多 60 个字），点「发送到屏幕」，设备立即全屏显示这条留言，可以选择「叮咚」响一声。点屏幕或按 BOOT 键关闭
- 久坐提醒和喝水提醒：在设置页面的「健康提醒」里打开，间隔分别默认 45 分钟和 60 分钟，只在提醒时段（默认 9:00～21:00）内提醒。到点时屏幕全屏提示并响一声，5 分钟没人理会自动关掉。番茄钟专注时先不打扰，等休息时再提醒；开始休息时久坐提醒重新计时
- 番茄钟提示音（BOX-3）：专注结束播放下行三个音，休息结束播放上行三个音；音量可调，默认夜间时段不响。设置页面可以试听
- 在线升级（OTA）：设备每天检查一次 GitHub Release，有新版本时在凌晨 3～5 点自动安装（可关闭）；也可以在设置页面手动检查、立即升级，或者直接上传固件文件。升级时屏幕显示进度，写入后校验 MD5，失败不会影响旧固件
- 多页面：点一下屏幕（BOX-3）或短按 BOOT 键切换页面；也可以在设置页面设定自动轮播间隔（默认 15 秒，只轮播已经设置好的页面）

### 设置 GitHub 热力图

1. 打开 https://github.com/settings/tokens/new?description=DeskScreen 创建一个 classic Token，**不用勾选任何权限**（这样的 Token 只能读公开信息，泄露了也改不了你的任何东西），有效期按需选择
2. 在设备设置页面的「GitHub Token」里粘贴并保存

设备用内置的根证书校验 api.github.com 的 HTTPS 证书，Token 不会被中间人截获。Token 只保存在设备上，设置页面不会显示它。如果想统计私有仓库的贡献，需要在 GitHub 个人主页的贡献图设置里打开「Private contributions」。

### 设置自选股

在设置页面的「自选股」里填代码，用逗号或空格分隔：

| 写法 | 含义 |
|---|---|
| `600519`、`000001`、`510300` | A 股和 ETF，6 位代码，自动判断沪市/深市/北交所 |
| `sh000001` | 加前缀指定市场，例如上证指数（和深市的 000001 平安银行区分） |
| `00700` 或 `hk700` | 港股 |
| `AAPL`、`BRK.B` | 美股，会自动在纳斯达克、纽交所、美交所里查找 |

### 发布新版本（OTA）

两种方式任选一种：

- **网页（手机也行）**：打开仓库的 Actions → Release → Run workflow，填写版本号（如 `v0.5.0`），点运行。会在 `main` 的最新提交上创建标签
- **命令行**：`git tag v0.5.0 && git push origin v0.5.0`

之后 GitHub Actions 会检查版本号、跑单元测试、编译两个板子的固件、生成 `manifest.json` 并创建 Release。设备从 `releases/latest/download/manifest.json` 检查更新，只有正式版本号（vX.Y.Z）之间才会自动升级；本地编译的开发版不会被自动覆盖，但可以在设置页面手动装上最新正式版。

Release 里每个板子有两个文件：

| 文件 | 用途 |
|---|---|
| `firmware-box3-merged.bin` | 第一次烧录，用网页工具写到 `0x0` |
| `firmware-box3.bin` | 已经装过的设备，在设置页面上传升级（OTA 也下载这个） |

国内访问 GitHub 慢的话，可以把 Release 文件同步到自己的服务器，然后修改 `src/ota.cpp` 里的 `OTA_MANIFEST_URL`。

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
  platformio.ini       编译配置（box3 / desk_v1 固件，native 单元测试）
  include/board.h      各板子的引脚定义
  include/lv_conf.h    LVGL 配置
  src/main.cpp         启动流程、主循环
  src/display.cpp      屏幕驱动（LovyanGFX）和 LVGL 对接
  src/ui.cpp           界面
  src/weather.cpp      后台拉取天气
  src/web_config.cpp   局域网设置页面
  src/github.cpp       后台拉取 GitHub 贡献日历（GraphQL API）
  src/github_parse.cpp 解析贡献日历、计算连续天数（不依赖硬件，有单元测试）
  src/root_ca.h        访问 GitHub 用的根证书
  src/ota.cpp          在线升级和网页上传升级
  src/ota_util.cpp     版本号比较、manifest 解析、固件格式检查（有单元测试）
  src/stock.cpp        后台拉取自选股行情
  src/stock_parse.cpp  解析股票代码和行情（不依赖硬件，有单元测试）
  src/night.cpp        夜间模式的时间计算（有单元测试）
  src/screen_ctrl.cpp  亮度渐变、夜间点亮、页面轮播的状态机（不依赖硬件，有按时间线模拟的场景测试）
  src/pomodoro.cpp     番茄钟状态机（有单元测试）
  src/sound_synth.cpp  提示音合成（有单元测试）
  src/audio.cpp        BOX-3 的 ES8311 解码芯片和 I2S 播放
  src/settings.cpp     设置保存（NVS）
  src/fonts/           中文字体（自动生成，不要手改）
  tools/gen_fonts.py   中文字体生成脚本
  tools/merge_bin.py   编译后生成单文件固件
  tools/version.py     编译前从 git 标签生成版本号
  tools/check_image.py 检查编译出的固件格式（CI 用）
  tools/make_manifest.py 发布时生成 OTA 的 manifest.json
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

## 路线图

- [x] v0.1 时间、天气、配网、网页设置
- [x] GitHub 贡献热力图
- [ ] GitHub Star 数、仓库动态
- [x] 自选股行情
- [ ] 场外基金估值
- [x] 多页面切换（触摸或按键）
- [x] 番茄钟
- [x] 番茄钟提示音（BOX-3）
- [ ] 量产板加蜂鸣器
- [ ] 待办事项
- [x] OTA 在线升级
- [x] 夜间自动调暗（按时间；量产板可加光敏电阻按环境光调）
- [ ] 量产板 PCB 和外壳
