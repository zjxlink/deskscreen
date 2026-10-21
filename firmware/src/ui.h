#pragma once

#include "github.h"
#include "music.h"
#include "news.h"
#include "pages.h"
#include "pet.h"
#include "pomodoro.h"
#include "runner.h"
#include "stock.h"
#include "voice.h"
#include "weather.h"

void ui_init();

// 全屏提示页，detail 可以包含换行
void ui_show_status(const char *title, const char *detail);

// 带进度条的全屏提示页（升级固件用），percent 为 0~100
void ui_show_progress(const char *title, const char *detail, int percent);

// 首次配网提示页
void ui_show_provision(const char *ap_name);

// 切换到某个页面，animate 为 true 时有滑动动画：默认向左滑，back 为 true 时向右滑（往回翻）
void ui_show_page(Page page, bool animate, bool back = false);

// 每次主循环调用，秒数变化时才真正刷新
void ui_tick_clock();

void ui_set_city(const char *city);

// 时钟页上的室内温湿度（SENSOR 底座），超过阈值的那一项显示成橙色；没调用过就不显示
void ui_set_room(float temp, float humidity, bool hot, bool humid);

// 时钟页的日出日落（当天第几分钟，-1 表示不知道）。月相按日期自己算，对上时间就显示
void ui_set_sun(int sunrise, int sunset);
void ui_set_weather(const WeatherData &w);
void ui_set_github(const GithubData &g);
void ui_set_stocks(const StockData &s);

// 资讯页：d 要一直有效（主循环里那份，放在 PSRAM），新数据来了再调用一次；style 0 列表，1 一屏一条
void ui_set_news(const NewsData &d, uint8_t style);
// 翻到下一批（自动翻页、往上滑），这个频道看完了换下一个频道；往下滑翻回上一批
void ui_news_next();
void ui_news_prev();
// 换频道（点顶栏的频道名），dir 为 1 或 -1
void ui_news_channel(int dir);
// 触摸点是否落在资讯页顶栏的频道名上
bool ui_news_header_hit(int x, int y);
// 时钟页的要闻条：换下一条标题，没有新闻时返回 false；show 为 true 时显示要闻条，否则显示日出日落
bool ui_ticker_next();
void ui_show_ticker(bool show);

// 番茄钟页：msg 不为空时在顶栏显示提示（如"专注完成，休息一下吧"）；done_today 为今日完成数（来自统计，重启不丢）
void ui_set_pomodoro(const Pomodoro &p, uint32_t now_ms, const char *msg, int done_today);

// 像素小猫页：心情或眨眼变化时重画小猫；line 是小猫说的话，status 是底部的一行小字
void ui_set_pet(PetMood mood, bool blink, const char *line, const char *status);

// 小猫的动画：dx、bob 为左右、上下偏移的像素数，zzz 为睡觉时"Zzz"的帧（0 或 1），不睡觉时传 -1
void ui_pet_anim(int dx, int bob, int zzz);

// 小猫页底部的音乐频谱：bands 是 kMusicBands 根条的高度（0～1），nullptr 表示不放音乐，换回状态那一行
void ui_set_pet_music(const float *bands);

// 全屏提醒（留言、久坐、喝水、闹钟），盖在当前页面上，关掉后用 ui_show_page 回到原来的页面
enum class AlertIcon { Message, Sit, Water, Alarm, Climate, Rain };
void ui_show_alert(AlertIcon icon, const char *title, const char *body, const char *foot);

// 快捷设置页（长按屏幕打开）：亮度、提示音、没人熄屏，以及打开网页设置的二维码。
// 点「显示页面」进入第二屏，选哪些页面参与翻页；页面多了放不下时上下滑动
struct QuickState {
  uint8_t brightness;  // 白天亮度 10~255
  bool sound_on;
  uint8_t volume;      // 0~100，小橘说话、提示音、整点报时都用它
  bool has_dock;       // 插了 SENSOR 底座，「没人熄屏」才有用
  bool away_off;
  const char *url;     // 网页设置地址，空字符串表示还没连上 WiFi
  bool pages_view;     // 显示「显示页面」那一屏
  uint32_t page_hidden;       // 第 i 位：第 i 页被隐藏
  uint32_t page_unavailable;  // 第 i 位：板子上没有这页的硬件，不列出来
  uint32_t page_unset;        // 第 i 位：这页还没设置好（如没填 GitHub Token），格子上提示一下
};
enum class QuickHit { None, Close, BrightDown, BrightUp, VolumeDown, VolumeUp, Sound, Away, Pages, Back, PageTile };

// 显示快捷设置页，或者在已经显示时刷新上面的状态
void ui_show_quick(const QuickState &q);

// 快捷设置页上点到了什么（按钮周围留了一圈余量，好点中）；点到页面格子时 page 为哪一页
QuickHit ui_quick_hit(int x, int y, Page *page = nullptr);

// 「显示页面」那一屏上下滑动：dy 为手指从按下到现在往上移动的像素数，跟着手指滚动；
// 松手时调用 ui_quick_drag_end，对齐到整行。不在那一屏时什么也不做
void ui_quick_drag(int dy);
void ui_quick_drag_end();

// 小橘跑酷：打开游戏画面；每帧用游戏状态刷新画面，best 为最高分
void ui_show_game();
void ui_set_game(const RunnerGame &g, int best);
bool ui_game_close_hit(int x, int y);

// 触摸点是否落在小猫页的小橘身上（点小橘开始跑酷）
bool ui_pet_hit(int x, int y);

// AI 聊天页：小橘（表情跟着它说的话变）、对话气泡（主人在右、小橘在左，小橘的话跟着声音一个字一个字亮起来）、
// 提示和「开始聊天 / 挂断 / 打断」按钮
struct ChatView {
  VoiceState state;
  uint8_t level;       // 麦克风音量 0~100
  uint32_t latency_ms;   // 上一句从说完到小橘出声用了多久
  uint32_t idle_left_ms; // 再过多久没人说话就挂断
  const ChatLog *log;  // 这次聊天说过的话，画成左右两边的气泡
  uint16_t spoken;     // 小橘最后一句已经念到第几个字，后面的字先用灰色显示；kSpokenAll 表示全念完了
  const char *error;
  bool has_key;        // 设置了百炼 API Key
  bool wake;           // 唤醒词开着
  bool full_duplex;    // 小橘说话时开口就能打断
  bool blink;
  int bob;             // 小橘上下跳的像素数
};
void ui_set_chat(const ChatView &v);
bool ui_chat_button_hit(int x, int y);

// 电子相册页：px 为 320x240 的 RGB565 像素（photos_take_frame 给的），nullptr 表示不显示照片；
// msg 是盖在中间的提示（没插卡、没有照片），nullptr 表示不显示
void ui_set_album_photo(const uint16_t *px);
void ui_set_album_msg(const char *msg);

// 照片底部的说明（必应图片的标题和地点），空字符串或 nullptr 表示不显示
void ui_set_album_caption(const char *text);

// 相册页左右边缘的 < > 提示，当屏保时不显示
void ui_set_album_arrows(bool show);

// 触摸点是否落在番茄钟的圆环上（圆环居中，屏幕上下或左右翻转时判断结果也不变）
bool ui_pomodoro_hit(int x, int y);
void ui_set_wifi(bool connected);

// 换成浅色或深色主题，和现在一样时什么都不做。开机时界面是深色
void ui_set_theme(bool light);

// 立即把当前界面画到屏幕上（主循环阻塞前使用）
void ui_refresh_now();
