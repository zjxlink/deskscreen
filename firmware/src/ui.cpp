#include "ui.h"

#include <Arduino.h>
#include <esp_heap_caps.h>
#include <lvgl.h>
#include <string.h>
#include <time.h>

#include <string>

#include "astro.h"
#include "friends.h"
#include "movie_util.h"
#include "board.h"
#include "screen_load.h"

// 屏幕配色有深色、浅色两套。深色参考 GitHub 深色主题；浅色是偏暖的米白底、白卡片，
// 雨的蓝色调深一点，白底上才看得清。启动时界面先按深色建好，ui_set_theme 再换成浅色
struct Palette {
  uint32_t bg, card, text, subtle, accent, ok, err, flat, line, rain, rain_soft;
  uint32_t level[5];  // GitHub 贡献等级，0 级为空白格
  uint32_t moon_lit, moon_dark;
  uint32_t flat_text;  // 灰色涨跌标签（平盘）上的字
};
static const Palette kDark = {
    0x0D1117, 0x161B22, 0xE6EDF3, 0x8B949E, 0x58A6FF, 0x3FB950, 0xF85149, 0x30363D, 0x21262D, 0x4FC3F7, 0x3B5368,
    {0x161B22, 0x0E4429, 0x006D32, 0x26A641, 0x39D353},
    0xF2E6B8, 0x2D333B,
    0xFFFFFF,
};
// 卡片不用纯白：换主题时按颜色值找控件，和按钮、标签上的白字分开
static const Palette kLight = {
    0xF3F1EC, 0xFAFAF7, 0x1F2328, 0x6E7781, 0x0969DA, 0x2DA44E, 0xE5534B, 0xD8DCE1, 0xE4E2DC, 0x1E88D6, 0xA9CBE6,
    {0xE6E8EB, 0x9BE9A8, 0x40C463, 0x30A14E, 0x216E39},
    0xE0B33A, 0xD3D7DD,
    0x6E7781,
};
static const Palette *pal = &kDark;

static uint32_t weather_color_on(int code) {
  return pal == &kLight ? weather_color_light(code) : weather_color(code);
}

#define COLOR_BG      lv_color_hex(pal->bg)
#define COLOR_CARD    lv_color_hex(pal->card)
#define COLOR_TEXT    lv_color_hex(pal->text)
#define COLOR_SUBTLE  lv_color_hex(pal->subtle)
#define COLOR_ACCENT  lv_color_hex(pal->accent)
#define COLOR_OK      lv_color_hex(pal->ok)
#define COLOR_ERR     lv_color_hex(pal->err)
#define COLOR_RAIN    lv_color_hex(pal->rain)

// 行情涨跌配色，按 A 股习惯红涨绿跌，两套主题一样
#define COLOR_UP      lv_color_hex(0xDA3633)
#define COLOR_DOWN    lv_color_hex(0x238636)
#define COLOR_FLAT    lv_color_hex(pal->flat)
#define COLOR_LINE    lv_color_hex(pal->line)

// 热力图格子尺寸：24 周 x 13px = 309px，正好放进 320 宽的屏幕
static constexpr int kCell = 10;
static constexpr int kGap = 3;

// 中文字体由 tools/gen_fonts.py 生成
LV_FONT_DECLARE(font_cn_16);
LV_FONT_DECLARE(font_cn_24);

static lv_obj_t *scr_status;
static lv_obj_t *status_title;
static lv_obj_t *status_detail;
static lv_obj_t *status_bar;

static lv_obj_t *scr_pages[PAGE_COUNT];
static lv_obj_t *page_arrows[PAGE_COUNT][2];  // 页面左右边缘淡淡的 < >：点两边翻页的提示

static lv_obj_t *scr_main;
static lv_obj_t *lbl_date;
static lv_obj_t *lbl_city;
static std::string city_name;          // 有好友在桌前时右上角显示好友，换回来时用
static bool friends_here_shown = false;
static lv_obj_t *dot_wifi[PAGE_COUNT];
static lv_obj_t *lbl_time;
static lv_obj_t *lbl_sec;
static lv_obj_t *lbl_temp;
static lv_obj_t *lbl_desc;
static lv_obj_t *lbl_range;
static lv_obj_t *lbl_humidity;
static lv_obj_t *lbl_room;
// 时间下面一行：月相图标、月相名字、日出日落
static constexpr int kMoonPx = 16;
static constexpr int kTimeY = 38;
static constexpr int kRoomY = 94;
static constexpr int kSunY = 114;        // 有室内温湿度时在它下面
static constexpr int kSunAloneY = 100;   // 没有 SENSOR 底座时往上挪
static lv_obj_t *sun_row;
static lv_obj_t *moon_canvas;
static lv_color_t moon_buf[kMoonPx * kMoonPx];
static lv_obj_t *lbl_sun;
static float moon_now = -1;          // 今天的月相，还没对时为 -1
static int sun_rise = -1, sun_set = -1;

static lv_obj_t *lbl_gh_login;
static lv_obj_t *lbl_gh_today;
static lv_obj_t *lbl_gh_streak;
static lv_obj_t *lbl_gh_total;
static lv_obj_t *gh_grid;
static lv_obj_t *gh_cells[kGithubWeeks][7];
static lv_obj_t *lbl_gh_msg;
static lv_obj_t *gh_legend[5];  // 图例「少 ■■■■■ 多」

struct StockRow {
  lv_obj_t *row;
  lv_obj_t *name;
  lv_obj_t *price;
  lv_obj_t *pill;
  lv_obj_t *pct;
};
static StockRow stock_rows[kMaxStocks];
static lv_obj_t *lbl_stock_updated;
static lv_obj_t *lbl_stock_msg;

// 番茄钟：圆环在屏幕正中，半径 88
static constexpr int kPomoRing = 176;
static constexpr int kPomoMaxDots = 8;
static lv_obj_t *pomo_arc;
static lv_obj_t *lbl_pomo_time;
static lv_obj_t *lbl_pomo_state;
static lv_obj_t *lbl_pomo_today;
static lv_obj_t *lbl_pomo_msg;
static lv_obj_t *pomo_dots[kPomoMaxDots];

// 像素小猫：16x16 的像素画放大 7 倍画在 canvas 上，画布缓冲区放在 PSRAM（lv_mem_alloc）
static constexpr int kPetScale = 7;
static constexpr int kPetPx = kPetSize * kPetScale;
static constexpr int kPetTop = 34;
// 0 号是背景色，画的时候换成当前主题的背景
static const uint32_t kPetPalette[] = {0x0D1117, 0x5C3317, 0xF0A04B, 0xFFF4E6, 0xFF8FA3, 0x58A6FF};
static lv_color_t pet_color(int i) { return lv_color_hex(i == 0 ? pal->bg : kPetPalette[i]); }
// 换主题时加一，画小橘的地方据此知道要重画
static uint8_t theme_epoch;
static lv_obj_t *pet_canvas;
static lv_color_t *pet_buf;
static lv_obj_t *lbl_pet_line;
static lv_obj_t *lbl_pet_status;
static lv_obj_t *lbl_pet_zzz;
static lv_obj_t *pet_bars[kMusicBands];  // 音乐律动的频谱，放音乐时代替底部那行状态
static constexpr int kPetBarW = 14;
static constexpr int kPetBarGap = 4;
static constexpr int kPetBarMaxH = 40;
static constexpr int kPetBarBase = SCREEN_H - 10;  // 频谱条底边

// 全屏提醒
static lv_obj_t *scr_alert;
static lv_obj_t *alert_badge;
static lv_obj_t *alert_icon;
static lv_obj_t *alert_title;
static lv_obj_t *alert_body;
static lv_obj_t *alert_foot;

// 快捷设置页
static lv_obj_t *scr_quick;
static lv_obj_t *quick_close;
static lv_obj_t *quick_minus;
static lv_obj_t *quick_plus;
static lv_obj_t *lbl_quick_bright;
static lv_obj_t *quick_vol_minus;
static lv_obj_t *quick_vol_plus;
static lv_obj_t *lbl_quick_volume;
static lv_obj_t *quick_sound;
static lv_obj_t *quick_away;
static lv_obj_t *lbl_quick_away;
static lv_obj_t *quick_qr;
static lv_obj_t *lbl_quick_url;
static String quick_url;
static lv_obj_t *quick_pages;  // 「显示页面 ›」按钮
static lv_obj_t *quick_title;
static lv_obj_t *quick_music[3];  // 有当前的歌时代替标题：上一首、播放/暂停、下一首

// 快捷设置的第二屏：选哪些页面参与翻页，3 列格子，放不下时上下滑动
static constexpr int kTileW = 98, kTileH = 56, kTileGapX = 6, kTileGapY = 6, kTilesTop = 48;
static lv_obj_t *scr_qpages;
static lv_obj_t *qp_back;
static lv_obj_t *lbl_qp_count;
static lv_obj_t *qp_list;
struct PageTile {
  lv_obj_t *box, *name, *hint, *check, *mark;
};
static PageTile qp_tiles[PAGE_COUNT];
static int qp_drag_base = -1;  // 开始拖动时的滚动位置，-1 表示没在拖

// 小橘跑酷
static lv_obj_t *scr_game;
static lv_obj_t *game_close;
static lv_obj_t *game_cat;
static lv_color_t *game_cat_buf;
static lv_obj_t *game_obs[RunnerGame::kMaxObstacles];
static constexpr int kPebbles = 8;
static lv_obj_t *game_pebbles[kPebbles];
static lv_obj_t *lbl_game_score;
static lv_obj_t *lbl_game_best;
static lv_obj_t *lbl_game_msg;

// AI 聊天页
static constexpr int kChatCat = 48;  // 小橘放大 3 倍
static lv_obj_t *chat_cat;
static lv_color_t *chat_cat_buf;
static lv_obj_t *chat_area;  // 气泡的显示区域，放不下时最早的从上面移出去
static constexpr int kChatBubbles = kChatLines + 1;  // 再多一个给「嗯……」之类的提示
static lv_obj_t *chat_bubbles[kChatBubbles];
static lv_obj_t *chat_texts[kChatBubbles];
static lv_obj_t *lbl_chat_hint;
static lv_obj_t *chat_button;
static lv_obj_t *lbl_chat_button;
static lv_obj_t *chat_level;

// 天气预报页：未来 12 小时降水柱状图 + 未来 3 天
static constexpr int kRainBarW = 18;
static constexpr int kRainBarGap = 6;
static constexpr int kRainBarMaxH = 34;
static constexpr int kRainBase = 30 + kRainBarMaxH;  // 柱子底边在卡片里的 y
static lv_obj_t *lbl_fc_city;
static lv_obj_t *lbl_rain_title;
static lv_obj_t *rain_bars[kRainHours];
static lv_obj_t *lbl_rain_hours[kRainHours / 3];
static lv_obj_t *lbl_fc_name[kForecastDays - 1];
static lv_obj_t *lbl_fc_desc[kForecastDays - 1];
static lv_obj_t *lbl_fc_range[kForecastDays - 1];
static lv_obj_t *lbl_fc_rain[kForecastDays - 1];

// 电子相册：一张铺满屏幕的图，像素在 PSRAM 里（photos.cpp 解码好的帧）；没有照片时显示提示
static lv_obj_t *album_img;
static lv_img_dsc_t album_dsc;
static lv_obj_t *lbl_album_msg;
static lv_obj_t *lbl_album_caption;

static lv_obj_t *make_screen() {
  lv_obj_t *scr = lv_obj_create(nullptr);
  lv_obj_set_style_bg_color(scr, COLOR_BG, 0);
  lv_obj_set_style_text_color(scr, COLOR_TEXT, 0);
  lv_obj_set_style_text_font(scr, &font_cn_16, 0);
  lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
  return scr;
}

static lv_obj_t *make_label(lv_obj_t *parent, const lv_font_t *font, lv_color_t color) {
  lv_obj_t *l = lv_label_create(parent);
  lv_obj_set_style_text_font(l, font, 0);
  lv_obj_set_style_text_color(l, color, 0);
  lv_label_set_text(l, "");
  return l;
}

static lv_obj_t *make_wifi_dot(lv_obj_t *parent) {
  lv_obj_t *dot = lv_obj_create(parent);
  lv_obj_set_size(dot, 8, 8);
  lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_border_width(dot, 0, 0);
  lv_obj_set_style_bg_color(dot, COLOR_ERR, 0);
  lv_obj_align(dot, LV_ALIGN_TOP_RIGHT, -12, 14);
  return dot;
}

static lv_obj_t *make_cell(lv_obj_t *parent, int size, uint8_t level) {
  lv_obj_t *c = lv_obj_create(parent);
  lv_obj_set_size(c, size, size);
  lv_obj_set_style_radius(c, 2, 0);
  lv_obj_set_style_border_width(c, 0, 0);
  lv_obj_set_style_pad_all(c, 0, 0);
  lv_obj_set_style_bg_color(c, lv_color_hex(pal->level[level]), 0);
  lv_obj_clear_flag(c, LV_OBJ_FLAG_SCROLLABLE);
  return c;
}

static void build_status() {
  scr_status = make_screen();
  status_title = make_label(scr_status, &font_cn_24, COLOR_ACCENT);
  lv_obj_align(status_title, LV_ALIGN_TOP_MID, 0, 40);

  status_detail = make_label(scr_status, &font_cn_16, COLOR_TEXT);
  lv_obj_set_width(status_detail, SCREEN_W - 32);
  lv_label_set_long_mode(status_detail, LV_LABEL_LONG_WRAP);
  lv_obj_set_style_text_align(status_detail, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_style_text_line_space(status_detail, 6, 0);
  lv_label_set_recolor(status_detail, true);  // 支持 "#RRGGBB 文字#" 高亮
  lv_obj_align(status_detail, LV_ALIGN_TOP_MID, 0, 80);

  status_bar = lv_bar_create(scr_status);
  lv_obj_set_size(status_bar, SCREEN_W - 64, 10);
  lv_obj_align(status_bar, LV_ALIGN_BOTTOM_MID, 0, -48);
  lv_obj_set_style_bg_color(status_bar, COLOR_CARD, LV_PART_MAIN);
  lv_obj_set_style_bg_opa(status_bar, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_bg_color(status_bar, COLOR_ACCENT, LV_PART_INDICATOR);
  lv_bar_set_range(status_bar, 0, 100);
  lv_obj_add_flag(status_bar, LV_OBJ_FLAG_HIDDEN);
}

static void build_main() {
  scr_main = make_screen();

  // 顶栏：日期 · 城市 · WiFi 状态
  lbl_date = make_label(scr_main, &font_cn_16, COLOR_SUBTLE);
  lv_obj_align(lbl_date, LV_ALIGN_TOP_LEFT, 12, 10);

  dot_wifi[PAGE_CLOCK] = make_wifi_dot(scr_main);

  lbl_city = make_label(scr_main, &font_cn_16, COLOR_SUBTLE);
  lv_label_set_recolor(lbl_city, true);  // 好友在桌前时前面有个绿点
  lv_obj_align_to(lbl_city, dot_wifi[PAGE_CLOCK], LV_ALIGN_OUT_LEFT_MID, -8, 0);

  // 中间：大号时间
  lbl_time = make_label(scr_main, &lv_font_montserrat_48, COLOR_TEXT);
  lv_label_set_text(lbl_time, "--:--");
  lv_obj_align(lbl_time, LV_ALIGN_TOP_MID, -14, kTimeY);

  lbl_sec = make_label(scr_main, &lv_font_montserrat_20, COLOR_SUBTLE);
  lv_label_set_text(lbl_sec, "--");
  lv_obj_align_to(lbl_sec, lbl_time, LV_ALIGN_OUT_RIGHT_BOTTOM, 4, -6);

  // 时间下面：室内温湿度，插了 SENSOR 底座才显示
  lbl_room = make_label(scr_main, &font_cn_16, COLOR_SUBTLE);
  lv_label_set_recolor(lbl_room, true);
  lv_obj_align(lbl_room, LV_ALIGN_TOP_MID, 0, kRoomY);
  lv_obj_add_flag(lbl_room, LV_OBJ_FLAG_HIDDEN);

  // 再下面：月相和日出日落，对上时间后才显示
  sun_row = lv_obj_create(scr_main);
  lv_obj_remove_style_all(sun_row);
  lv_obj_set_size(sun_row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(sun_row, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(sun_row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(sun_row, 6, 0);
  lv_obj_clear_flag(sun_row, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
  lv_obj_align(sun_row, LV_ALIGN_TOP_MID, 0, kSunAloneY);
  moon_canvas = lv_canvas_create(sun_row);
  lv_canvas_set_buffer(moon_canvas, moon_buf, kMoonPx, kMoonPx, LV_IMG_CF_TRUE_COLOR);
  lbl_sun = make_label(sun_row, &font_cn_16, COLOR_SUBTLE);
  lv_obj_add_flag(sun_row, LV_OBJ_FLAG_HIDDEN);

  // 底部：天气卡片
  lv_obj_t *card = lv_obj_create(scr_main);
  lv_obj_set_size(card, SCREEN_W - 24, 88);
  lv_obj_align(card, LV_ALIGN_BOTTOM_MID, 0, -12);
  lv_obj_set_style_bg_color(card, COLOR_CARD, 0);
  lv_obj_set_style_border_width(card, 0, 0);
  lv_obj_set_style_radius(card, 12, 0);
  lv_obj_set_style_pad_all(card, 12, 0);
  lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

  lbl_temp = make_label(card, &lv_font_montserrat_48, COLOR_TEXT);
  lv_label_set_text(lbl_temp, "--\xC2\xB0");  // "--°"
  lv_obj_align(lbl_temp, LV_ALIGN_LEFT_MID, 4, 0);

  lbl_desc = make_label(card, &font_cn_24, COLOR_SUBTLE);
  lv_label_set_text(lbl_desc, "获取中…");
  lv_obj_align(lbl_desc, LV_ALIGN_TOP_LEFT, 140, -4);

  lbl_range = make_label(card, &lv_font_montserrat_20, COLOR_TEXT);
  lv_obj_align(lbl_range, LV_ALIGN_LEFT_MID, 140, 2);

  lbl_humidity = make_label(card, &font_cn_16, COLOR_SUBTLE);
  lv_label_set_recolor(lbl_humidity, true);  // 有雨时用蓝色
  lv_obj_align(lbl_humidity, LV_ALIGN_BOTTOM_LEFT, 140, 0);
}

static lv_obj_t *make_card(lv_obj_t *parent, int x, int y, int w, int h) {
  lv_obj_t *card = lv_obj_create(parent);
  lv_obj_set_pos(card, x, y);
  lv_obj_set_size(card, w, h);
  lv_obj_set_style_bg_color(card, COLOR_CARD, 0);
  lv_obj_set_style_border_width(card, 0, 0);
  lv_obj_set_style_radius(card, 12, 0);
  lv_obj_set_style_pad_all(card, 0, 0);
  lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
  return card;
}

static void build_weather() {
  lv_obj_t *scr = make_screen();
  scr_pages[PAGE_WEATHER] = scr;

  lbl_fc_city = make_label(scr, &font_cn_16, COLOR_SUBTLE);
  lv_label_set_text(lbl_fc_city, "天气预报");
  lv_obj_align(lbl_fc_city, LV_ALIGN_TOP_LEFT, 12, 10);
  dot_wifi[PAGE_WEATHER] = make_wifi_dot(scr);

  // 上面：一句话 + 12 根柱子，柱高是降水概率，会下雨的小时是蓝色
  lv_obj_t *rain = make_card(scr, 12, 34, SCREEN_W - 24, 88);
  lbl_rain_title = make_label(rain, &font_cn_16, COLOR_TEXT);
  lv_obj_set_width(lbl_rain_title, SCREEN_W - 48);
  lv_label_set_long_mode(lbl_rain_title, LV_LABEL_LONG_DOT);
  lv_obj_set_pos(lbl_rain_title, 12, 8);
  lv_label_set_text(lbl_rain_title, "获取中…");
  int chart_w = kRainHours * (kRainBarW + kRainBarGap) - kRainBarGap;
  int x0 = (SCREEN_W - 24 - chart_w) / 2;
  for (int i = 0; i < kRainHours; i++) {
    lv_obj_t *b = lv_obj_create(rain);
    lv_obj_set_style_border_width(b, 0, 0);
    lv_obj_set_style_radius(b, 3, 0);
    lv_obj_set_style_pad_all(b, 0, 0);
    lv_obj_set_style_bg_color(b, COLOR_FLAT, 0);
    lv_obj_set_size(b, kRainBarW, 2);
    lv_obj_set_pos(b, x0 + i * (kRainBarW + kRainBarGap), kRainBase - 2);
    lv_obj_clear_flag(b, LV_OBJ_FLAG_SCROLLABLE);
    rain_bars[i] = b;
    if (i % 3 == 0) {
      lv_obj_t *l = make_label(rain, &lv_font_montserrat_14, COLOR_SUBTLE);
      lv_obj_set_width(l, kRainBarW + 12);
      lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
      lv_obj_set_pos(l, x0 + i * (kRainBarW + kRainBarGap) - 6, kRainBase + 2);
      lbl_rain_hours[i / 3] = l;
    }
  }

  // 下面：明天、后天、大后天三张小卡片
  int gap = 8, w = (SCREEN_W - 24 - 2 * gap) / 3;
  for (int i = 0; i < kForecastDays - 1; i++) {
    lv_obj_t *c = make_card(scr, 12 + i * (w + gap), 130, w, 102);
    lbl_fc_name[i] = make_label(c, &font_cn_16, COLOR_SUBTLE);
    lv_obj_align(lbl_fc_name[i], LV_ALIGN_TOP_MID, 0, 6);
    lbl_fc_desc[i] = make_label(c, &font_cn_24, COLOR_TEXT);
    lv_obj_align(lbl_fc_desc[i], LV_ALIGN_TOP_MID, 0, 27);
    lbl_fc_range[i] = make_label(c, &lv_font_montserrat_14, COLOR_TEXT);
    lv_obj_align(lbl_fc_range[i], LV_ALIGN_TOP_MID, 0, 59);
    lbl_fc_rain[i] = make_label(c, &font_cn_16, COLOR_SUBTLE);
    lv_obj_align(lbl_fc_rain[i], LV_ALIGN_TOP_MID, 0, 77);
  }
}

// 统计数字 + 下方的小标签，三列均分
static lv_obj_t *make_stat(lv_obj_t *parent, int col, const char *caption) {
  int x = (SCREEN_W / 3) * col + SCREEN_W / 6;
  lv_obj_t *num = make_label(parent, &lv_font_montserrat_20, COLOR_TEXT);
  lv_label_set_text(num, "-");
  lv_obj_set_width(num, SCREEN_W / 3);
  lv_obj_set_style_text_align(num, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_pos(num, x - SCREEN_W / 6, 40);

  lv_obj_t *cap = make_label(parent, &font_cn_16, COLOR_SUBTLE);
  lv_label_set_text(cap, caption);
  lv_obj_set_width(cap, SCREEN_W / 3);
  lv_obj_set_style_text_align(cap, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_pos(cap, x - SCREEN_W / 6, 66);
  return num;
}

// ---------- 资讯页 ----------
// 两种样式：列表（一屏 4 条，标题 + 来源和时间）和一屏一条（标题、摘要）。
// 自动每隔一会儿翻到下一批，一个频道翻完换下一个频道；上下滑动直接换频道
static constexpr int kNewsRows = 4;
static constexpr int kNewsRowH = 49;
static constexpr int kNewsTop = 38;
static constexpr uint32_t kNewsOrange = 0xF28C3C;  // 前三名的序号、要闻条的标签，两套主题都用它
static const NewsData *news;  // 主循环里的那份（PSRAM），ui_set_news 时更新
static uint8_t news_style;
static int news_ch, news_pos;  // 当前频道、这一屏第一条的下标
static lv_obj_t *lbl_news_title;
static lv_obj_t *lbl_news_page;
static lv_obj_t *lbl_news_msg;
struct NewsRow {
  lv_obj_t *row;
  lv_obj_t *num;
  lv_obj_t *title;
  lv_obj_t *meta;
};
static NewsRow news_rows[kNewsRows];
static lv_obj_t *news_card;
static lv_obj_t *lbl_nc_title;
static lv_obj_t *nc_bar;
static lv_obj_t *lbl_nc_summary;
static lv_obj_t *lbl_nc_meta;

// 时钟页的要闻条，和日出日落那一行轮流显示
static lv_obj_t *ticker_row;
static lv_obj_t *lbl_ticker;
static bool ticker_shown = false;
static int ticker_ch = 0, ticker_idx = 0;
static void update_sun_row();
static bool music_bar_shown = false;

// 新闻里偶尔有字库里没有的字（生僻字、繁体、表情），直接去掉，免得显示成方块
static void news_fit_font(const char *in, char *out, size_t size) {
  size_t o = 0;
  const uint8_t *p = (const uint8_t *)in;
  while (*p && o + 1 < size) {
    int len = *p < 0x80 ? 1 : (*p >> 5) == 6 ? 2 : (*p >> 4) == 14 ? 3 : (*p >> 3) == 30 ? 4 : 0;
    if (len == 0) {
      p++;
      continue;
    }
    uint32_t cp = len == 1 ? *p : (*p & (0x3F >> (len - 1)));
    bool ok = true;
    for (int i = 1; i < len; i++) {
      if ((p[i] & 0xC0) != 0x80) ok = false;
      else cp = (cp << 6) | (p[i] & 0x3F);
    }
    if (!ok) {
      p++;
      continue;
    }
    lv_font_glyph_dsc_t g;
    if ((cp == ' ' || lv_font_get_glyph_dsc(&font_cn_16, &g, cp, 0)) && o + len < size) {
      memcpy(out + o, p, len);
      o += len;
    }
    p += len;
  }
  out[o] = 0;
}

static void set_label_fit(lv_obj_t *l, const char *text) {
  char buf[256];
  news_fit_font(text, buf, sizeof(buf));
  lv_label_set_text(l, buf);
}

static void build_news() {
  lv_obj_t *scr = make_screen();
  scr_pages[PAGE_NEWS] = scr;

  lbl_news_title = make_label(scr, &font_cn_16, COLOR_SUBTLE);
  lv_label_set_text(lbl_news_title, "资讯");
  lv_obj_align(lbl_news_title, LV_ALIGN_TOP_LEFT, 12, 10);
  dot_wifi[PAGE_NEWS] = make_wifi_dot(scr);
  lbl_news_page = make_label(scr, &font_cn_16, COLOR_SUBTLE);
  lv_obj_align_to(lbl_news_page, dot_wifi[PAGE_NEWS], LV_ALIGN_OUT_LEFT_MID, -8, 0);

  // 列表样式
  for (int i = 0; i < kNewsRows; i++) {
    NewsRow &r = news_rows[i];
    r.row = lv_obj_create(scr);
    lv_obj_remove_style_all(r.row);
    lv_obj_set_size(r.row, SCREEN_W - 24, kNewsRowH);
    lv_obj_set_pos(r.row, 12, kNewsTop + i * kNewsRowH);
    lv_obj_clear_flag(r.row, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    if (i < kNewsRows - 1) {
      lv_obj_set_style_border_side(r.row, LV_BORDER_SIDE_BOTTOM, 0);
      lv_obj_set_style_border_width(r.row, 1, 0);
      lv_obj_set_style_border_color(r.row, COLOR_LINE, 0);
    }
    r.num = make_label(r.row, &lv_font_montserrat_20, COLOR_SUBTLE);
    lv_obj_set_width(r.num, 24);
    lv_obj_align(r.num, LV_ALIGN_TOP_LEFT, 0, 4);
    r.title = make_label(r.row, &font_cn_16, COLOR_TEXT);
    lv_obj_set_width(r.title, SCREEN_W - 24 - 26);
    lv_label_set_long_mode(r.title, LV_LABEL_LONG_DOT);
    lv_obj_align(r.title, LV_ALIGN_TOP_LEFT, 26, 4);
    r.meta = make_label(r.row, &font_cn_16, COLOR_SUBTLE);
    lv_obj_set_width(r.meta, SCREEN_W - 24 - 26);
    lv_label_set_long_mode(r.meta, LV_LABEL_LONG_DOT);
    lv_obj_set_height(r.meta, 20);
    lv_obj_align(r.meta, LV_ALIGN_TOP_LEFT, 26, 25);
    lv_obj_add_flag(r.row, LV_OBJ_FLAG_HIDDEN);
  }

  // 一屏一条：卡片里从上往下是标题、一小段橙色横线、摘要，最下面是来源和时间
  news_card = make_card(scr, 12, kNewsTop, SCREEN_W - 24, SCREEN_H - kNewsTop - 12);
  lv_obj_set_style_pad_all(news_card, 12, 0);
  lv_obj_set_style_pad_row(news_card, 8, 0);
  lv_obj_set_flex_flow(news_card, LV_FLEX_FLOW_COLUMN);
  lbl_nc_title = make_label(news_card, &font_cn_16, COLOR_TEXT);
  lv_obj_set_width(lbl_nc_title, lv_pct(100));
  lv_obj_set_style_text_line_space(lbl_nc_title, 4, 0);
  lv_label_set_long_mode(lbl_nc_title, LV_LABEL_LONG_DOT);  // 最多三行，高度在 render_news 里按字数算
  nc_bar = lv_obj_create(news_card);
  lv_obj_remove_style_all(nc_bar);
  lv_obj_set_size(nc_bar, 32, 3);
  lv_obj_set_style_bg_opa(nc_bar, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(nc_bar, lv_color_hex(kNewsOrange), 0);
  lbl_nc_summary = make_label(news_card, &font_cn_16, COLOR_SUBTLE);
  lv_obj_set_width(lbl_nc_summary, lv_pct(100));
  lv_obj_set_style_text_line_space(lbl_nc_summary, 3, 0);
  lv_label_set_long_mode(lbl_nc_summary, LV_LABEL_LONG_DOT);
  lv_obj_set_flex_grow(lbl_nc_summary, 1);
  lbl_nc_meta = make_label(news_card, &font_cn_16, COLOR_SUBTLE);
  lv_obj_set_width(lbl_nc_meta, lv_pct(100));
  lv_label_set_long_mode(lbl_nc_meta, LV_LABEL_LONG_DOT);
  lv_obj_set_height(lbl_nc_meta, 20);
  lv_obj_add_flag(news_card, LV_OBJ_FLAG_HIDDEN);

  lbl_news_msg = make_label(scr, &font_cn_16, COLOR_SUBTLE);
  lv_obj_set_width(lbl_news_msg, SCREEN_W - 32);
  lv_obj_set_style_text_align(lbl_news_msg, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(lbl_news_msg, LV_ALIGN_CENTER, 0, 0);
  lv_label_set_text(lbl_news_msg, "获取中…");
}

// 「新浪科技 · 12 分钟前」；热搜榜没有时间，返回 false（列表样式里标题就占两行）
static bool news_meta(const NewsItem &it, char *out, size_t size) {
  char age[24];
  time_t now = time(nullptr);
  news_age_text(now > 1700000000 ? it.ts : 0, now, age, sizeof(age));
  if (!age[0]) {
    snprintf(out, size, "%s", it.source);
    return false;
  }
  snprintf(out, size, it.source[0] ? "%s · %s" : "%s%s", it.source, age);
  return true;
}

static int news_per_page() { return news_style == 1 ? 1 : kNewsRows; }

static void render_news() {
  const NewsChannel *ch = news && news->nch ? &news->ch[news_ch % news->nch] : nullptr;
  bool has = ch && ch->count > 0;
  lv_label_set_text_fmt(lbl_news_title, ch ? "资讯 · %s" : "资讯", ch ? ch->name : "");
  for (NewsRow &r : news_rows) lv_obj_add_flag(r.row, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(news_card, LV_OBJ_FLAG_HIDDEN);
  if (!has) {
    lv_label_set_text(lbl_news_page, "");
    if (!ch) lv_label_set_text(lbl_news_msg, "在网页设置的「服务 › 资讯」里\n选择想看的新闻");
    else if (ch->error) lv_label_set_text_fmt(lbl_news_msg, "获取失败（%d），稍后重试", ch->error);
    else lv_label_set_text(lbl_news_msg, "获取中…");
    lv_obj_clear_flag(lbl_news_msg, LV_OBJ_FLAG_HIDDEN);
    return;
  }
  lv_obj_add_flag(lbl_news_msg, LV_OBJ_FLAG_HIDDEN);
  int per = news_per_page();
  if (news_pos >= ch->count) news_pos = 0;
  int pages = (ch->count + per - 1) / per;
  lv_label_set_text_fmt(lbl_news_page, "%d/%d", news_pos / per + 1, pages);
  lv_obj_align_to(lbl_news_page, dot_wifi[PAGE_NEWS], LV_ALIGN_OUT_LEFT_MID, -8, 0);
  char meta[64];
  if (news_style == 1) {
    const NewsItem &it = ch->items[news_pos];
    // 省略号要有固定的高度才知道在哪里截断：算出标题占几行，超过三行按三行。
    // 要用原文算（设置后 LVGL 会把放不下的部分换成省略号）
    char title[256];
    news_fit_font(it.title, title, sizeof(title));
    lv_point_t sz;
    const lv_font_t *f = &font_cn_16;
    lv_txt_get_size(&sz, title, f, 0, 4, SCREEN_W - 48, LV_TEXT_FLAG_NONE);
    lv_coord_t max_h = 3 * lv_font_get_line_height(f) + 2 * 4;
    lv_obj_set_height(lbl_nc_title, sz.y < max_h ? sz.y : max_h);
    lv_label_set_text(lbl_nc_title, title);
    bool has_sum = it.summary[0] && strcmp(it.summary, it.title) != 0;
    set_label_fit(lbl_nc_summary, has_sum ? it.summary : "");
    if (has_sum) {
      lv_obj_clear_flag(nc_bar, LV_OBJ_FLAG_HIDDEN);
    } else {
      lv_obj_add_flag(nc_bar, LV_OBJ_FLAG_HIDDEN);
    }
    news_meta(it, meta, sizeof(meta));
    set_label_fit(lbl_nc_meta, meta);
    lv_obj_clear_flag(news_card, LV_OBJ_FLAG_HIDDEN);
    return;
  }
  for (int i = 0; i < kNewsRows; i++) {
    int k = news_pos + i;
    if (k >= ch->count) break;
    NewsRow &r = news_rows[i];
    const NewsItem &it = ch->items[k];
    lv_label_set_text_fmt(r.num, "%d", k + 1);
    lv_obj_set_style_text_color(r.num, k < 3 ? lv_color_hex(kNewsOrange) : COLOR_SUBTLE, 0);
    set_label_fit(r.title, it.title);
    bool timed = news_meta(it, meta, sizeof(meta));
    if (timed) {
      lv_obj_set_height(r.title, 20);
      set_label_fit(r.meta, meta);
      lv_obj_clear_flag(r.meta, LV_OBJ_FLAG_HIDDEN);
    } else {
      lv_obj_set_height(r.title, 42);  // 没有时间就不要第二行，标题能多显示一行
      lv_obj_add_flag(r.meta, LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_clear_flag(r.row, LV_OBJ_FLAG_HIDDEN);
  }
}

static int news_channel_count() { return news ? news->nch : 0; }

void ui_set_news(const NewsData &d, uint8_t style) {
  news = &d;
  if (style != news_style) news_pos = 0;
  news_style = style;
  if (news_ch >= news_channel_count()) news_ch = 0;
  render_news();
}

void ui_news_next() {
  const NewsChannel *ch = news && news->nch ? &news->ch[news_ch % news->nch] : nullptr;
  if (!ch) return;
  news_pos += news_per_page();
  if (news_pos >= ch->count) {
    // 这个频道看完了，换下一个有新闻的频道
    news_pos = 0;
    for (int k = 1; k <= news->nch; k++) {
      int c = (news_ch + k) % news->nch;
      if (news->ch[c].count > 0) {
        news_ch = c;
        break;
      }
    }
  }
  render_news();
}

void ui_news_prev() {
  const NewsChannel *ch = news && news->nch ? &news->ch[news_ch % news->nch] : nullptr;
  if (!ch) return;
  int per = news_per_page();
  if (news_pos >= per) {
    news_pos -= per;
  } else {
    // 已经是这个频道的第一批：回到上一个有新闻的频道的最后一批
    for (int k = 1; k <= news->nch; k++) {
      int c = (news_ch - k + news->nch) % news->nch;
      if (news->ch[c].count > 0) {
        news_ch = c;
        news_pos = (news->ch[c].count - 1) / per * per;
        break;
      }
    }
  }
  render_news();
}

bool ui_news_header_hit(int x, int y) {
  // 顶栏的「资讯 · 科技」，往下和往右都多留一点，好点中
  return y < 36 && x < lv_obj_get_x(lbl_news_title) + lv_obj_get_width(lbl_news_title) + 24;
}

void ui_news_channel(int dir) {
  int n = news_channel_count();
  if (n == 0) return;
  news_ch = ((news_ch + dir) % n + n) % n;
  news_pos = 0;
  render_news();
}

static void build_ticker() {
  ticker_row = lv_obj_create(scr_main);
  lv_obj_remove_style_all(ticker_row);
  lv_obj_set_size(ticker_row, SCREEN_W - 24, 22);
  lv_obj_align(ticker_row, LV_ALIGN_TOP_MID, 0, kSunAloneY - 2);
  lv_obj_clear_flag(ticker_row, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
  lv_obj_t *pill = lv_obj_create(ticker_row);
  lv_obj_set_size(pill, 40, 20);
  lv_obj_set_style_radius(pill, 4, 0);
  lv_obj_set_style_border_width(pill, 0, 0);
  lv_obj_set_style_pad_all(pill, 0, 0);
  lv_obj_set_style_bg_color(pill, lv_color_hex(kNewsOrange), 0);
  lv_obj_clear_flag(pill, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
  lv_obj_align(pill, LV_ALIGN_LEFT_MID, 0, 0);
  lv_obj_t *tag = make_label(pill, &font_cn_16, lv_color_hex(0xFFFFFF));
  lv_label_set_text(tag, "要闻");
  lv_obj_center(tag);
  lbl_ticker = make_label(ticker_row, &font_cn_16, COLOR_TEXT);
  lv_obj_set_size(lbl_ticker, SCREEN_W - 24 - 48, 20);
  lv_label_set_long_mode(lbl_ticker, LV_LABEL_LONG_DOT);
  lv_obj_align(lbl_ticker, LV_ALIGN_LEFT_MID, 48, 0);
  lv_obj_add_flag(ticker_row, LV_OBJ_FLAG_HIDDEN);
}

bool ui_ticker_next() {
  if (!news || news->nch == 0) return false;
  // 各频道轮流出一条：第 1 轮是各频道的第 1 条，第 2 轮是第 2 条……
  for (int tries = 0; tries < news->nch * 3; tries++) {
    ticker_ch = (ticker_ch + 1) % news->nch;
    if (ticker_ch == 0) ticker_idx = (ticker_idx + 1) % 3;
    const NewsChannel &ch = news->ch[ticker_ch];
    int k = ch.count > ticker_idx ? ticker_idx : 0;
    if (ch.count > 0) {
      set_label_fit(lbl_ticker, ch.items[k].title);
      return true;
    }
  }
  return false;
}

void ui_show_ticker(bool show) {
  if (show == ticker_shown) return;
  ticker_shown = show;
  if (music_bar_shown) return;  // 迷你播放条占着这一行，收起时再按 ticker_shown 换回来
  if (show) {
    lv_obj_add_flag(sun_row, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(ticker_row, LV_OBJ_FLAG_HIDDEN);
  } else {
    lv_obj_add_flag(ticker_row, LV_OBJ_FLAG_HIDDEN);
    update_sun_row();
  }
}

static void build_github() {
  lv_obj_t *scr = make_screen();
  scr_pages[PAGE_GITHUB] = scr;

  lbl_gh_login = make_label(scr, &font_cn_16, COLOR_SUBTLE);
  lv_label_set_text(lbl_gh_login, "GitHub");
  lv_obj_align(lbl_gh_login, LV_ALIGN_TOP_LEFT, 12, 10);
  dot_wifi[PAGE_GITHUB] = make_wifi_dot(scr);

  lbl_gh_today = make_stat(scr, 0, "今日贡献");
  lbl_gh_streak = make_stat(scr, 1, "连续天数");
  lbl_gh_total = make_stat(scr, 2, "过去一年");

  // 热力图：每列一周，每行一个星期几（周日在最上面，和 GitHub 一致）
  int grid_w = kGithubWeeks * (kCell + kGap) - kGap;
  int grid_h = 7 * (kCell + kGap) - kGap;
  gh_grid = lv_obj_create(scr);
  lv_obj_remove_style_all(gh_grid);
  lv_obj_set_size(gh_grid, grid_w, grid_h);
  lv_obj_align(gh_grid, LV_ALIGN_TOP_MID, 0, 100);
  for (int w = 0; w < kGithubWeeks; w++) {
    for (int d = 0; d < 7; d++) {
      lv_obj_t *c = make_cell(gh_grid, kCell, 0);
      lv_obj_set_pos(c, w * (kCell + kGap), d * (kCell + kGap));
      gh_cells[w][d] = c;
    }
  }

  lbl_gh_msg = make_label(scr, &font_cn_16, COLOR_SUBTLE);
  lv_obj_set_width(lbl_gh_msg, SCREEN_W - 32);
  lv_obj_set_style_text_align(lbl_gh_msg, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(lbl_gh_msg, LV_ALIGN_TOP_MID, 0, 128);
  lv_label_set_text(lbl_gh_msg, "获取中…");

  // 底部：说明和图例
  lv_obj_t *caption = make_label(scr, &font_cn_16, COLOR_SUBTLE);
  lv_label_set_text_fmt(caption, "最近 %d 周", kGithubWeeks);
  lv_obj_align(caption, LV_ALIGN_BOTTOM_LEFT, 12, -12);

  lv_obj_t *more = make_label(scr, &font_cn_16, COLOR_SUBTLE);
  lv_label_set_text(more, "多");
  lv_obj_align(more, LV_ALIGN_BOTTOM_RIGHT, -12, -12);
  lv_obj_t *prev = more;
  for (int lv = 4; lv >= 0; lv--) {
    lv_obj_t *c = make_cell(scr, 10, lv);
    lv_obj_align_to(c, prev, LV_ALIGN_OUT_LEFT_MID, lv == 4 ? -6 : -3, 0);
    gh_legend[lv] = c;
    prev = c;
  }
  lv_obj_t *less = make_label(scr, &font_cn_16, COLOR_SUBTLE);
  lv_label_set_text(less, "少");
  lv_obj_align_to(less, prev, LV_ALIGN_OUT_LEFT_MID, -6, 0);
}

// 自选股：每行 名称 | 价格 | 涨跌幅，最多 6 行
static void build_stocks() {
  static constexpr int kRowH = 32;
  static constexpr int kTop = 40;
  lv_obj_t *scr = make_screen();
  scr_pages[PAGE_STOCKS] = scr;

  lv_obj_t *title = make_label(scr, &font_cn_16, COLOR_SUBTLE);
  lv_label_set_text(title, "自选股");
  lv_obj_align(title, LV_ALIGN_TOP_LEFT, 12, 10);
  dot_wifi[PAGE_STOCKS] = make_wifi_dot(scr);
  lbl_stock_updated = make_label(scr, &font_cn_16, COLOR_SUBTLE);
  lv_obj_align_to(lbl_stock_updated, dot_wifi[PAGE_STOCKS], LV_ALIGN_OUT_LEFT_MID, -8, 0);

  for (int i = 0; i < kMaxStocks; i++) {
    StockRow &r = stock_rows[i];
    r.row = lv_obj_create(scr);
    lv_obj_remove_style_all(r.row);
    lv_obj_set_size(r.row, SCREEN_W - 24, kRowH);
    lv_obj_set_pos(r.row, 12, kTop + i * kRowH);
    if (i < kMaxStocks - 1) {
      lv_obj_set_style_border_side(r.row, LV_BORDER_SIDE_BOTTOM, 0);
      lv_obj_set_style_border_width(r.row, 1, 0);
      lv_obj_set_style_border_color(r.row, COLOR_LINE, 0);
    }

    r.name = make_label(r.row, &font_cn_16, COLOR_TEXT);
    lv_obj_set_width(r.name, 120);
    lv_label_set_long_mode(r.name, LV_LABEL_LONG_DOT);
    lv_obj_align(r.name, LV_ALIGN_LEFT_MID, 0, 0);

    r.price = make_label(r.row, &lv_font_montserrat_20, COLOR_TEXT);
    lv_obj_set_width(r.price, 96);
    lv_obj_set_style_text_align(r.price, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_align(r.price, LV_ALIGN_RIGHT_MID, -88, 0);

    r.pill = lv_obj_create(r.row);
    lv_obj_set_size(r.pill, 80, 24);
    lv_obj_set_style_radius(r.pill, 4, 0);
    lv_obj_set_style_border_width(r.pill, 0, 0);
    lv_obj_set_style_pad_all(r.pill, 0, 0);
    lv_obj_set_style_bg_color(r.pill, COLOR_FLAT, 0);
    lv_obj_clear_flag(r.pill, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(r.pill, LV_ALIGN_RIGHT_MID, 0, 0);

    r.pct = make_label(r.pill, &font_cn_16, lv_color_hex(0xFFFFFF));
    lv_obj_center(r.pct);
    lv_obj_add_flag(r.row, LV_OBJ_FLAG_HIDDEN);
  }

  lbl_stock_msg = make_label(scr, &font_cn_16, COLOR_SUBTLE);
  lv_obj_set_width(lbl_stock_msg, SCREEN_W - 32);
  lv_obj_set_style_text_align(lbl_stock_msg, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(lbl_stock_msg, LV_ALIGN_CENTER, 0, 0);
  lv_label_set_text(lbl_stock_msg, "获取中…");
}

static void build_pet() {
  lv_obj_t *scr = make_screen();
  scr_pages[PAGE_PET] = scr;

  lv_obj_t *title = make_label(scr, &font_cn_16, COLOR_SUBTLE);
  lv_label_set_text(title, "小橘");
  lv_obj_align(title, LV_ALIGN_TOP_LEFT, 12, 10);
  dot_wifi[PAGE_PET] = make_wifi_dot(scr);

  pet_buf = (lv_color_t *)lv_mem_alloc(LV_CANVAS_BUF_SIZE_TRUE_COLOR(kPetPx, kPetPx));
  pet_canvas = lv_canvas_create(scr);
  if (pet_buf) lv_canvas_set_buffer(pet_canvas, pet_buf, kPetPx, kPetPx, LV_IMG_CF_TRUE_COLOR);
  lv_obj_align(pet_canvas, LV_ALIGN_TOP_MID, 0, kPetTop);

  lbl_pet_zzz = make_label(scr, &lv_font_montserrat_20, COLOR_SUBTLE);
  lv_obj_align(lbl_pet_zzz, LV_ALIGN_TOP_MID, kPetPx / 2 + 10, kPetTop);
  lv_obj_add_flag(lbl_pet_zzz, LV_OBJ_FLAG_HIDDEN);

  lbl_pet_line = make_label(scr, &font_cn_16, COLOR_TEXT);
  lv_obj_set_width(lbl_pet_line, SCREEN_W - 32);
  lv_obj_set_style_text_align(lbl_pet_line, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(lbl_pet_line, LV_ALIGN_TOP_MID, 0, kPetTop + kPetPx + 14);

  lbl_pet_status = make_label(scr, &font_cn_16, COLOR_SUBTLE);
  lv_obj_set_width(lbl_pet_status, SCREEN_W - 32);
  lv_obj_set_style_text_align(lbl_pet_status, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(lbl_pet_status, LV_ALIGN_BOTTOM_MID, 0, -12);

  // 频谱：16 根橘色的条，低音在左
  int w = kMusicBands * (kPetBarW + kPetBarGap) - kPetBarGap;
  for (int i = 0; i < kMusicBands; i++) {
    lv_obj_t *b = lv_obj_create(scr);
    lv_obj_set_style_border_width(b, 0, 0);
    lv_obj_set_style_radius(b, 2, 0);
    lv_obj_set_style_pad_all(b, 0, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(0xF28C3C), 0);
    lv_obj_set_style_bg_grad_color(b, lv_color_hex(0xFFD08A), 0);
    lv_obj_set_style_bg_grad_dir(b, LV_GRAD_DIR_VER, 0);
    lv_obj_clear_flag(b, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(b, kPetBarW, 2);
    lv_obj_set_pos(b, (SCREEN_W - w) / 2 + i * (kPetBarW + kPetBarGap), kPetBarBase - 2);
    lv_obj_add_flag(b, LV_OBJ_FLAG_HIDDEN);
    pet_bars[i] = b;
  }
}

static void build_album() {
  lv_obj_t *scr = make_screen();
  lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
  scr_pages[PAGE_ALBUM] = scr;
  dot_wifi[PAGE_ALBUM] = make_wifi_dot(scr);
  lv_obj_add_flag(dot_wifi[PAGE_ALBUM], LV_OBJ_FLAG_HIDDEN);  // 照片上不放 WiFi 小圆点

  album_dsc.header.always_zero = 0;
  album_dsc.header.cf = LV_IMG_CF_TRUE_COLOR;
  album_dsc.header.w = SCREEN_W;
  album_dsc.header.h = SCREEN_H;
  album_dsc.data_size = SCREEN_W * SCREEN_H * LV_COLOR_SIZE / 8;
  album_img = lv_img_create(scr);
  lv_obj_set_pos(album_img, 0, 0);
  lv_obj_add_flag(album_img, LV_OBJ_FLAG_HIDDEN);

  lbl_album_msg = make_label(scr, &font_cn_16, COLOR_SUBTLE);
  lv_obj_set_width(lbl_album_msg, SCREEN_W - 40);
  lv_obj_set_style_text_align(lbl_album_msg, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_style_text_line_space(lbl_album_msg, 6, 0);
  lv_label_set_recolor(lbl_album_msg, true);  // 第一行用亮一点的颜色当标题
  lv_obj_align(lbl_album_msg, LV_ALIGN_CENTER, 0, 0);
  lv_label_set_text(lbl_album_msg, "正在读取 TF 卡…");

  // 必应图片的说明：底部一行淡淡的小字，垫一条半透明的黑底，亮的照片上也看得清
  lbl_album_caption = make_label(scr, &font_cn_16, lv_color_white());
  lv_obj_set_size(lbl_album_caption, SCREEN_W, 26);
  lv_obj_align(lbl_album_caption, LV_ALIGN_BOTTOM_MID, 0, 0);
  lv_obj_set_style_pad_hor(lbl_album_caption, 12, 0);
  lv_obj_set_style_pad_top(lbl_album_caption, 3, 0);
  lv_obj_set_style_bg_color(lbl_album_caption, lv_color_black(), 0);
  lv_obj_set_style_bg_opa(lbl_album_caption, LV_OPA_40, 0);
  lv_obj_set_style_text_opa(lbl_album_caption, LV_OPA_80, 0);
  lv_obj_set_style_text_align(lbl_album_caption, LV_TEXT_ALIGN_CENTER, 0);
  lv_label_set_long_mode(lbl_album_caption, LV_LABEL_LONG_DOT);
  lv_obj_add_flag(lbl_album_caption, LV_OBJ_FLAG_HIDDEN);
}

void ui_set_album_photo(const uint16_t *px) {
  if (!px) {
    lv_obj_add_flag(album_img, LV_OBJ_FLAG_HIDDEN);
    return;
  }
  album_dsc.data = (const uint8_t *)px;
  lv_img_cache_invalidate_src(&album_dsc);  // 两帧轮流用同一个描述，换了像素要清掉缓存
  lv_img_set_src(album_img, &album_dsc);
  lv_obj_invalidate(album_img);
  lv_obj_clear_flag(album_img, LV_OBJ_FLAG_HIDDEN);
}

void ui_set_album_caption(const char *text) {
  if (!text || !*text) {
    lv_obj_add_flag(lbl_album_caption, LV_OBJ_FLAG_HIDDEN);
    return;
  }
  lv_label_set_text(lbl_album_caption, text);
  lv_obj_clear_flag(lbl_album_caption, LV_OBJ_FLAG_HIDDEN);
}

void ui_set_album_arrows(bool show) {
  for (lv_obj_t *a : page_arrows[PAGE_ALBUM]) {
    if (show) {
      lv_obj_clear_flag(a, LV_OBJ_FLAG_HIDDEN);
    } else {
      lv_obj_add_flag(a, LV_OBJ_FLAG_HIDDEN);
    }
  }
}

void ui_set_album_msg(const char *msg) {
  if (!msg) {
    lv_obj_add_flag(lbl_album_msg, LV_OBJ_FLAG_HIDDEN);
    return;
  }
  lv_label_set_text(lbl_album_msg, msg);
  lv_obj_clear_flag(lbl_album_msg, LV_OBJ_FLAG_HIDDEN);
}


// ---- 影片页：列表 + 全屏播放 ----

static constexpr int kMovieRowH = 42;
static constexpr int kMovieRowTop = 46;
static constexpr int kMovieRowGap = 4;
static constexpr uint32_t kMovieOrange = 0xF2A65A;  // 和网页设置一样的橙色，不跟主题变
static constexpr int kMovieBarX = 14, kMovieBarY = 204, kMovieBarW = SCREEN_W - 28;
static constexpr int kMovieBtnY = 120;               // 中间三个按钮的中心
static constexpr int kMovieBtnDx = 84;               // 快退、快进离中间多远

struct MovieRowUi {
  lv_obj_t *row;
  lv_obj_t *name;
  lv_obj_t *dur;
  lv_obj_t *bar;
  lv_obj_t *fill;
};
static MovieRowUi movie_rows[kMovieRows];
static lv_obj_t *movie_list;    // 列表那一屏的东西都放在这里面
static lv_obj_t *lbl_movie_page;
static lv_obj_t *lbl_movie_msg;
static lv_obj_t *movie_player;  // 全屏播放：黑底、画面、控制条
static lv_obj_t *movie_img;
static lv_img_dsc_t movie_dsc;
static lv_obj_t *movie_overlay;
static lv_obj_t *lbl_movie_title;
static lv_obj_t *lbl_movie_volume;
static lv_obj_t *lbl_movie_play;
static lv_obj_t *movie_bar_fill;
static lv_obj_t *movie_knob;
static lv_obj_t *lbl_movie_pos;
static lv_obj_t *lbl_movie_dur;
static lv_obj_t *lbl_movie_center;  // 正在打开、打不开时的提示
static bool movie_player_on = false;

static lv_obj_t *make_box(lv_obj_t *parent, int w, int h, uint32_t color, lv_opa_t opa, int radius) {
  lv_obj_t *o = lv_obj_create(parent);
  lv_obj_set_size(o, w, h);
  lv_obj_set_style_bg_color(o, lv_color_hex(color), 0);
  lv_obj_set_style_bg_opa(o, opa, 0);
  lv_obj_set_style_radius(o, radius, 0);
  lv_obj_set_style_border_width(o, 0, 0);
  lv_obj_set_style_pad_all(o, 0, 0);
  lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_clear_flag(o, LV_OBJ_FLAG_CLICKABLE);
  return o;
}

// 中间的圆按钮：半透明黑底白字，播放键大一点、橙色
static lv_obj_t *make_round(lv_obj_t *parent, int size, int dx, bool main_btn, const lv_font_t *font, const char *text) {
  lv_obj_t *b = make_box(parent, size, size, main_btn ? kMovieOrange : 0x000000, main_btn ? LV_OPA_90 : LV_OPA_50,
                         LV_RADIUS_CIRCLE);
  lv_obj_set_pos(b, SCREEN_W / 2 + dx - size / 2, kMovieBtnY - size / 2);
  lv_obj_t *l = make_label(b, font, lv_color_hex(main_btn ? 0x1A1A1A : 0xFFFFFF));
  lv_label_set_text(l, text);
  lv_obj_center(l);
  return l;
}

static void build_movie() {
  lv_obj_t *scr = make_screen();
  scr_pages[PAGE_MOVIE] = scr;
  dot_wifi[PAGE_MOVIE] = make_wifi_dot(scr);

  movie_list = make_box(scr, SCREEN_W, SCREEN_H, pal->bg, LV_OPA_TRANSP, 0);
  lv_obj_t *title = make_label(movie_list, &font_cn_24, COLOR_TEXT);
  lv_label_set_text(title, "影片");
  lv_obj_set_pos(title, 16, 8);
  lbl_movie_page = make_label(movie_list, &font_cn_16, COLOR_SUBTLE);
  lv_obj_align(lbl_movie_page, LV_ALIGN_TOP_RIGHT, -30, 14);
  for (int i = 0; i < kMovieRows; i++) {
    MovieRowUi &r = movie_rows[i];
    r.row = make_card(movie_list, 16, kMovieRowTop + i * (kMovieRowH + kMovieRowGap), SCREEN_W - 32, kMovieRowH);
    lv_obj_set_style_radius(r.row, 10, 0);
    lv_obj_t *play = make_label(r.row, &lv_font_montserrat_14, COLOR_SUBTLE);
    lv_label_set_text(play, LV_SYMBOL_PLAY);
    lv_obj_align(play, LV_ALIGN_LEFT_MID, 12, 0);
    r.name = make_label(r.row, &font_cn_16, COLOR_TEXT);
    lv_label_set_long_mode(r.name, LV_LABEL_LONG_DOT);
    lv_obj_set_size(r.name, 170, 20);
    lv_obj_set_pos(r.name, 34, 6);
    r.bar = make_box(r.row, 170, 3, pal->flat, LV_OPA_COVER, 2);
    lv_obj_set_pos(r.bar, 34, 30);
    r.fill = make_box(r.bar, 0, 3, kMovieOrange, LV_OPA_COVER, 2);
    r.dur = make_label(r.row, &font_cn_16, COLOR_SUBTLE);
    lv_obj_align(r.dur, LV_ALIGN_RIGHT_MID, -12, 0);
    lv_obj_add_flag(r.row, LV_OBJ_FLAG_HIDDEN);
  }
  lbl_movie_msg = make_label(movie_list, &font_cn_16, COLOR_SUBTLE);
  lv_obj_set_width(lbl_movie_msg, SCREEN_W - 40);
  lv_obj_set_style_text_align(lbl_movie_msg, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_style_text_line_space(lbl_movie_msg, 6, 0);
  lv_label_set_recolor(lbl_movie_msg, true);
  lv_obj_align(lbl_movie_msg, LV_ALIGN_CENTER, 0, 14);
  lv_label_set_text(lbl_movie_msg, "正在读取 TF 卡…");

  // 全屏播放：黑底上放画面，控制条盖在上面，点一下屏幕出来
  movie_player = make_box(scr, SCREEN_W, SCREEN_H, 0x000000, LV_OPA_COVER, 0);
  lv_obj_add_flag(movie_player, LV_OBJ_FLAG_HIDDEN);
  movie_dsc.header.always_zero = 0;
  movie_dsc.header.cf = LV_IMG_CF_TRUE_COLOR;
  movie_dsc.header.w = SCREEN_W;
  movie_dsc.header.h = SCREEN_H;
  movie_dsc.data_size = SCREEN_W * SCREEN_H * LV_COLOR_SIZE / 8;
  movie_img = lv_img_create(movie_player);
  lv_obj_set_pos(movie_img, 0, 0);
  lv_obj_add_flag(movie_img, LV_OBJ_FLAG_HIDDEN);
  lbl_movie_center = make_label(movie_player, &font_cn_16, lv_color_hex(0xC9D1D9));
  lv_obj_set_width(lbl_movie_center, SCREEN_W - 40);
  lv_obj_set_style_text_align(lbl_movie_center, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_style_text_line_space(lbl_movie_center, 6, 0);
  lv_obj_align(lbl_movie_center, LV_ALIGN_CENTER, 0, 0);

  movie_overlay = make_box(movie_player, SCREEN_W, SCREEN_H, 0x000000, LV_OPA_TRANSP, 0);
  lv_obj_add_flag(movie_overlay, LV_OBJ_FLAG_HIDDEN);
  // 上面一条：返回列表、片名、音量
  lv_obj_t *top = make_box(movie_overlay, SCREEN_W, 40, 0x000000, LV_OPA_50, 0);
  lv_obj_t *back = make_label(top, &lv_font_montserrat_20, lv_color_white());
  lv_label_set_text(back, LV_SYMBOL_LEFT);
  lv_obj_align(back, LV_ALIGN_LEFT_MID, 12, 0);
  lbl_movie_title = make_label(top, &font_cn_16, lv_color_white());
  lv_label_set_long_mode(lbl_movie_title, LV_LABEL_LONG_DOT);
  lv_obj_set_size(lbl_movie_title, 190, 20);
  lv_obj_align(lbl_movie_title, LV_ALIGN_LEFT_MID, 40, 0);
  lbl_movie_volume = make_label(top, &lv_font_montserrat_14, lv_color_white());
  lv_obj_align(lbl_movie_volume, LV_ALIGN_RIGHT_MID, -12, 0);
  // 中间：快退 10 秒、播放/暂停、快进 10 秒
  make_round(movie_overlay, 46, -kMovieBtnDx, false, &font_cn_16, "-10s");
  lbl_movie_play = make_round(movie_overlay, 60, 0, true, &lv_font_montserrat_20, LV_SYMBOL_PAUSE);
  make_round(movie_overlay, 46, kMovieBtnDx, false, &font_cn_16, "+10s");
  // 下面：进度条和时间
  lv_obj_t *bottom = make_box(movie_overlay, SCREEN_W, 52, 0x000000, LV_OPA_50, 0);
  lv_obj_set_pos(bottom, 0, SCREEN_H - 52);
  lv_obj_t *track = make_box(movie_overlay, kMovieBarW, 4, 0xFFFFFF, LV_OPA_30, 2);
  lv_obj_set_pos(track, kMovieBarX, kMovieBarY);
  movie_bar_fill = make_box(track, 0, 4, kMovieOrange, LV_OPA_COVER, 2);
  movie_knob = make_box(movie_overlay, 12, 12, 0xFFFFFF, LV_OPA_COVER, LV_RADIUS_CIRCLE);
  lbl_movie_pos = make_label(bottom, &lv_font_montserrat_14, lv_color_hex(0xE6EDF3));
  lv_obj_align(lbl_movie_pos, LV_ALIGN_BOTTOM_LEFT, kMovieBarX, -8);
  lbl_movie_dur = make_label(bottom, &lv_font_montserrat_14, lv_color_hex(0xE6EDF3));
  lv_obj_align(lbl_movie_dur, LV_ALIGN_BOTTOM_RIGHT, -kMovieBarX, -8);
}

static void set_hidden(lv_obj_t *o, bool hidden) {
  if (hidden) lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
  else lv_obj_clear_flag(o, LV_OBJ_FLAG_HIDDEN);
}

void ui_set_movie_list(const char *msg, const MovieRow *rows, int n, int page, int pages) {
  set_hidden(lbl_movie_msg, !msg);
  if (msg) lv_label_set_text(lbl_movie_msg, msg);
  for (int i = 0; i < kMovieRows; i++) {
    MovieRowUi &r = movie_rows[i];
    set_hidden(r.row, msg || i >= n);
    if (msg || i >= n) continue;
    lv_label_set_text(r.name, rows[i].name);
    char t[16] = "";
    if (rows[i].dur_ms) movie_format_time(rows[i].dur_ms, t, sizeof(t));
    lv_label_set_text(r.dur, t);
    uint32_t pos = rows[i].pos_ms, dur = rows[i].dur_ms;
    int w = dur ? (int)((uint64_t)170 * (pos > dur ? dur : pos) / dur) : 0;
    lv_obj_set_width(r.fill, w);
    set_hidden(r.bar, pos == 0);  // 还没看过的不画进度条
  }
  if (pages > 1 && !msg) {
    char t[24];
    snprintf(t, sizeof(t), "%d / %d", page + 1, pages);
    lv_label_set_text(lbl_movie_page, t);
  } else {
    lv_label_set_text(lbl_movie_page, "");
  }
}

int ui_movie_list_hit(int x, int y) {
  if (movie_player_on || x < 16 || x >= SCREEN_W - 16) return -1;
  for (int i = 0; i < kMovieRows; i++) {
    int top = kMovieRowTop + i * (kMovieRowH + kMovieRowGap);
    if (y >= top - 2 && y < top + kMovieRowH + 2 && !lv_obj_has_flag(movie_rows[i].row, LV_OBJ_FLAG_HIDDEN)) return i;
  }
  return -1;
}

void ui_movie_show_player(bool on) {
  movie_player_on = on;
  set_hidden(movie_player, !on);
  set_hidden(movie_list, on);
  set_hidden(dot_wifi[PAGE_MOVIE], on);
  for (lv_obj_t *a : page_arrows[PAGE_MOVIE]) set_hidden(a, on);  // 放片子时边上不放翻页提示
  if (!on) lv_obj_add_flag(movie_img, LV_OBJ_FLAG_HIDDEN);
}

void ui_set_movie_frame(const uint16_t *px) {
  movie_dsc.data = (const uint8_t *)px;
  lv_img_cache_invalidate_src(&movie_dsc);  // 两帧轮流用同一个描述，换了像素要清掉缓存
  lv_img_set_src(movie_img, &movie_dsc);
  lv_obj_invalidate(movie_img);
  lv_obj_clear_flag(movie_img, LV_OBJ_FLAG_HIDDEN);
}

void ui_set_movie_player(const MovieView &v) {
  const char *center = v.error ? v.error : v.loading ? "正在打开…" : nullptr;
  set_hidden(lbl_movie_center, !center);
  if (center) lv_label_set_text(lbl_movie_center, center);
  // 暂停、放完、打不开时一直显示控制条
  set_hidden(movie_overlay, !(v.overlay || v.paused || v.ended || v.error));
  lv_label_set_text(lbl_movie_title, v.title ? v.title : "");
  char t[24];
  snprintf(t, sizeof(t), "%s %d%%", !v.sound ? LV_SYMBOL_MUTE : v.volume == 0 ? LV_SYMBOL_MUTE : LV_SYMBOL_VOLUME_MAX,
           v.volume);
  lv_label_set_text(lbl_movie_volume, v.sound ? t : LV_SYMBOL_MUTE " --");
  lv_label_set_text(lbl_movie_play, v.paused || v.ended ? LV_SYMBOL_PLAY : LV_SYMBOL_PAUSE);
  uint32_t pos = v.pos_ms > v.dur_ms ? v.dur_ms : v.pos_ms;
  int w = v.dur_ms ? (int)((uint64_t)kMovieBarW * pos / v.dur_ms) : 0;
  lv_obj_set_width(movie_bar_fill, w);
  lv_obj_set_pos(movie_knob, kMovieBarX + w - 6, kMovieBarY - 4);
  movie_format_time(pos, t, sizeof(t));
  lv_label_set_text(lbl_movie_pos, t);
  movie_format_time(v.dur_ms, t, sizeof(t));
  lv_label_set_text(lbl_movie_dur, t);
}

MovieHit ui_movie_hit(int x, int y, float *frac) {
  if (!movie_player_on || lv_obj_has_flag(movie_overlay, LV_OBJ_FLAG_HIDDEN)) return MovieHit::None;
  if (y < 44) {
    if (x < 70) return MovieHit::Back;
    if (x >= SCREEN_W - 90) return MovieHit::Volume;
    return MovieHit::None;
  }
  if (y >= kMovieBarY - 22) {
    if (frac) {
      float f = (float)(x - kMovieBarX) / kMovieBarW;
      *frac = f < 0 ? 0 : f > 1 ? 1 : f;
    }
    return MovieHit::Progress;
  }
  if (abs(y - kMovieBtnY) <= 40) {
    int dx = x - SCREEN_W / 2;
    if (abs(dx) <= 38) return MovieHit::PlayPause;
    if (abs(dx + kMovieBtnDx) <= 40) return MovieHit::Rewind;
    if (abs(dx - kMovieBtnDx) <= 40) return MovieHit::Forward;
  }
  return MovieHit::None;
}

// ---------- 好友页 ----------
// 顶栏：圈子名（几个圈子时点它换）、「邀请」；中间一屏 3 个人，每人一个「戳一戳」；底部「戳全部人」「发留言」
static constexpr int kFrRowTop = 36, kFrRowStep = 42, kFrRowH = 38;
static constexpr int kFrBtnY = 188, kFrBtnH = 44;
static constexpr uint32_t kFrOrange = 0xF08A2C;  // 戳一戳的按钮，两套主题都用它
static constexpr uint32_t kFrHere = 0x3FB950;
static lv_obj_t *lbl_fr_title;
static lv_obj_t *fr_invite;
static lv_obj_t *lbl_fr_msg;
static lv_obj_t *lbl_fr_note;
static lv_obj_t *fr_all;
static lv_obj_t *fr_send;
struct FriendRowUi {
  lv_obj_t *card, *name, *dot, *state, *poke;
};
static FriendRowUi fr_rows[kFriendRows];

// 圆角按钮，里面一行字。bg 是 0 时用主题的灰色
static lv_obj_t *make_text_button(lv_obj_t *parent, int x, int y, int w, int h, uint32_t bg, const char *text,
                                  lv_obj_t **label = nullptr) {
  lv_obj_t *b = lv_obj_create(parent);
  lv_obj_set_pos(b, x, y);
  lv_obj_set_size(b, w, h);
  lv_obj_set_style_radius(b, h / 2, 0);
  lv_obj_set_style_border_width(b, 0, 0);
  lv_obj_set_style_pad_all(b, 0, 0);
  lv_obj_set_style_bg_color(b, bg ? lv_color_hex(bg) : COLOR_FLAT, 0);
  lv_obj_clear_flag(b, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
  lv_obj_t *l = make_label(b, &font_cn_16, bg ? lv_color_hex(0xFFFFFF) : COLOR_TEXT);
  lv_label_set_text(l, text);
  lv_obj_center(l);
  if (label) *label = l;
  return b;
}

static void build_friends() {
  lv_obj_t *scr = make_screen();
  scr_pages[PAGE_FRIENDS] = scr;

  lbl_fr_title = make_label(scr, &font_cn_16, COLOR_SUBTLE);
  lv_label_set_long_mode(lbl_fr_title, LV_LABEL_LONG_DOT);
  lv_obj_set_width(lbl_fr_title, 200);
  lv_obj_set_pos(lbl_fr_title, 12, 10);
  dot_wifi[PAGE_FRIENDS] = make_wifi_dot(scr);
  fr_invite = make_text_button(scr, SCREEN_W - 92, 5, 64, 28, 0, "邀请");

  for (int i = 0; i < kFriendRows; i++) {
    FriendRowUi &r = fr_rows[i];
    r.card = make_card(scr, 12, kFrRowTop + i * kFrRowStep, SCREEN_W - 24, kFrRowH);
    r.name = make_label(r.card, &font_cn_16, COLOR_TEXT);
    lv_label_set_long_mode(r.name, LV_LABEL_LONG_DOT);
    lv_obj_set_width(r.name, 96);
    lv_obj_align(r.name, LV_ALIGN_LEFT_MID, 12, 0);
    r.dot = lv_obj_create(r.card);
    lv_obj_set_size(r.dot, 8, 8);
    lv_obj_set_style_radius(r.dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(r.dot, 0, 0);
    lv_obj_set_style_bg_color(r.dot, lv_color_hex(kFrHere), 0);
    lv_obj_align(r.dot, LV_ALIGN_LEFT_MID, 116, 0);
    r.state = make_label(r.card, &font_cn_16, COLOR_SUBTLE);
    lv_obj_align(r.state, LV_ALIGN_LEFT_MID, 130, 0);
    r.poke = make_text_button(r.card, SCREEN_W - 24 - 84, 4, 78, kFrRowH - 8, kFrOrange, "戳一戳");
  }

  lbl_fr_note = make_label(scr, &font_cn_16, COLOR_SUBTLE);
  lv_obj_set_width(lbl_fr_note, SCREEN_W - 24);
  lv_label_set_long_mode(lbl_fr_note, LV_LABEL_LONG_DOT);
  lv_obj_set_style_text_align(lbl_fr_note, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(lbl_fr_note, LV_ALIGN_TOP_MID, 0, kFrRowTop + kFriendRows * kFrRowStep + 2);

  fr_all = make_text_button(scr, 12, kFrBtnY, 144, kFrBtnH, 0, "戳全部人");
  lv_obj_set_style_bg_color(fr_all, COLOR_ACCENT, 0);
  lv_obj_set_style_text_color(lv_obj_get_child(fr_all, 0), lv_color_hex(0xFFFFFF), 0);
  fr_send = make_text_button(scr, SCREEN_W - 12 - 144, kFrBtnY, 144, kFrBtnH, 0, "发留言 ›");

  lbl_fr_msg = make_label(scr, &font_cn_16, COLOR_SUBTLE);
  lv_obj_set_width(lbl_fr_msg, SCREEN_W - 48);
  lv_label_set_long_mode(lbl_fr_msg, LV_LABEL_LONG_WRAP);
  lv_obj_set_style_text_align(lbl_fr_msg, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_style_text_line_space(lbl_fr_msg, 4, 0);
  lv_label_set_recolor(lbl_fr_msg, true);
  lv_obj_align(lbl_fr_msg, LV_ALIGN_CENTER, 0, 6);
}

void ui_set_friends(const FriendsView &v) {
  bool list = v.msg == nullptr;
  set_hidden(lbl_fr_msg, list);
  if (!list) lv_label_set_text(lbl_fr_msg, v.msg);
  char title[64];
  snprintf(title, sizeof(title), "%s%s", v.title ? v.title : "好友", v.switchable ? " ›" : "");
  lv_label_set_text(lbl_fr_title, title);
  set_hidden(fr_invite, !(list && v.can_invite));
  for (int i = 0; i < kFriendRows; i++) {
    FriendRowUi &r = fr_rows[i];
    bool on = list && i < v.n;
    set_hidden(r.card, !on);
    if (!on) continue;
    const FriendRow &f = v.rows[i];
    lv_label_set_text(r.name, f.name);
    set_hidden(r.dot, !f.here);
    lv_label_set_text(r.state, f.here ? "在桌前" : f.online ? "在线" : "不在");
    lv_obj_align(r.state, LV_ALIGN_LEFT_MID, f.here ? 130 : 116, 0);
    lv_obj_set_style_text_color(r.state, f.here ? lv_color_hex(kFrHere) : COLOR_SUBTLE, 0);
  }
  bool any = list && v.n > 0;
  set_hidden(fr_all, !any);
  set_hidden(fr_send, !any);
  char note[128] = "";
  if (list && v.note && v.note[0]) snprintf(note, sizeof(note), "%s", v.note);
  else if (list && v.pages > 1) snprintf(note, sizeof(note), "%d/%d  上下滑动看其他人", v.page + 1, v.pages);
  else if (list && v.n == 0) snprintf(note, sizeof(note), "圈子里还只有你，点右上角「邀请」");
  lv_label_set_text(lbl_fr_note, note);
  set_hidden(lbl_fr_note, !list);
}

// 按钮本身的范围再放宽 2 像素，相邻的按钮不会抢
static bool hit_exact(lv_obj_t *o, int x, int y) {
  if (lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN)) return false;
  lv_area_t a;
  lv_obj_get_coords(o, &a);
  return x >= a.x1 - 2 && x <= a.x2 + 2 && y >= a.y1 - 2 && y <= a.y2 + 2;
}

FriendsHit ui_friends_hit(int x, int y, int *row) {
  lv_obj_update_layout(scr_pages[PAGE_FRIENDS]);
  if (hit_exact(fr_invite, x, y)) return FriendsHit::Invite;
  for (int i = 0; i < kFriendRows; i++) {
    if (!lv_obj_has_flag(fr_rows[i].card, LV_OBJ_FLAG_HIDDEN) && hit_exact(fr_rows[i].poke, x, y)) {
      if (row) *row = i;
      return FriendsHit::Poke;
    }
  }
  if (hit_exact(fr_all, x, y)) return FriendsHit::PokeAll;
  if (hit_exact(fr_send, x, y)) return FriendsHit::Send;
  if (y < 32 && x < 220) return FriendsHit::Title;
  return FriendsHit::None;
}

void ui_set_friends_here(const char *text) {
  bool show = text && text[0];
  friends_here_shown = show;
  if (show) {
    char buf[96];
    snprintf(buf, sizeof(buf), "#3FB950 ●# %s", text);
    lv_label_set_text(lbl_city, buf);
  } else {
    lv_label_set_text(lbl_city, city_name.c_str());
  }
  lv_obj_align_to(lbl_city, dot_wifi[PAGE_CLOCK], LV_ALIGN_OUT_LEFT_MID, -8, 0);
}

// ---------- 选一句话 ----------
static constexpr int kPhW = 96, kPhH = 48, kPhGap = 8, kPhTop = 46;
static lv_obj_t *scr_phrases;
static lv_obj_t *lbl_ph_title;
static lv_obj_t *ph_chips[kFrPhraseCount];
static lv_obj_t *ph_cancel;

static void build_phrases() {
  scr_phrases = make_screen();
  lbl_ph_title = make_label(scr_phrases, &font_cn_16, COLOR_SUBTLE);
  lv_obj_set_pos(lbl_ph_title, 12, 10);
  int left = (SCREEN_W - 3 * kPhW - 2 * kPhGap) / 2;
  for (int i = 0; i < kFrPhraseCount; i++) {
    ph_chips[i] = make_text_button(scr_phrases, left + i % 3 * (kPhW + kPhGap), kPhTop + i / 3 * (kPhH + kPhGap), kPhW,
                                   kPhH, 0, kFrPhrases[i]);
    lv_obj_set_style_radius(ph_chips[i], 12, 0);
    lv_obj_set_style_bg_color(ph_chips[i], COLOR_CARD, 0);
  }
  ph_cancel = make_text_button(scr_phrases, (SCREEN_W - 144) / 2, kFrBtnY, 144, kFrBtnH, 0, "取消");
}

void ui_show_phrases(const char *title) {
  lv_label_set_text(lbl_ph_title, title);
  screen_load(scr_phrases, false);
}

int ui_phrases_hit(int x, int y) {
  lv_obj_update_layout(scr_phrases);
  for (int i = 0; i < kFrPhraseCount; i++) {
    if (hit_exact(ph_chips[i], x, y)) return i;
  }
  if (hit_exact(ph_cancel, x, y)) return -1;
  return -2;
}

// ---------- 好友弹窗 ----------
static constexpr int kFrCatPx = kPetSize * 4;
static lv_obj_t *scr_friend;
static lv_obj_t *lbl_fa_title;
static lv_obj_t *lbl_fa_body;
static lv_obj_t *lbl_fa_foot;
static lv_obj_t *fa_cat;
static lv_color_t *fa_cat_buf;
static lv_obj_t *fa_img;
static lv_img_dsc_t fa_dsc;
static lv_obj_t *fa_action;
static lv_obj_t *lbl_fa_action;
static lv_obj_t *fa_close;
static uint8_t fa_cat_epoch = 0xFF;

static void build_friend_alert() {
  scr_friend = make_screen();
  // 名字什么字都有，font_cn_24 只有源码里用到的字，所以标题用 16 号（GB2312 全部汉字）
  lbl_fa_title = make_label(scr_friend, &font_cn_16, COLOR_TEXT);
  lv_obj_set_width(lbl_fa_title, SCREEN_W - 32);
  lv_label_set_long_mode(lbl_fa_title, LV_LABEL_LONG_DOT);
  lv_obj_set_style_text_align(lbl_fa_title, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(lbl_fa_title, LV_ALIGN_TOP_MID, 0, 14);

  fa_cat_buf = (lv_color_t *)lv_mem_alloc(LV_CANVAS_BUF_SIZE_TRUE_COLOR(kFrCatPx, kFrCatPx));
  fa_cat = lv_canvas_create(scr_friend);
  if (fa_cat_buf) lv_canvas_set_buffer(fa_cat, fa_cat_buf, kFrCatPx, kFrCatPx, LV_IMG_CF_TRUE_COLOR);
  lv_obj_set_pos(fa_cat, 40, 62);

  fa_img = lv_img_create(scr_friend);
  lv_obj_set_pos(fa_img, 12, 52);
  fa_dsc.header.cf = LV_IMG_CF_TRUE_COLOR;
  fa_dsc.header.w = kFrThumbW;
  fa_dsc.header.h = kFrThumbH;
  fa_dsc.data_size = kFrThumbW * kFrThumbH * 2;

  lbl_fa_body = make_label(scr_friend, &font_cn_16, COLOR_TEXT);
  lv_label_set_long_mode(lbl_fa_body, LV_LABEL_LONG_WRAP);
  lv_obj_set_style_text_line_space(lbl_fa_body, 4, 0);

  lbl_fa_foot = make_label(scr_friend, &font_cn_16, COLOR_SUBTLE);
  lv_obj_set_width(lbl_fa_foot, SCREEN_W - 24);
  lv_label_set_long_mode(lbl_fa_foot, LV_LABEL_LONG_DOT);
  lv_obj_set_style_text_align(lbl_fa_foot, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(lbl_fa_foot, LV_ALIGN_TOP_MID, 0, 162);

  fa_action = make_text_button(scr_friend, 12, kFrBtnY, 144, kFrBtnH, kFrOrange, "", &lbl_fa_action);
  fa_close = make_text_button(scr_friend, SCREEN_W - 12 - 144, kFrBtnY, 144, kFrBtnH, 0, "关闭");
}

void ui_show_friend_alert(const FriendAlertView &v) {
  lv_label_set_text(lbl_fa_title, v.title);
  set_hidden(fa_cat, !v.poke);
  if (v.poke && fa_cat_buf && fa_cat_epoch != theme_epoch) {
    fa_cat_epoch = theme_epoch;
    for (int y = 0; y < kFrCatPx; y++) {
      for (int x = 0; x < kFrCatPx; x++)
        fa_cat_buf[y * kFrCatPx + x] = pet_color(pet_pixel(PetMood::Happy, false, x / 4, y / 4));
    }
    lv_obj_invalidate(fa_cat);
  }
  set_hidden(fa_img, !v.thumb);
  if (v.thumb) {
    fa_dsc.data = (const uint8_t *)v.thumb;
    lv_img_set_src(fa_img, &fa_dsc);
    lv_obj_invalidate(fa_img);
  }

  lv_obj_set_style_text_font(lbl_fa_body, v.big ? &lv_font_montserrat_48 : v.poke ? &font_cn_24 : &font_cn_16, 0);
  lv_obj_set_style_text_color(lbl_fa_body, v.poke ? lv_color_hex(0xFF8FA3) : COLOR_TEXT, 0);
  lv_label_set_text(lbl_fa_body, v.body);
  if (v.poke) {
    lv_obj_set_width(lbl_fa_body, SCREEN_W - 136);
    lv_obj_set_style_text_align(lbl_fa_body, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_align(lbl_fa_body, LV_ALIGN_TOP_LEFT, 124, 80);
  } else if (v.thumb) {
    lv_obj_set_width(lbl_fa_body, SCREEN_W - 12 - kFrThumbW - 24);
    lv_obj_set_style_text_align(lbl_fa_body, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_align(lbl_fa_body, LV_ALIGN_TOP_LEFT, 12 + kFrThumbW + 12, 52);
  } else {
    lv_obj_set_width(lbl_fa_body, SCREEN_W - 40);
    lv_obj_set_style_text_align(lbl_fa_body, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(lbl_fa_body, LV_ALIGN_TOP_MID, 0, v.big ? 70 : 62);
  }
  lv_label_set_text(lbl_fa_foot, v.foot ? v.foot : "");

  set_hidden(fa_action, !v.action);
  if (v.action) {
    lv_label_set_text(lbl_fa_action, v.action);
    lv_obj_set_x(fa_close, SCREEN_W - 12 - 144);
  } else {
    lv_obj_set_x(fa_close, (SCREEN_W - 144) / 2);
  }
  screen_load(scr_friend, false);
}

FriendAlertHit ui_friend_alert_hit(int x, int y) {
  lv_obj_update_layout(scr_friend);
  if (hit_exact(fa_action, x, y)) return FriendAlertHit::Action;
  if (hit_exact(fa_close, x, y)) return FriendAlertHit::Close;
  return FriendAlertHit::None;
}

// ---- 音乐页：播放画面 + 歌曲列表 ----

static constexpr int kMusicCoverX = 16, kMusicCoverY = 34;
static constexpr int kMusicTextX = kMusicCoverX + kPlayerCover + 14;
static constexpr int kMusicBars = 8;  // 16 根频谱两两合成一根，放在封面右边
static constexpr int kMusicBarW = 6, kMusicBarGap = 3, kMusicBarMaxH = 18;
static constexpr int kMusicBarsX = 226, kMusicBarsBase = 124;
static constexpr int kMusicProgX = 58, kMusicProgY = 187, kMusicProgW = 204;
static constexpr int kMusicBtnY = 214;  // 底下一排按钮的中心
static constexpr int kMusicBtnX[5] = {40, 100, 160, 220, 280};  // 播放顺序、上一首、播放、下一首、列表
static constexpr int kMusicRowTop = 66, kMusicRowH = 38, kMusicRowGap = 4;
static constexpr int kMusicChipY = 34, kMusicChipH = 26;
static constexpr int kMusicChips = 6;

static lv_obj_t *music_player_box;  // 播放画面的东西都放在这里面
static lv_obj_t *lbl_music_head;
static lv_obj_t *lbl_music_queue;
static lv_obj_t *music_sleep;
static lv_obj_t *lbl_music_sleep;
static lv_obj_t *music_cover_art;  // 没有封面时的音符图
static lv_obj_t *music_cover_img;
static lv_img_dsc_t music_cover_dsc;
static uint16_t *music_cover_px;   // ui 自己留一份，播放器那边换歌也不影响
static lv_obj_t *lbl_music_title;
static lv_obj_t *lbl_music_artist;
static lv_obj_t *lbl_music_album;
static lv_obj_t *music_bars[kMusicBars];
static lv_obj_t *lbl_music_lyric;
static lv_obj_t *lbl_music_lyric_next;
static lv_obj_t *lbl_music_pos;
static lv_obj_t *lbl_music_dur;
static lv_obj_t *music_prog_fill;
static lv_obj_t *music_knob;
static lv_obj_t *lbl_music_mode;
static lv_obj_t *lbl_music_mode_one;  // 单曲循环时图标右下角的小「1」
static lv_obj_t *lbl_music_play;
static lv_obj_t *lbl_music_msg;       // 没插卡、没有歌时盖住整页的提示

static lv_obj_t *music_list_box;
static lv_obj_t *lbl_music_count;
struct MusicChipUi {
  lv_obj_t *box;
  lv_obj_t *text;
  int index;  // 第几个歌单，-1 表示「›」
};
static MusicChipUi music_chips[kMusicChips];
static int music_chip_next = 0;  // 点「›」后最左边显示第几个歌单
struct MusicRowUi {
  lv_obj_t *row;
  lv_obj_t *num;
  lv_obj_t *title;
  lv_obj_t *artist;
  lv_obj_t *dur;
};
static MusicRowUi music_rows[kMusicRows];
static lv_obj_t *lbl_music_list_msg;
static bool music_list_on = false;

// 迷你播放条（时钟页）
static lv_obj_t *music_bar;
static lv_obj_t *lbl_music_bar;
static lv_obj_t *lbl_music_bar_play;

static lv_obj_t *music_icon(lv_obj_t *parent, int cx, const lv_font_t *font, const char *sym, uint32_t color) {
  lv_obj_t *l = make_label(parent, font, lv_color_hex(color));
  lv_label_set_text(l, sym);
  lv_obj_align(l, LV_ALIGN_TOP_LEFT, 0, 0);
  lv_obj_update_layout(l);
  lv_obj_set_pos(l, cx - lv_obj_get_width(l) / 2, kMusicBtnY - lv_obj_get_height(l) / 2);
  return l;
}

static void build_music() {
  lv_obj_t *scr = make_screen();
  scr_pages[PAGE_MUSIC] = scr;
  dot_wifi[PAGE_MUSIC] = make_wifi_dot(scr);

  // 播放画面
  music_player_box = make_box(scr, SCREEN_W, SCREEN_H, pal->bg, LV_OPA_TRANSP, 0);
  lv_obj_t *note = make_label(music_player_box, &lv_font_montserrat_14, lv_color_hex(kMovieOrange));
  lv_label_set_text(note, LV_SYMBOL_AUDIO);
  lv_obj_set_pos(note, 16, 11);
  lbl_music_head = make_label(music_player_box, &font_cn_16, COLOR_SUBTLE);
  lv_label_set_long_mode(lbl_music_head, LV_LABEL_LONG_DOT);
  lv_obj_set_size(lbl_music_head, 150, 20);
  lv_obj_set_pos(lbl_music_head, 36, 8);
  music_sleep = make_box(music_player_box, 64, 22, pal->flat, LV_OPA_COVER, 11);
  lv_obj_set_pos(music_sleep, 182, 7);
  lbl_music_sleep = make_label(music_sleep, &font_cn_16, COLOR_SUBTLE);
  lv_obj_center(lbl_music_sleep);
  lbl_music_queue = make_label(music_player_box, &lv_font_montserrat_14, COLOR_SUBTLE);
  lv_obj_align(lbl_music_queue, LV_ALIGN_TOP_RIGHT, -26, 11);

  // 封面：默认是橙到紫的渐变上一个大音符
  music_cover_art = make_box(music_player_box, kPlayerCover, kPlayerCover, 0xF2A65A, LV_OPA_COVER, 12);
  lv_obj_set_style_bg_grad_color(music_cover_art, lv_color_hex(0x8A4FE0), 0);
  lv_obj_set_style_bg_grad_dir(music_cover_art, LV_GRAD_DIR_VER, 0);
  lv_obj_set_pos(music_cover_art, kMusicCoverX, kMusicCoverY);
  lv_obj_t *big = make_label(music_cover_art, &lv_font_montserrat_48, lv_color_hex(0xFFFFFF));
  lv_obj_set_style_text_opa(big, LV_OPA_80, 0);
  lv_label_set_text(big, LV_SYMBOL_AUDIO);
  lv_obj_center(big);
  music_cover_px = (uint16_t *)heap_caps_malloc(kPlayerCover * kPlayerCover * 2, MALLOC_CAP_SPIRAM);
  music_cover_dsc.header.always_zero = 0;
  music_cover_dsc.header.cf = LV_IMG_CF_TRUE_COLOR;
  music_cover_dsc.header.w = kPlayerCover;
  music_cover_dsc.header.h = kPlayerCover;
  music_cover_dsc.data_size = kPlayerCover * kPlayerCover * LV_COLOR_SIZE / 8;
  music_cover_dsc.data = (const uint8_t *)music_cover_px;
  music_cover_img = lv_img_create(music_player_box);
  lv_obj_set_pos(music_cover_img, kMusicCoverX, kMusicCoverY);
  lv_obj_add_flag(music_cover_img, LV_OBJ_FLAG_HIDDEN);

  const int text_w = SCREEN_W - kMusicTextX - 14;
  lbl_music_title = make_label(music_player_box, &font_cn_16, COLOR_TEXT);
  lbl_music_artist = make_label(music_player_box, &font_cn_16, COLOR_SUBTLE);
  lbl_music_album = make_label(music_player_box, &font_cn_16, COLOR_SUBTLE);
  lv_obj_t *lines[] = {lbl_music_title, lbl_music_artist, lbl_music_album};
  for (int i = 0; i < 3; i++) {
    lv_label_set_long_mode(lines[i], LV_LABEL_LONG_DOT);
    lv_obj_set_size(lines[i], text_w, 20);
    lv_obj_set_pos(lines[i], kMusicTextX, 40 + i * 23);
  }
  lv_obj_set_width(lbl_music_album, kMusicBarsX - kMusicTextX - 4);  // 别压住频谱

  for (int i = 0; i < kMusicBars; i++) {
    lv_obj_t *b = make_box(music_player_box, kMusicBarW, 2, 0xF28C3C, LV_OPA_COVER, 1);
    lv_obj_set_style_bg_grad_color(b, lv_color_hex(0xFFD08A), 0);
    lv_obj_set_style_bg_grad_dir(b, LV_GRAD_DIR_VER, 0);
    lv_obj_set_pos(b, kMusicBarsX + i * (kMusicBarW + kMusicBarGap), kMusicBarsBase - 2);
    music_bars[i] = b;
  }

  lbl_music_lyric = make_label(music_player_box, &font_cn_16, lv_color_hex(kMovieOrange));
  lbl_music_lyric_next = make_label(music_player_box, &font_cn_16, COLOR_SUBTLE);
  for (lv_obj_t *l : {lbl_music_lyric, lbl_music_lyric_next}) {
    lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
    lv_obj_set_size(l, SCREEN_W - 40, 20);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
  }
  lv_obj_set_pos(lbl_music_lyric, 20, 134);
  lv_obj_set_pos(lbl_music_lyric_next, 20, 156);

  lbl_music_pos = make_label(music_player_box, &lv_font_montserrat_14, COLOR_SUBTLE);
  lv_obj_set_pos(lbl_music_pos, 16, kMusicProgY - 7);
  lbl_music_dur = make_label(music_player_box, &lv_font_montserrat_14, COLOR_SUBTLE);
  lv_obj_align(lbl_music_dur, LV_ALIGN_TOP_RIGHT, -16, kMusicProgY - 7);
  lv_obj_t *track = make_box(music_player_box, kMusicProgW, 4, pal->flat, LV_OPA_COVER, 2);
  lv_obj_set_pos(track, kMusicProgX, kMusicProgY);
  music_prog_fill = make_box(track, 0, 4, kMovieOrange, LV_OPA_COVER, 2);
  music_knob = make_box(music_player_box, 12, 12, 0xFFFFFF, LV_OPA_COVER, LV_RADIUS_CIRCLE);
  lv_obj_set_style_border_width(music_knob, 1, 0);
  lv_obj_set_style_border_color(music_knob, lv_color_hex(0xC9D1D9), 0);
  lv_obj_set_pos(music_knob, kMusicProgX - 6, kMusicProgY - 4);

  lbl_music_mode = music_icon(music_player_box, kMusicBtnX[0], &lv_font_montserrat_20, LV_SYMBOL_LOOP, kMovieOrange);
  lbl_music_mode_one = make_label(music_player_box, &lv_font_montserrat_14, lv_color_hex(kMovieOrange));
  lv_label_set_text(lbl_music_mode_one, "1");
  lv_obj_set_pos(lbl_music_mode_one, kMusicBtnX[0] + 11, kMusicBtnY);
  music_icon(music_player_box, kMusicBtnX[1], &lv_font_montserrat_20, LV_SYMBOL_PREV, pal->text);
  lv_obj_t *play = make_box(music_player_box, 44, 44, kMovieOrange, LV_OPA_COVER, LV_RADIUS_CIRCLE);
  lv_obj_set_pos(play, kMusicBtnX[2] - 22, kMusicBtnY - 22);
  lbl_music_play = make_label(play, &lv_font_montserrat_20, lv_color_hex(0x1A1A1A));
  lv_label_set_text(lbl_music_play, LV_SYMBOL_PLAY);
  lv_obj_center(lbl_music_play);
  music_icon(music_player_box, kMusicBtnX[3], &lv_font_montserrat_20, LV_SYMBOL_NEXT, pal->text);
  music_icon(music_player_box, kMusicBtnX[4], &lv_font_montserrat_20, LV_SYMBOL_LIST, pal->subtle);

  lbl_music_msg = make_label(scr, &font_cn_16, COLOR_SUBTLE);
  lv_obj_set_width(lbl_music_msg, SCREEN_W - 40);
  lv_obj_set_style_text_align(lbl_music_msg, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_style_text_line_space(lbl_music_msg, 6, 0);
  lv_label_set_recolor(lbl_music_msg, true);
  lv_obj_align(lbl_music_msg, LV_ALIGN_CENTER, 0, 0);
  lv_obj_add_flag(lbl_music_msg, LV_OBJ_FLAG_HIDDEN);

  // 歌曲列表
  music_list_box = make_box(scr, SCREEN_W, SCREEN_H, pal->bg, LV_OPA_TRANSP, 0);
  lv_obj_add_flag(music_list_box, LV_OBJ_FLAG_HIDDEN);
  lv_obj_t *back = make_label(music_list_box, &lv_font_montserrat_20, COLOR_SUBTLE);
  lv_label_set_text(back, LV_SYMBOL_LEFT);
  lv_obj_set_pos(back, 12, 8);
  lv_obj_t *title = make_label(music_list_box, &font_cn_24, COLOR_TEXT);
  lv_label_set_text(title, "音乐");
  lv_obj_set_pos(title, 36, 3);
  lbl_music_count = make_label(music_list_box, &font_cn_16, COLOR_SUBTLE);
  lv_obj_align(lbl_music_count, LV_ALIGN_TOP_RIGHT, -26, 9);
  for (int i = 0; i < kMusicChips; i++) {
    MusicChipUi &c = music_chips[i];
    c.box = make_box(music_list_box, 40, kMusicChipH, pal->card, LV_OPA_COVER, kMusicChipH / 2);
    c.text = make_label(c.box, &font_cn_16, COLOR_SUBTLE);
    lv_obj_center(c.text);
    lv_obj_add_flag(c.box, LV_OBJ_FLAG_HIDDEN);
  }
  for (int i = 0; i < kMusicRows; i++) {
    MusicRowUi &r = music_rows[i];
    r.row = make_card(music_list_box, 16, kMusicRowTop + i * (kMusicRowH + kMusicRowGap), SCREEN_W - 32, kMusicRowH);
    lv_obj_set_style_radius(r.row, 10, 0);
    lv_obj_clear_flag(r.row, LV_OBJ_FLAG_CLICKABLE);
    r.num = make_label(r.row, &lv_font_montserrat_14, COLOR_SUBTLE);
    lv_obj_set_width(r.num, 28);
    lv_obj_set_style_text_align(r.num, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(r.num, LV_ALIGN_LEFT_MID, 4, 0);
    r.title = make_label(r.row, &font_cn_16, COLOR_TEXT);
    lv_label_set_long_mode(r.title, LV_LABEL_LONG_DOT);
    lv_obj_set_size(r.title, 132, 20);
    lv_obj_align(r.title, LV_ALIGN_LEFT_MID, 36, 0);
    r.artist = make_label(r.row, &font_cn_16, COLOR_SUBTLE);
    lv_label_set_long_mode(r.artist, LV_LABEL_LONG_DOT);
    lv_obj_set_size(r.artist, 68, 20);
    lv_obj_align(r.artist, LV_ALIGN_LEFT_MID, 174, 0);
    r.dur = make_label(r.row, &lv_font_montserrat_14, COLOR_SUBTLE);
    lv_obj_align(r.dur, LV_ALIGN_RIGHT_MID, -10, 0);
    lv_obj_add_flag(r.row, LV_OBJ_FLAG_HIDDEN);
  }
  lbl_music_list_msg = make_label(music_list_box, &font_cn_16, COLOR_SUBTLE);
  lv_obj_set_width(lbl_music_list_msg, SCREEN_W - 40);
  lv_obj_set_style_text_align(lbl_music_list_msg, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(lbl_music_list_msg, LV_ALIGN_CENTER, 0, 20);
}

// 时钟页的迷你播放条：和要闻条、日出日落在同一个位置，放歌时优先显示它
static void build_music_bar() {
  music_bar = make_box(scr_main, SCREEN_W - 24, 24, pal->card, LV_OPA_COVER, 12);
  lv_obj_set_style_border_width(music_bar, 1, 0);
  lv_obj_set_style_border_color(music_bar, lv_color_hex(0xB0743C), 0);
  lv_obj_align(music_bar, LV_ALIGN_TOP_MID, 0, kSunAloneY - 3);
  lv_obj_t *note = make_label(music_bar, &lv_font_montserrat_14, lv_color_hex(kMovieOrange));
  lv_label_set_text(note, LV_SYMBOL_AUDIO);
  lv_obj_align(note, LV_ALIGN_LEFT_MID, 10, 0);
  lbl_music_bar = make_label(music_bar, &font_cn_16, COLOR_TEXT);
  lv_label_set_long_mode(lbl_music_bar, LV_LABEL_LONG_DOT);
  lv_obj_set_size(lbl_music_bar, SCREEN_W - 24 - 32 - 74, 20);
  lv_obj_align(lbl_music_bar, LV_ALIGN_LEFT_MID, 30, 0);
  lbl_music_bar_play = make_label(music_bar, &lv_font_montserrat_14, lv_color_hex(kMovieOrange));
  lv_obj_align(lbl_music_bar_play, LV_ALIGN_RIGHT_MID, -44, 0);
  lv_obj_t *next = make_label(music_bar, &lv_font_montserrat_14, lv_color_hex(kMovieOrange));
  lv_label_set_text(next, LV_SYMBOL_NEXT);
  lv_obj_align(next, LV_ALIGN_RIGHT_MID, -12, 0);
  lv_obj_add_flag(music_bar, LV_OBJ_FLAG_HIDDEN);
}

static void music_time(uint32_t ms, char *out, size_t n) {
  uint32_t s = ms / 1000;
  if (s >= 3600) snprintf(out, n, "%u:%02u:%02u", (unsigned)(s / 3600), (unsigned)(s / 60 % 60), (unsigned)(s % 60));
  else snprintf(out, n, "%u:%02u", (unsigned)(s / 60), (unsigned)(s % 60));
}

void ui_set_music(const MusicPageView &v) {
  const char *head = nullptr, *msg = nullptr;
  switch (v.card) {
    case PlayerCard::NoSlot: msg = "这块板子没有 TF 卡槽"; break;
    case PlayerCard::Reading: msg = "正在读取 TF 卡…"; break;
    case PlayerCard::NoCard:
      head = "没有检测到 TF 卡";
      msg = "插入 FAT32 格式的 TF 卡，把歌放进卡里的 music 文件夹";
      break;
    case PlayerCard::Empty:
      head = "还没有歌";
      msg = "把 MP3、FLAC、WAV 放进 TF 卡的 music 文件夹，或者在网页设置的「音乐管理」里上传";
      break;
    case PlayerCard::Ok: break;
  }
  if (v.loaded) msg = nullptr;  // 正在放的歌优先，读卡时也照常显示
  set_hidden(lbl_music_msg, !msg || music_list_on);
  if (msg && head) {
    lv_label_set_text_fmt(lbl_music_msg, "#%06X %s#\n\n%s", (unsigned)pal->text, head, msg);  // 标题用正文颜色，换主题也看得清
  } else if (msg) {
    lv_label_set_text(lbl_music_msg, msg);
  }
  set_hidden(music_player_box, msg || music_list_on);
  if (msg) return;

  char t[64];
  if (v.folder && v.folder[0]) snprintf(t, sizeof(t), "音乐 · %s", v.folder);
  else snprintf(t, sizeof(t), "音乐 · 全部");
  lv_label_set_text(lbl_music_head, t);
  if (v.queue_len > 0 && v.loaded) lv_label_set_text_fmt(lbl_music_queue, "%d / %d", v.queue_pos + 1, v.queue_len);
  else lv_label_set_text(lbl_music_queue, "");
  if (v.sleep_left_s > 0) {
    snprintf(t, sizeof(t), "%d分后停", (v.sleep_left_s + 59) / 60);
    lv_obj_set_style_bg_color(music_sleep, lv_color_hex(kMovieOrange), 0);
    lv_obj_set_style_text_color(lbl_music_sleep, lv_color_hex(0x1A1A1A), 0);
  } else {
    snprintf(t, sizeof(t), "定时");
    lv_obj_set_style_bg_color(music_sleep, COLOR_FLAT, 0);
    lv_obj_set_style_text_color(lbl_music_sleep, COLOR_SUBTLE, 0);
  }
  lv_label_set_text(lbl_music_sleep, t);
  lv_obj_set_width(music_sleep, v.sleep_left_s > 0 ? 80 : 50);
  lv_obj_center(lbl_music_sleep);
  // 定时按钮挨着右边的「3 / 24」，左边的歌单名让出位置
  lv_obj_align(lbl_music_queue, LV_ALIGN_TOP_RIGHT, -26, 11);
  lv_obj_update_layout(lbl_music_queue);
  int qx = lv_label_get_text(lbl_music_queue)[0] ? lv_obj_get_x(lbl_music_queue) : SCREEN_W - 26;
  int sx = qx - 8 - lv_obj_get_width(music_sleep);
  lv_obj_set_x(music_sleep, sx);
  lv_obj_set_width(lbl_music_head, sx - 36 - 6);

  if (v.loaded) {
    lv_label_set_text(lbl_music_title, v.title);
    lv_label_set_text(lbl_music_artist, v.error ? v.error : v.artist);
    lv_obj_set_style_text_color(lbl_music_artist, v.error ? COLOR_ERR : COLOR_SUBTLE, 0);
    lv_label_set_text(lbl_music_album, v.album);
  } else {
    lv_label_set_text(lbl_music_title, "还没放歌");
    lv_label_set_text(lbl_music_artist, "点播放键开始");
    lv_obj_set_style_text_color(lbl_music_artist, COLOR_SUBTLE, 0);
    lv_label_set_text(lbl_music_album, "");
  }

  for (int i = 0; i < kMusicBars; i++) {
    float h = v.bands && v.playing ? (v.bands[i * 2] + v.bands[i * 2 + 1]) / 2 : 0;
    int px = 2 + (int)(h * (kMusicBarMaxH - 2));
    if (px > kMusicBarMaxH) px = kMusicBarMaxH;
    lv_obj_set_height(music_bars[i], px);
    lv_obj_set_y(music_bars[i], kMusicBarsBase - px);
  }

  if (v.lyric_cur) {
    lv_label_set_text(lbl_music_lyric, v.lyric_cur[0] ? v.lyric_cur : "· · ·");
    lv_label_set_text(lbl_music_lyric_next, v.lyric_next ? v.lyric_next : "");
  } else {
    lv_label_set_text(lbl_music_lyric, "");
    lv_label_set_text(lbl_music_lyric_next, v.loaded ? (v.loading ? "正在打开…" : "没有歌词") : "");
  }

  uint32_t pos = v.pos_ms > v.dur_ms && v.dur_ms ? v.dur_ms : v.pos_ms;
  int w = v.dur_ms ? (int)((uint64_t)kMusicProgW * pos / v.dur_ms) : 0;
  lv_obj_set_width(music_prog_fill, w);
  lv_obj_set_x(music_knob, kMusicProgX + w - 6);
  music_time(pos, t, sizeof(t));
  lv_label_set_text(lbl_music_pos, t);
  if (v.dur_ms) music_time(v.dur_ms, t, sizeof(t));
  else snprintf(t, sizeof(t), "--:--");
  lv_label_set_text(lbl_music_dur, t);
  lv_obj_align(lbl_music_dur, LV_ALIGN_TOP_RIGHT, -16, kMusicProgY - 7);

  lv_label_set_text(lbl_music_mode, v.mode == PlayMode::Shuffle ? LV_SYMBOL_SHUFFLE : LV_SYMBOL_LOOP);
  set_hidden(lbl_music_mode_one, v.mode != PlayMode::One);
  lv_label_set_text(lbl_music_play, v.playing ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
}

void ui_set_music_cover(const uint16_t *px) {
  if (px && music_cover_px) {
    memcpy(music_cover_px, px, kPlayerCover * kPlayerCover * 2);
    lv_img_cache_invalidate_src(&music_cover_dsc);
    lv_img_set_src(music_cover_img, &music_cover_dsc);
    lv_obj_invalidate(music_cover_img);
  }
  bool show = px && music_cover_px;
  set_hidden(music_cover_img, !show);
  set_hidden(music_cover_art, show);
}

void ui_music_show_list(bool on) {
  music_list_on = on;
  set_hidden(music_list_box, !on);
  if (on) {
    lv_obj_add_flag(music_player_box, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(lbl_music_msg, LV_OBJ_FLAG_HIDDEN);
  }
}

bool ui_music_list_shown() { return music_list_on; }

void ui_set_music_list(const char *msg, const char *const *chips, int nchips, int sel, int first, const MusicRow *rows,
                       int n, int count, int page, int pages) {
  // 歌单按钮从左往右排，放不下时最后放一个「›」
  int x = 16;
  int shown = 0;
  for (int i = 0; i < kMusicChips; i++) music_chips[i].index = -2;
  for (int k = first; k < nchips && shown < kMusicChips - 1; k++) {  // 留一个给「›」
    MusicChipUi &c = music_chips[shown];
    lv_label_set_long_mode(c.text, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(c.text, LV_SIZE_CONTENT);
    lv_label_set_text(c.text, chips[k]);
    lv_obj_update_layout(c.text);
    int w = lv_obj_get_width(c.text) + 24;
    if (w > 120) {
      w = 120;
      lv_label_set_long_mode(c.text, LV_LABEL_LONG_DOT);
      lv_obj_set_width(c.text, w - 16);
    }
    bool last = k == nchips - 1;
    if (x + w > SCREEN_W - 16 - (last ? 0 : 34) && shown > 0) break;
    lv_obj_set_size(c.box, w, kMusicChipH);
    lv_obj_set_pos(c.box, x, kMusicChipY);
    lv_obj_center(c.text);
    bool on = k == sel;
    lv_obj_set_style_bg_color(c.box, on ? lv_color_hex(kMovieOrange) : COLOR_CARD, 0);
    lv_obj_set_style_text_color(c.text, on ? lv_color_hex(0x1A1A1A) : COLOR_SUBTLE, 0);
    lv_obj_clear_flag(c.box, LV_OBJ_FLAG_HIDDEN);
    c.index = k;
    x += w + 6;
    shown++;
  }
  bool more = first > 0 || first + shown < nchips;
  music_chip_next = first + shown < nchips ? first + shown : 0;
  if (more && shown < kMusicChips) {
    MusicChipUi &c = music_chips[shown];
    lv_label_set_long_mode(c.text, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(c.text, LV_SIZE_CONTENT);
    lv_label_set_text(c.text, "›");  // 点了往后挪，到头了回到最前面
    lv_obj_set_size(c.box, 28, kMusicChipH);
    lv_obj_set_pos(c.box, SCREEN_W - 16 - 28, kMusicChipY);
    lv_obj_center(c.text);
    lv_obj_set_style_bg_color(c.box, COLOR_CARD, 0);
    lv_obj_set_style_text_color(c.text, COLOR_SUBTLE, 0);
    lv_obj_clear_flag(c.box, LV_OBJ_FLAG_HIDDEN);
    c.index = -1;
    shown++;
  }
  for (int i = shown; i < kMusicChips; i++) lv_obj_add_flag(music_chips[i].box, LV_OBJ_FLAG_HIDDEN);

  set_hidden(lbl_music_list_msg, !msg);
  if (msg) lv_label_set_text(lbl_music_list_msg, msg);
  for (int i = 0; i < kMusicRows; i++) {
    MusicRowUi &r = music_rows[i];
    set_hidden(r.row, msg || i >= n);
    if (msg || i >= n) continue;
    const MusicRow &m = rows[i];
    if (m.playing) lv_label_set_text(r.num, LV_SYMBOL_AUDIO);
    else lv_label_set_text_fmt(r.num, "%d", m.num);
    lv_obj_set_style_text_color(r.num, m.playing ? lv_color_hex(kMovieOrange) : COLOR_SUBTLE, 0);
    lv_label_set_text(r.title, m.title);
    lv_obj_set_style_text_color(r.title, m.playing ? lv_color_hex(kMovieOrange) : COLOR_TEXT, 0);
    lv_label_set_text(r.artist, m.artist);
    char t[16] = "";
    if (m.dur_ms) music_time(m.dur_ms, t, sizeof(t));
    lv_label_set_text(r.dur, t);
  }
  char t[32];
  if (pages > 1) snprintf(t, sizeof(t), "%d 首 · %d/%d", count, page + 1, pages);
  else snprintf(t, sizeof(t), "%d 首", count);
  lv_label_set_text(lbl_music_count, msg ? "" : t);
  lv_obj_align(lbl_music_count, LV_ALIGN_TOP_RIGHT, -26, 9);
}

MusicHit ui_music_hit(int x, int y, float *frac, int *index) {
  if (music_list_on) {
    if (y < kMusicChipY - 2) return x < 120 ? MusicHit::Back : MusicHit::None;
    if (y < kMusicChipY + kMusicChipH + 3) {
      for (const MusicChipUi &c : music_chips) {
        if (c.index < -1 || lv_obj_has_flag(c.box, LV_OBJ_FLAG_HIDDEN)) continue;
        int cx = lv_obj_get_x(c.box), cw = lv_obj_get_width(c.box);
        if (x >= cx - 3 && x < cx + cw + 3) {
          if (index) *index = c.index < 0 ? music_chip_next : c.index;
          return c.index < 0 ? MusicHit::ChipMore : MusicHit::Chip;
        }
      }
      return MusicHit::None;
    }
    if (x < 16 || x >= SCREEN_W - 16) return MusicHit::None;
    for (int i = 0; i < kMusicRows; i++) {
      int top = kMusicRowTop + i * (kMusicRowH + kMusicRowGap);
      if (y >= top - 2 && y < top + kMusicRowH + 2 && !lv_obj_has_flag(music_rows[i].row, LV_OBJ_FLAG_HIDDEN)) {
        if (index) *index = i;
        return MusicHit::Row;
      }
    }
    return MusicHit::None;
  }
  if (lv_obj_has_flag(music_player_box, LV_OBJ_FLAG_HIDDEN)) return MusicHit::None;
  if (y < 32) {
    int sx = lv_obj_get_x(music_sleep);
    if (x >= sx - 6 && x < sx + lv_obj_get_width(music_sleep) + 6) return MusicHit::Sleep;
    if (x < sx - 6) return MusicHit::List;
    return MusicHit::None;
  }
  if (y >= kMusicProgY - 14 && y < kMusicProgY + 12 && x >= kMusicProgX - 12 && x < kMusicProgX + kMusicProgW + 12) {
    if (frac) {
      float f = (float)(x - kMusicProgX) / kMusicProgW;
      *frac = f < 0 ? 0 : f > 1 ? 1 : f;
    }
    return MusicHit::Progress;
  }
  if (y >= kMusicBtnY - 26) {
    static const MusicHit kBtns[5] = {MusicHit::Mode, MusicHit::Prev, MusicHit::PlayPause, MusicHit::Next,
                                      MusicHit::List};
    for (int i = 0; i < 5; i++) {
      if (abs(x - kMusicBtnX[i]) <= 28) return kBtns[i];
    }
  }
  return MusicHit::None;
}

void ui_set_music_bar(bool show, bool playing, const char *title, const char *artist) {
  if (show) {
    char t[2 * kTagMax + 8];
    if (artist && artist[0]) snprintf(t, sizeof(t), "%s · %s", title, artist);
    else snprintf(t, sizeof(t), "%s", title);
    lv_label_set_text(lbl_music_bar, t);
    lv_label_set_text(lbl_music_bar_play, playing ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
  }
  if (show == music_bar_shown) return;
  music_bar_shown = show;
  set_hidden(music_bar, !show);
  if (show) {
    lv_obj_add_flag(sun_row, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(ticker_row, LV_OBJ_FLAG_HIDDEN);
  } else if (ticker_shown) {
    lv_obj_clear_flag(ticker_row, LV_OBJ_FLAG_HIDDEN);
  } else {
    update_sun_row();
  }
}

MusicBarHit ui_music_bar_hit(int x, int y) {
  if (!music_bar_shown) return MusicBarHit::None;
  lv_area_t a;
  lv_obj_get_coords(music_bar, &a);
  if (y < a.y1 - 8 || y > a.y2 + 8 || x < a.x1 || x > a.x2) return MusicBarHit::None;
  if (x >= a.x2 - 30) return MusicBarHit::Next;
  if (x >= a.x2 - 70) return MusicBarHit::PlayPause;
  return MusicBarHit::Open;
}

static void build_alert() {
  scr_alert = make_screen();

  alert_badge = lv_obj_create(scr_alert);
  lv_obj_set_size(alert_badge, 56, 56);
  lv_obj_set_style_radius(alert_badge, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_border_width(alert_badge, 0, 0);
  lv_obj_set_style_pad_all(alert_badge, 0, 0);
  lv_obj_clear_flag(alert_badge, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_align(alert_badge, LV_ALIGN_TOP_MID, 0, 18);

  alert_icon = make_label(alert_badge, &lv_font_montserrat_20, lv_color_hex(0xFFFFFF));
  lv_obj_center(alert_icon);

  alert_title = make_label(scr_alert, &font_cn_24, COLOR_TEXT);
  lv_obj_align(alert_title, LV_ALIGN_TOP_MID, 0, 84);

  alert_body = make_label(scr_alert, &font_cn_16, COLOR_TEXT);
  lv_obj_set_width(alert_body, SCREEN_W - 40);
  lv_label_set_long_mode(alert_body, LV_LABEL_LONG_WRAP);
  lv_obj_set_style_text_align(alert_body, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_style_text_line_space(alert_body, 4, 0);
  lv_obj_align(alert_body, LV_ALIGN_TOP_MID, 0, 120);  // 最多 4 行，留言限 60 个字

  alert_foot = make_label(scr_alert, &font_cn_16, COLOR_SUBTLE);
  lv_obj_align(alert_foot, LV_ALIGN_BOTTOM_MID, 0, -8);
}

// 圆角按钮，里面一个符号
static lv_obj_t *make_button(lv_obj_t *parent, const char *symbol) {
  lv_obj_t *b = lv_obj_create(parent);
  lv_obj_set_size(b, 44, 36);
  lv_obj_set_style_radius(b, 8, 0);
  lv_obj_set_style_border_width(b, 0, 0);
  lv_obj_set_style_pad_all(b, 0, 0);
  lv_obj_set_style_bg_color(b, COLOR_FLAT, 0);
  lv_obj_clear_flag(b, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_t *l = make_label(b, &lv_font_montserrat_20, COLOR_TEXT);
  lv_label_set_text(l, symbol);
  lv_obj_center(l);
  return b;
}

// 开关：打开时绿色、圆点在右边
static lv_obj_t *make_toggle(lv_obj_t *parent) {
  lv_obj_t *t = lv_obj_create(parent);
  lv_obj_set_size(t, 56, 30);
  lv_obj_set_style_radius(t, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_border_width(t, 0, 0);
  lv_obj_set_style_pad_all(t, 0, 0);
  lv_obj_clear_flag(t, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_t *knob = lv_obj_create(t);
  lv_obj_set_size(knob, 24, 24);
  lv_obj_set_style_radius(knob, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_border_width(knob, 0, 0);
  lv_obj_set_style_bg_color(knob, lv_color_hex(0xFFFFFF), 0);
  lv_obj_set_style_pad_all(knob, 0, 0);
  lv_obj_clear_flag(knob, LV_OBJ_FLAG_SCROLLABLE);
  return t;
}

static void set_toggle(lv_obj_t *t, bool on, bool enabled = true) {
  lv_obj_set_style_bg_color(t, on && enabled ? COLOR_OK : COLOR_FLAT, 0);
  lv_obj_set_style_opa(t, enabled ? LV_OPA_COVER : LV_OPA_40, 0);
  lv_obj_align(lv_obj_get_child(t, 0), on ? LV_ALIGN_RIGHT_MID : LV_ALIGN_LEFT_MID, on ? -3 : 3, 0);
}

static void build_quick() {
  scr_quick = make_screen();

  quick_title = make_label(scr_quick, &font_cn_16, COLOR_SUBTLE);
  lv_label_set_text(quick_title, "快捷设置");
  lv_obj_align(quick_title, LV_ALIGN_TOP_LEFT, 12, 10);
  static const char *const kMusicSyms[3] = {LV_SYMBOL_PREV, LV_SYMBOL_PLAY, LV_SYMBOL_NEXT};
  for (int i = 0; i < 3; i++) {
    quick_music[i] = make_button(scr_quick, kMusicSyms[i]);
    lv_obj_align(quick_music[i], LV_ALIGN_TOP_LEFT, 8 + i * 48, 4);
    lv_obj_add_flag(quick_music[i], LV_OBJ_FLAG_HIDDEN);
  }
  lv_obj_set_style_bg_color(quick_music[1], lv_color_hex(kMovieOrange), 0);
  lv_obj_set_style_text_color(lv_obj_get_child(quick_music[1], 0), lv_color_hex(0x1A1A1A), 0);

  quick_close = make_button(scr_quick, LV_SYMBOL_CLOSE);
  lv_obj_align(quick_close, LV_ALIGN_TOP_RIGHT, -8, 4);
  quick_pages = make_button(scr_quick, "");
  lv_obj_set_width(quick_pages, 96);
  lv_obj_align(quick_pages, LV_ALIGN_TOP_RIGHT, -60, 4);
  lv_obj_t *pl = lv_obj_get_child(quick_pages, 0);
  lv_obj_set_style_text_font(pl, &font_cn_16, 0);
  lv_label_set_text(pl, "显示页面 ›");

  // 左边：二维码
  // 二维码外面留一圈白边，手机才扫得出来
  lv_obj_t *qr_box = lv_obj_create(scr_quick);
  lv_obj_set_size(qr_box, 124, 124);
  lv_obj_set_style_radius(qr_box, 4, 0);
  lv_obj_set_style_border_width(qr_box, 0, 0);
  lv_obj_set_style_pad_all(qr_box, 0, 0);
  lv_obj_set_style_bg_color(qr_box, lv_color_hex(0xFFFFFF), 0);
  lv_obj_clear_flag(qr_box, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_align(qr_box, LV_ALIGN_TOP_LEFT, 12, 52);
  quick_qr = lv_qrcode_create(qr_box, 112, lv_color_hex(0x000000), lv_color_hex(0xFFFFFF));
  lv_obj_center(quick_qr);
  lbl_quick_url = make_label(scr_quick, &font_cn_16, COLOR_SUBTLE);
  lv_obj_set_width(lbl_quick_url, 140);
  lv_obj_set_style_text_align(lbl_quick_url, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(lbl_quick_url, LV_ALIGN_TOP_LEFT, 4, 186);

  // 右边：四行设置，亮度和音量是「− 数值 +」
  const int x = 158;
  lv_obj_t *l;
  auto stepper = [&](const char *name, int y, lv_obj_t **minus, lv_obj_t **value, lv_obj_t **plus) {
    l = make_label(scr_quick, &font_cn_16, COLOR_TEXT);
    lv_label_set_text(l, name);
    lv_obj_align(l, LV_ALIGN_TOP_LEFT, x, y + 7);
    *minus = make_button(scr_quick, LV_SYMBOL_MINUS);
    lv_obj_set_size(*minus, 34, 32);
    lv_obj_align(*minus, LV_ALIGN_TOP_LEFT, x + 36, y);
    *value = make_label(scr_quick, &lv_font_montserrat_20, COLOR_TEXT);
    lv_obj_set_width(*value, 48);
    lv_obj_set_style_text_align(*value, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(*value, LV_ALIGN_TOP_LEFT, x + 70, y + 6);
    *plus = make_button(scr_quick, LV_SYMBOL_PLUS);
    lv_obj_set_size(*plus, 34, 32);
    lv_obj_align(*plus, LV_ALIGN_TOP_LEFT, x + 118, y);
  };
  stepper("亮度", 44, &quick_minus, &lbl_quick_bright, &quick_plus);
  stepper("音量", 88, &quick_vol_minus, &lbl_quick_volume, &quick_vol_plus);

  l = make_label(scr_quick, &font_cn_16, COLOR_TEXT);
  lv_label_set_text(l, "提示音");
  lv_obj_align(l, LV_ALIGN_TOP_LEFT, x, 140);
  quick_sound = make_toggle(scr_quick);
  lv_obj_align(quick_sound, LV_ALIGN_TOP_RIGHT, -10, 134);

  lbl_quick_away = make_label(scr_quick, &font_cn_16, COLOR_TEXT);
  lv_obj_align(lbl_quick_away, LV_ALIGN_TOP_LEFT, x, 190);
  quick_away = make_toggle(scr_quick);
  lv_obj_align(quick_away, LV_ALIGN_TOP_RIGHT, -10, 184);
}

static void build_qpages() {
  scr_qpages = make_screen();
  qp_back = make_button(scr_qpages, LV_SYMBOL_LEFT);
  lv_obj_align(qp_back, LV_ALIGN_TOP_LEFT, 8, 4);
  lv_obj_t *title = make_label(scr_qpages, &font_cn_16, COLOR_TEXT);
  lv_label_set_text(title, "显示哪些页面");
  lv_obj_align(title, LV_ALIGN_TOP_LEFT, 62, 13);
  lbl_qp_count = make_label(scr_qpages, &font_cn_16, COLOR_SUBTLE);
  lv_obj_align(lbl_qp_count, LV_ALIGN_TOP_RIGHT, -12, 13);

  // 滚动由 main.cpp 的触摸代码通过 ui_quick_drag 驱动（没有接 LVGL 的输入设备）
  qp_list = lv_obj_create(scr_qpages);
  lv_obj_set_size(qp_list, SCREEN_W, SCREEN_H - kTilesTop);
  lv_obj_set_pos(qp_list, 0, kTilesTop);
  lv_obj_set_style_bg_opa(qp_list, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(qp_list, 0, 0);
  lv_obj_set_style_radius(qp_list, 0, 0);
  lv_obj_set_style_pad_all(qp_list, 0, 0);
  lv_obj_set_style_pad_bottom(qp_list, 8, 0);
  lv_obj_set_scroll_dir(qp_list, LV_DIR_VER);
  lv_obj_set_scrollbar_mode(qp_list, LV_SCROLLBAR_MODE_AUTO);
  lv_obj_set_style_width(qp_list, 3, LV_PART_SCROLLBAR);
  lv_obj_set_style_pad_right(qp_list, 1, LV_PART_SCROLLBAR);
  lv_obj_set_style_bg_color(qp_list, COLOR_SUBTLE, LV_PART_SCROLLBAR);
  lv_obj_set_style_bg_opa(qp_list, LV_OPA_60, LV_PART_SCROLLBAR);

  for (int p = 0; p < PAGE_COUNT; p++) {
    PageTile &t = qp_tiles[p];
    t.box = lv_obj_create(qp_list);
    lv_obj_set_size(t.box, kTileW, kTileH);
    lv_obj_set_style_radius(t.box, 10, 0);
    lv_obj_set_style_pad_all(t.box, 0, 0);
    lv_obj_clear_flag(t.box, LV_OBJ_FLAG_SCROLLABLE);
    t.name = make_label(t.box, &font_cn_16, COLOR_TEXT);
    lv_label_set_text(t.name, kPageInfo[p].name);
    t.hint = make_label(t.box, &font_cn_16, COLOR_SUBTLE);
    lv_obj_align(t.hint, LV_ALIGN_TOP_LEFT, 10, 29);
    t.check = lv_obj_create(t.box);
    lv_obj_set_size(t.check, 20, 20);
    lv_obj_set_style_radius(t.check, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_pad_all(t.check, 0, 0);
    lv_obj_clear_flag(t.check, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(t.check, LV_ALIGN_TOP_RIGHT, -6, 6);
    t.mark = make_label(t.check, &lv_font_montserrat_14, lv_color_hex(0xFFFFFF));
    lv_label_set_text(t.mark, LV_SYMBOL_OK);
    lv_obj_center(t.mark);
  }
}

// 打开的格子：卡片底色、蓝边、蓝色勾；关掉的：透明底、灰边、空心圈、灰字。时钟页一直开着，勾是灰的
static void show_qpages(const QuickState &q) {
  int shown = 0, total = 0;
  for (int p = 0; p < PAGE_COUNT; p++) {
    PageTile &t = qp_tiles[p];
    if (q.page_unavailable & (1u << p)) {
      lv_obj_add_flag(t.box, LV_OBJ_FLAG_HIDDEN);
      continue;
    }
    lv_obj_clear_flag(t.box, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_pos(t.box, 8 + (total % 3) * (kTileW + kTileGapX), (total / 3) * (kTileH + kTileGapY));
    total++;
    bool locked = p == PAGE_CLOCK;
    bool on = locked || !(q.page_hidden & (1u << p));
    if (on) shown++;
    const char *hint = locked ? "始终显示" : (q.page_unset & (1u << p)) ? "未设置" : "";
    lv_label_set_text(t.hint, hint);
    lv_obj_align(t.name, LV_ALIGN_TOP_LEFT, 10, hint[0] ? 7 : 17);
    lv_obj_set_style_bg_color(t.box, on ? COLOR_CARD : COLOR_BG, 0);
    lv_obj_set_style_border_width(t.box, on ? 2 : 1, 0);
    lv_obj_set_style_border_color(t.box, on ? COLOR_ACCENT : COLOR_FLAT, 0);
    lv_obj_set_style_text_color(t.name, on ? COLOR_TEXT : COLOR_SUBTLE, 0);
    lv_obj_set_style_bg_color(t.check, !on ? COLOR_BG : locked ? COLOR_FLAT : COLOR_ACCENT, 0);
    lv_obj_set_style_border_width(t.check, on ? 0 : 2, 0);
    lv_obj_set_style_border_color(t.check, COLOR_FLAT, 0);
    lv_obj_set_style_text_color(t.mark, locked ? COLOR_SUBTLE : lv_color_hex(0xFFFFFF), 0);
    if (on) {
      lv_obj_clear_flag(t.mark, LV_OBJ_FLAG_HIDDEN);
    } else {
      lv_obj_add_flag(t.mark, LV_OBJ_FLAG_HIDDEN);
    }
  }
  lv_label_set_text_fmt(lbl_qp_count, "%d/%d", shown, total);
  if (lv_scr_act() != scr_qpages) {
    qp_drag_base = -1;
    lv_obj_scroll_to_y(qp_list, 0, LV_ANIM_OFF);
    screen_load(scr_qpages, false);
  }
}

static int qp_max_scroll() {
  lv_obj_update_layout(qp_list);
  int m = lv_obj_get_scroll_y(qp_list) + lv_obj_get_scroll_bottom(qp_list);
  return m > 0 ? m : 0;
}

void ui_quick_drag(int dy) {
  if (lv_scr_act() != scr_qpages) return;
  if (qp_drag_base < 0) qp_drag_base = lv_obj_get_scroll_y(qp_list);
  int y = qp_drag_base + dy;
  int m = qp_max_scroll();
  lv_obj_scroll_to_y(qp_list, y < 0 ? 0 : y > m ? m : y, LV_ANIM_OFF);
}

void ui_quick_drag_end() {
  qp_drag_base = -1;
  if (lv_scr_act() != scr_qpages) return;
  const int row = kTileH + kTileGapY;
  int y = (lv_obj_get_scroll_y(qp_list) + row / 2) / row * row;
  int m = qp_max_scroll();
  lv_obj_scroll_to_y(qp_list, y > m ? m : y, LV_ANIM_ON);
}

void ui_show_quick(const QuickState &q) {
  if (q.pages_view) {
    show_qpages(q);
    return;
  }
  set_hidden(quick_title, q.music);
  for (lv_obj_t *b : quick_music) set_hidden(b, !q.music);
  lv_label_set_text(lv_obj_get_child(quick_music[1], 0), q.music_playing ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
  lv_label_set_text_fmt(lbl_quick_bright, "%d%%", (q.brightness * 100 + 127) / 255);
  lv_label_set_text_fmt(lbl_quick_volume, "%d", q.volume);
  set_toggle(quick_sound, q.sound_on);
  set_toggle(quick_away, q.away_off, q.has_dock);
  lv_label_set_text(lbl_quick_away, q.has_dock ? "没人熄屏" : "没人熄屏\n#8B949E 未插底座#");
  lv_label_set_recolor(lbl_quick_away, true);
  if (strcmp(quick_url.c_str(), q.url) != 0) {
    quick_url = q.url;
    if (quick_url.length()) {
      lv_qrcode_update(quick_qr, quick_url.c_str(), quick_url.length());
      lv_obj_clear_flag(lv_obj_get_parent(quick_qr), LV_OBJ_FLAG_HIDDEN);
      lv_label_set_text_fmt(lbl_quick_url, "扫码打开完整设置\n%s", quick_url.c_str() + 7);  // 去掉 http://
    } else {
      lv_obj_add_flag(lv_obj_get_parent(quick_qr), LV_OBJ_FLAG_HIDDEN);
      lv_label_set_text(lbl_quick_url, "还没连上 WiFi");
    }
  }
  if (lv_scr_act() != scr_quick) screen_load(scr_quick, false);
}

// 按钮四周各放宽 8 像素
static bool hit(lv_obj_t *o, int x, int y) {
  lv_area_t a;
  lv_obj_get_coords(o, &a);
  return x >= a.x1 - 8 && x <= a.x2 + 8 && y >= a.y1 - 8 && y <= a.y2 + 8;
}

QuickHit ui_quick_hit(int x, int y, Page *page) {
  if (lv_scr_act() == scr_qpages) {
    lv_obj_update_layout(scr_qpages);
    if (hit(qp_back, x, y)) return QuickHit::Back;
    if (y < kTilesTop) return QuickHit::None;  // 滚到上面去的格子被标题栏挡住了
    for (int p = 0; p < PAGE_COUNT; p++) {
      if (lv_obj_has_flag(qp_tiles[p].box, LV_OBJ_FLAG_HIDDEN)) continue;
      lv_area_t a;
      lv_obj_get_coords(qp_tiles[p].box, &a);
      if (x >= a.x1 - 3 && x <= a.x2 + 3 && y >= a.y1 - 3 && y <= a.y2 + 3) {
        if (page) *page = (Page)p;
        return QuickHit::PageTile;
      }
    }
    return QuickHit::None;
  }
  lv_obj_update_layout(scr_quick);
  if (hit(quick_close, x, y)) return QuickHit::Close;
  if (!lv_obj_has_flag(quick_music[0], LV_OBJ_FLAG_HIDDEN)) {
    // 三个按钮挨着，只往上下放宽
    static const QuickHit kMusic[3] = {QuickHit::MusicPrev, QuickHit::MusicPlay, QuickHit::MusicNext};
    for (int i = 0; i < 3; i++) {
      lv_area_t a;
      lv_obj_get_coords(quick_music[i], &a);
      if (x >= a.x1 - 2 && x <= a.x2 + 2 && y <= a.y2 + 4) return kMusic[i];
    }
  }
  // 「显示页面」按钮和下面的亮度按钮挨得近，不放宽
  lv_area_t pa;
  lv_obj_get_coords(quick_pages, &pa);
  if (x >= pa.x1 && x <= pa.x2 && y >= pa.y1 - 4 && y <= pa.y2 + 2) return QuickHit::Pages;
  if (hit(quick_minus, x, y)) return QuickHit::BrightDown;
  if (hit(quick_plus, x, y)) return QuickHit::BrightUp;
  if (hit(quick_vol_minus, x, y)) return QuickHit::VolumeDown;
  if (hit(quick_vol_plus, x, y)) return QuickHit::VolumeUp;
  // 开关那一整行都算
  lv_area_t a;
  lv_obj_get_coords(quick_sound, &a);
  if (x >= 150 && y >= a.y1 - 12 && y <= a.y2 + 12) return QuickHit::Sound;
  lv_obj_get_coords(quick_away, &a);
  if (x >= 150 && y >= a.y1 - 12 && y <= a.y2 + 12) return QuickHit::Away;
  return QuickHit::None;
}

static void build_game() {
  scr_game = make_screen();

  lbl_game_best = make_label(scr_game, &font_cn_16, COLOR_SUBTLE);
  lv_obj_align(lbl_game_best, LV_ALIGN_TOP_LEFT, 12, 10);
  lbl_game_score = make_label(scr_game, &lv_font_montserrat_20, COLOR_TEXT);
  lv_obj_align(lbl_game_score, LV_ALIGN_TOP_MID, 0, 8);
  game_close = make_button(scr_game, LV_SYMBOL_CLOSE);
  lv_obj_align(game_close, LV_ALIGN_TOP_RIGHT, -8, 4);

  // 地面和往后跑的小石子
  lv_obj_t *ground = lv_obj_create(scr_game);
  lv_obj_set_size(ground, SCREEN_W, 2);
  lv_obj_set_style_border_width(ground, 0, 0);
  lv_obj_set_style_radius(ground, 0, 0);
  lv_obj_set_style_bg_color(ground, COLOR_SUBTLE, 0);
  lv_obj_set_pos(ground, 0, RunnerGame::kGroundY);
  for (int i = 0; i < kPebbles; i++) {
    lv_obj_t *p = lv_obj_create(scr_game);
    lv_obj_set_size(p, i % 3 ? 3 : 6, 2);
    lv_obj_set_style_border_width(p, 0, 0);
    lv_obj_set_style_radius(p, 0, 0);
    lv_obj_set_style_bg_color(p, COLOR_FLAT, 0);
    game_pebbles[i] = p;
  }

  // 黄瓜
  for (auto &o : game_obs) {
    o = lv_obj_create(scr_game);
    lv_obj_set_style_radius(o, 6, 0);
    lv_obj_set_style_bg_color(o, lv_color_hex(0x3FB950), 0);
    lv_obj_set_style_border_color(o, lv_color_hex(0x1A7F37), 0);
    lv_obj_set_style_border_width(o, 2, 0);
    lv_obj_set_style_pad_all(o, 0, 0);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
  }

  // 小橘：16x16 像素画放大 2 倍
  game_cat_buf = (lv_color_t *)lv_mem_alloc(LV_CANVAS_BUF_SIZE_TRUE_COLOR(RunnerGame::kCatSize, RunnerGame::kCatSize));
  game_cat = lv_canvas_create(scr_game);
  if (game_cat_buf) {
    lv_canvas_set_buffer(game_cat, game_cat_buf, RunnerGame::kCatSize, RunnerGame::kCatSize, LV_IMG_CF_TRUE_COLOR);
  }

  lbl_game_msg = make_label(scr_game, &font_cn_16, COLOR_TEXT);
  lv_obj_set_style_text_align(lbl_game_msg, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(lbl_game_msg, LV_ALIGN_TOP_MID, 0, 70);
}

static void draw_game_cat(PetMood mood) {
  static int last = -1;
  static uint8_t epoch;
  if (!game_cat_buf || ((int)mood == last && epoch == theme_epoch)) return;
  last = (int)mood;
  epoch = theme_epoch;
  const int scale = RunnerGame::kCatSize / kPetSize;
  for (int y = 0; y < RunnerGame::kCatSize; y++) {
    for (int x = 0; x < RunnerGame::kCatSize; x++) {
      game_cat_buf[y * RunnerGame::kCatSize + x] = pet_color(pet_pixel(mood, false, x / scale, y / scale));
    }
  }
  lv_obj_invalidate(game_cat);
}

void ui_show_game() {
  if (lv_scr_act() != scr_game) screen_load(scr_game, false);
}

void ui_set_game(const RunnerGame &g, int best) {
  using S = RunnerGame::State;
  draw_game_cat(g.state() == S::Over ? PetMood::Startled : PetMood::Happy);
  lv_obj_set_pos(game_cat, RunnerGame::kCatX, (int)g.cat_y());

  for (int i = 0; i < RunnerGame::kMaxObstacles; i++) {
    const auto &o = g.obstacle(i);
    if (!o.active) {
      lv_obj_add_flag(game_obs[i], LV_OBJ_FLAG_HIDDEN);
      continue;
    }
    lv_obj_set_size(game_obs[i], o.w, o.h);
    lv_obj_set_pos(game_obs[i], (int)o.x, RunnerGame::kGroundY - o.h + 1);
    lv_obj_clear_flag(game_obs[i], LV_OBJ_FLAG_HIDDEN);
  }

  // 石子跟着地面往后退，看得出速度
  int offset = (int)g.distance() % SCREEN_W;
  for (int i = 0; i < kPebbles; i++) {
    int x = (i * 47 + SCREEN_W - offset) % SCREEN_W;
    lv_obj_set_pos(game_pebbles[i], x, RunnerGame::kGroundY + 8 + (i * 7) % 20);
  }

  lv_label_set_text_fmt(lbl_game_score, "%d", g.score());
  lv_label_set_text_fmt(lbl_game_best, "最高 %d", best);
  switch (g.state()) {
    case S::Ready:
      lv_label_set_text(lbl_game_msg, "小橘跑酷\n点屏幕起跳，躲开黄瓜！\n点一下开始");
      lv_obj_clear_flag(lbl_game_msg, LV_OBJ_FLAG_HIDDEN);
      break;
    case S::Over:
      if (g.score() >= best && g.score() > 0) {
        lv_label_set_text_fmt(lbl_game_msg, "新纪录！%d\n点屏幕再来一局", g.score());
      } else {
        lv_label_set_text(lbl_game_msg, "撞到黄瓜了！\n点屏幕再来一局");
      }
      lv_obj_clear_flag(lbl_game_msg, LV_OBJ_FLAG_HIDDEN);
      break;
    case S::Playing:
      lv_obj_add_flag(lbl_game_msg, LV_OBJ_FLAG_HIDDEN);
      break;
  }
  lv_obj_align(lbl_game_msg, LV_ALIGN_TOP_MID, 0, 70);
}

bool ui_game_close_hit(int x, int y) {
  lv_obj_update_layout(scr_game);
  return hit(game_close, x, y);
}

bool ui_pet_hit(int x, int y) {
  lv_area_t a;
  lv_obj_get_coords(pet_canvas, &a);
  return x >= a.x1 - 10 && x <= a.x2 + 10 && y >= a.y1 - 10 && y <= a.y2 + 10;
}

// 聊天页气泡区域和气泡尺寸
static constexpr int kChatAreaX = 66;
static constexpr int kChatAreaY = 32;
static constexpr int kChatAreaW = SCREEN_W - kChatAreaX - 8;
static constexpr int kChatAreaH = 116;
static constexpr int kChatMeW = 200;  // 主人的气泡最宽这么多，左边留出空白，一眼分得清谁说的
static constexpr int kBubblePadX = 8;
static constexpr int kBubblePadY = 5;
static constexpr int kBubbleGap = 6;
static constexpr int kBubbleLineSpace = 2;

static void build_chat() {
  lv_obj_t *scr = make_screen();
  scr_pages[PAGE_CHAT] = scr;

  lv_obj_t *title = make_label(scr, &font_cn_16, COLOR_SUBTLE);
  lv_label_set_text(title, "和小橘聊天");
  lv_obj_align(title, LV_ALIGN_TOP_LEFT, 12, 10);
  dot_wifi[PAGE_CHAT] = make_wifi_dot(scr);

  chat_cat_buf = (lv_color_t *)lv_mem_alloc(LV_CANVAS_BUF_SIZE_TRUE_COLOR(kChatCat, kChatCat));
  chat_cat = lv_canvas_create(scr);
  if (chat_cat_buf) lv_canvas_set_buffer(chat_cat, chat_cat_buf, kChatCat, kChatCat, LV_IMG_CF_TRUE_COLOR);
  lv_obj_set_pos(chat_cat, 12, 60);

  // 对话气泡：区域外的部分裁掉，位置在 ui_set_chat 里按内容排
  chat_area = lv_obj_create(scr);
  lv_obj_set_size(chat_area, kChatAreaW, kChatAreaH);
  lv_obj_set_pos(chat_area, kChatAreaX, kChatAreaY);
  lv_obj_set_style_bg_opa(chat_area, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(chat_area, 0, 0);
  lv_obj_set_style_pad_all(chat_area, 0, 0);
  lv_obj_set_style_radius(chat_area, 0, 0);
  lv_obj_clear_flag(chat_area, LV_OBJ_FLAG_SCROLLABLE);
  for (int i = 0; i < kChatBubbles; i++) {
    lv_obj_t *b = lv_obj_create(chat_area);
    lv_obj_set_style_radius(b, 10, 0);
    lv_obj_set_style_border_width(b, 0, 0);
    lv_obj_set_style_bg_color(b, COLOR_CARD, 0);
    lv_obj_set_style_pad_hor(b, kBubblePadX, 0);
    lv_obj_set_style_pad_ver(b, kBubblePadY, 0);
    lv_obj_clear_flag(b, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(b, LV_OBJ_FLAG_HIDDEN);
    lv_obj_t *t = make_label(b, &font_cn_16, COLOR_TEXT);
    lv_label_set_long_mode(t, LV_LABEL_LONG_WRAP);
    lv_label_set_recolor(t, true);  // 还没念到的字用灰色
    lv_obj_set_style_text_line_space(t, kBubbleLineSpace, 0);
    chat_bubbles[i] = b;
    chat_texts[i] = t;
  }

  lbl_chat_hint = make_label(scr, &font_cn_16, COLOR_SUBTLE);
  lv_obj_set_width(lbl_chat_hint, SCREEN_W - 24);
  lv_obj_set_style_text_align(lbl_chat_hint, LV_TEXT_ALIGN_CENTER, 0);
  lv_label_set_long_mode(lbl_chat_hint, LV_LABEL_LONG_DOT);
  lv_obj_align(lbl_chat_hint, LV_ALIGN_TOP_MID, 0, 154);

  // 开始聊天 / 挂断 / 打断的按钮，在听的时候下面有一条音量条
  chat_button = lv_obj_create(scr);
  lv_obj_set_size(chat_button, 168, 46);
  lv_obj_align(chat_button, LV_ALIGN_BOTTOM_MID, 0, -14);
  lv_obj_set_style_radius(chat_button, 23, 0);
  lv_obj_set_style_border_width(chat_button, 0, 0);
  lv_obj_set_style_pad_all(chat_button, 0, 0);
  lv_obj_clear_flag(chat_button, LV_OBJ_FLAG_SCROLLABLE);
  lbl_chat_button = make_label(chat_button, &font_cn_16, COLOR_TEXT);
  lv_obj_center(lbl_chat_button);

  chat_level = lv_obj_create(scr);
  lv_obj_set_height(chat_level, 4);
  lv_obj_set_style_radius(chat_level, 2, 0);
  lv_obj_set_style_border_width(chat_level, 0, 0);
  lv_obj_set_style_bg_color(chat_level, COLOR_ACCENT, 0);
  lv_obj_add_flag(chat_level, LV_OBJ_FLAG_HIDDEN);
}

static void draw_chat_cat(PetMood mood, bool blink) {
  static int last = -1;
  static uint8_t epoch;
  int key = (int)mood * 2 + blink;
  if (!chat_cat_buf || (key == last && epoch == theme_epoch)) return;
  last = key;
  epoch = theme_epoch;
  const int scale = kChatCat / kPetSize;
  for (int y = 0; y < kChatCat; y++) {
    for (int x = 0; x < kChatCat; x++) {
      chat_cat_buf[y * kChatCat + x] = pet_color(pet_pixel(mood, blink, x / scale, y / scale));
    }
  }
  lv_obj_invalidate(chat_cat);
}

static void set_text(lv_obj_t *label, const char *text) {
  if (strcmp(lv_label_get_text(label), text) != 0) lv_label_set_text(label, text);
}

// 一个要画的气泡。spoken 是已经念出来的字节数，后面的字用灰色；SIZE_MAX 表示整句都正常显示
struct Bubble {
  bool me;
  const char *text;
  size_t spoken;
  bool hint;  // 不是谁说的话，是「嗯……」这样的提示，字用灰色
};

// 主人的气泡：深色主题是暗蓝色，浅色主题是淡蓝色
static lv_color_t chat_me_bg() { return lv_color_hex(pal == &kLight ? 0xDCEAF8 : 0x1C3A5E); }

// 把 text 的前 split 个字节正常显示、后面用 color 显示，写成标签的 "#RRGGBB 文字#" 格式。
// 正文里的 # 会被当成颜色命令，换成空格
static void recolor_text(char *out, size_t size, const char *text, size_t split, uint32_t color) {
  size_t n = 0;
  for (size_t i = 0; text[i] && n + 10 < size; i++) {
    if (i == split) n += snprintf(out + n, size - n, "#%06X ", (unsigned)color);
    out[n++] = text[i] == '#' ? ' ' : text[i];
  }
  if (split < strlen(text) && n + 1 < size) out[n++] = '#';
  out[n] = 0;
}

static void render_bubbles(const Bubble *bubbles, int n) {
  static char buf[kChatLineLen + 16];
  int ys[kChatBubbles];
  int y = 0;
  int focus = -1;  // 正在念的地方（相对第一个气泡顶上），要保证看得见
  for (int i = 0; i < kChatBubbles; i++) {
    lv_obj_t *b = chat_bubbles[i];
    if (i >= n) {
      lv_obj_add_flag(b, LV_OBJ_FLAG_HIDDEN);
      continue;
    }
    const Bubble &bb = bubbles[i];
    int max_w = (bb.me ? kChatMeW : kChatAreaW) - 2 * kBubblePadX;
    lv_point_t size;
    lv_txt_get_size(&size, bb.text, &font_cn_16, 0, kBubbleLineSpace, max_w, LV_TEXT_FLAG_NONE);
    size_t len = strlen(bb.text);
    size_t split = bb.spoken < len ? bb.spoken : len;
    recolor_text(buf, sizeof(buf), bb.text, split, pal->subtle);
    lv_obj_t *t = chat_texts[i];
    lv_obj_set_width(t, size.x + 1);
    set_text(t, buf);
    lv_obj_set_style_text_color(t, bb.hint ? COLOR_SUBTLE : COLOR_TEXT, 0);
    lv_obj_set_style_bg_color(b, bb.me ? chat_me_bg() : COLOR_CARD, 0);
    int w = size.x + 1 + 2 * kBubblePadX;
    int h = size.y + 2 * kBubblePadY;
    lv_obj_set_size(b, w, h);
    lv_obj_set_x(b, bb.me ? kChatAreaW - w : 0);
    lv_obj_clear_flag(b, LV_OBJ_FLAG_HIDDEN);
    if (split < len) {
      // 念到的那一行下面再多露一行
      std::string said(bb.text, split);
      lv_point_t p;
      lv_txt_get_size(&p, said.c_str(), &font_cn_16, 0, kBubbleLineSpace, max_w, LV_TEXT_FLAG_NONE);
      focus = y + kBubblePadY + p.y + font_cn_16.line_height + kBubbleLineSpace;
      if (focus > y + h) focus = y + h;
    }
    ys[i] = y;
    y += h + kBubbleGap;
  }
  // 放不下时往上推，让最新的露出来；小橘正在念的话太长时，先停在念到的地方
  int total = y - kBubbleGap;
  int off = total > kChatAreaH ? total - kChatAreaH : 0;
  if (focus >= 0 && focus - kChatAreaH < off) off = focus > kChatAreaH ? focus - kChatAreaH : 0;
  // 上面被切掉一半的气泡不显示，剩下的顶到最上面；最后一句太长放不下时只能切
  int first = n - 1;
  while (first > 0 && ys[first - 1] >= off) first--;
  if (ys[first] >= off) off = ys[first];
  for (int i = 0; i < n; i++) {
    if (i < first) {
      lv_obj_add_flag(chat_bubbles[i], LV_OBJ_FLAG_HIDDEN);
    } else {
      lv_obj_set_y(chat_bubbles[i], ys[i] - off);
    }
  }
}

void ui_set_chat(const ChatView &v) {
  PetMood mood = PetMood::Normal;
  const char *hint = "";
  const char *button = "挂断";
  lv_color_t button_color = COLOR_ERR;
  // 还没说过话时小橘说的
  const char *reply = v.wake ? "喵！喊「喵喵同学」或点下面的按钮，然后直接跟我说话就行。"
                             : "喵！点下面的按钮接通，然后直接跟我说话就行，不用一直按着。";
  bool level = false;
  char buf[192];
  switch (v.state) {
    case VoiceState::Off:
      reply = "这块板子没有麦克风，聊不了天喵。";
      button = "用不了";
      button_color = COLOR_FLAT;
      break;
    case VoiceState::Idle:
      button = "开始聊天";
      button_color = COLOR_ACCENT;
      hint = v.error;
      if (v.wake && v.has_key && !(hint && hint[0])) hint = "也可以喊「喵喵同学」叫醒我";
      if (!v.has_key) {
        reply = "先在设置网页里填上阿里云百炼的 API Key，我就能跟你聊天啦。";
        hint = "设置网页 →「AI 语音聊天」";
      }
      break;
    case VoiceState::Connecting:
      reply = "我在听喵，想聊点什么？";
      hint = "正在接通…";
      break;
    case VoiceState::Listening:
      reply = "我在听喵，想聊点什么？";
      level = true;
      if (v.error && v.error[0]) {
        hint = v.error;
      } else if (v.idle_left_ms > 0 && v.idle_left_ms < 8000) {
        snprintf(buf, sizeof(buf), "没听到声音，%u 秒后挂断", (unsigned)(v.idle_left_ms / 1000 + 1));
        hint = buf;
      } else if (v.latency_ms) {
        snprintf(buf, sizeof(buf), "在听，直接说吧 · 上次 %u.%u 秒回应", (unsigned)(v.latency_ms / 1000),
                 (unsigned)(v.latency_ms % 1000 / 100));
        hint = buf;
      } else {
        hint = "在听，直接说吧";
      }
      break;
    case VoiceState::Hearing:
      reply = "我在听喵，想聊点什么？";
      level = true;
      hint = "在听…";
      break;
    case VoiceState::Thinking:
      reply = "嗯……让我想想。";
      mood = PetMood::Bored;
      hint = "小橘在想……";
      button = "打断";
      button_color = COLOR_FLAT;
      break;
    case VoiceState::Speaking:
      mood = PetMood::Happy;
      hint = v.full_duplex ? "直接说话就能打断我" : "点「打断」可以插话";
      button = "打断";
      button_color = COLOR_FLAT;
      break;
    case VoiceState::Error:
      mood = PetMood::Sad;
      hint = v.error;
      button = "重新开始";
      button_color = COLOR_ACCENT;
      break;
  }

  // 气泡：聊过的话；还没聊过就是小橘的开场白。小橘在想的时候后面跟一个「……」
  Bubble bubbles[kChatBubbles];
  int n = 0;
  int last_cat = -1;
  int count = v.log ? v.log->count() : 0;
  for (int i = 0; i < count; i++) {
    const ChatLine &l = v.log->line(i);
    if (l.text[0]) {
      if (!l.me) last_cat = n;
      bubbles[n++] = {l.me, l.text, SIZE_MAX, l.pending};
    }
  }
  if (n == 0) {
    bubbles[n++] = {false, reply, SIZE_MAX, false};
  } else if (v.state == VoiceState::Thinking && bubbles[n - 1].me) {
    bubbles[n++] = {false, "……", SIZE_MAX, true};
  }
  static std::string said;  // 小橘正在说的那句已经念出来的部分，用来定表情
  said.clear();
  if (last_cat >= 0 && v.spoken != kSpokenAll) {
    Bubble &b = bubbles[last_cat];
    b.spoken = utf8_prefix(b.text, v.spoken);
    said.assign(b.text, b.spoken);
  }
  if (v.state == VoiceState::Speaking) mood = chat_mood(said.c_str(), PetMood::Happy);

  // 内容没变就不用重新排版
  static uint32_t last_rev = 0xFFFFFFFF;
  static uint16_t last_spoken;
  static const char *last_reply;
  static int last_n;
  static uint8_t last_epoch;
  uint32_t rev = v.log ? v.log->rev() : 0;
  if (rev != last_rev || v.spoken != last_spoken || reply != last_reply || n != last_n || theme_epoch != last_epoch) {
    last_rev = rev;
    last_spoken = v.spoken;
    last_reply = reply;
    last_n = n;
    last_epoch = theme_epoch;
    render_bubbles(bubbles, n);
  }

  draw_chat_cat(mood, v.blink);
  lv_obj_set_y(chat_cat, 60 + v.bob);
  set_text(lbl_chat_hint, hint);
  lv_obj_set_style_text_color(lbl_chat_hint, v.state == VoiceState::Error ? COLOR_ERR : COLOR_SUBTLE, 0);
  set_text(lbl_chat_button, button);
  lv_obj_set_style_bg_color(chat_button, button_color, 0);
  if (level) {
    int w = 8 + v.level * 160 / 100;
    lv_obj_set_width(chat_level, w);
    lv_obj_align(chat_level, LV_ALIGN_BOTTOM_MID, 0, -6);
    lv_obj_clear_flag(chat_level, LV_OBJ_FLAG_HIDDEN);
  } else {
    lv_obj_add_flag(chat_level, LV_OBJ_FLAG_HIDDEN);
  }
}

bool ui_chat_button_hit(int x, int y) { return hit(chat_button, x, y); }

static void build_pomodoro() {
  lv_obj_t *scr = make_screen();
  scr_pages[PAGE_POMODORO] = scr;

  lv_obj_t *title = make_label(scr, &font_cn_16, COLOR_SUBTLE);
  lv_label_set_text(title, "番茄钟");
  lv_obj_align(title, LV_ALIGN_TOP_LEFT, 12, 10);
  dot_wifi[PAGE_POMODORO] = make_wifi_dot(scr);

  pomo_arc = lv_arc_create(scr);
  lv_obj_set_size(pomo_arc, kPomoRing, kPomoRing);
  lv_obj_align(pomo_arc, LV_ALIGN_CENTER, 0, 0);
  lv_arc_set_rotation(pomo_arc, 270);  // 从 12 点方向开始
  lv_arc_set_bg_angles(pomo_arc, 0, 360);
  lv_arc_set_range(pomo_arc, 0, 1000);
  lv_obj_remove_style(pomo_arc, nullptr, LV_PART_KNOB);
  lv_obj_clear_flag(pomo_arc, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_style_arc_width(pomo_arc, 12, LV_PART_MAIN);
  lv_obj_set_style_arc_width(pomo_arc, 12, LV_PART_INDICATOR);
  lv_obj_set_style_arc_color(pomo_arc, COLOR_LINE, LV_PART_MAIN);

  lbl_pomo_time = make_label(scr, &lv_font_montserrat_48, COLOR_TEXT);
  lv_obj_align(lbl_pomo_time, LV_ALIGN_CENTER, 0, -10);
  lbl_pomo_state = make_label(scr, &font_cn_16, COLOR_SUBTLE);
  lv_obj_align(lbl_pomo_state, LV_ALIGN_CENTER, 0, 32);

  // 阶段结束的提示放在顶栏正中，圆环里面放不下
  lbl_pomo_msg = make_label(scr, &font_cn_16, COLOR_ACCENT);
  lv_obj_align(lbl_pomo_msg, LV_ALIGN_TOP_MID, 0, 10);

  lbl_pomo_today = make_label(scr, &font_cn_16, COLOR_SUBTLE);
  lv_obj_align(lbl_pomo_today, LV_ALIGN_BOTTOM_LEFT, 12, -10);

  // 右下角：本轮进度，每个点代表一个番茄
  for (int i = 0; i < kPomoMaxDots; i++) {
    lv_obj_t *d = lv_obj_create(scr);
    lv_obj_set_size(d, 8, 8);
    lv_obj_set_style_radius(d, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(d, 0, 0);
    lv_obj_align(d, LV_ALIGN_BOTTOM_RIGHT, -12 - (kPomoMaxDots - 1 - i) * 14, -14);
    pomo_dots[i] = d;
  }
}

void ui_init() {
  build_status();
  build_main();
  scr_pages[PAGE_CLOCK] = scr_main;
  build_weather();
  build_news();
  build_ticker();
  build_github();
  build_stocks();
  build_pet();
  build_friends();
  build_pomodoro();
  build_chat();
  build_album();
  build_movie();
  build_music();
  build_music_bar();
  // 每个页面左右边缘加淡淡的 < >，提示点两边或左右滑动翻页
  for (int p = 0; p < PAGE_COUNT; p++) {
    for (int side = 0; side < 2; side++) {
      lv_obj_t *a = make_label(scr_pages[p], &lv_font_montserrat_14, COLOR_SUBTLE);
      lv_label_set_text(a, side ? LV_SYMBOL_RIGHT : LV_SYMBOL_LEFT);
      lv_obj_set_style_text_opa(a, LV_OPA_40, 0);
      // GitHub 页正中是热力图，箭头挪到上面数字那一行的两边，免得压住格子
      lv_obj_align(a, side ? LV_ALIGN_RIGHT_MID : LV_ALIGN_LEFT_MID, side ? -1 : 1, p == PAGE_GITHUB ? -48 : 0);
      lv_obj_clear_flag(a, LV_OBJ_FLAG_CLICKABLE);
      page_arrows[p][side] = a;
    }
  }
  build_alert();
  build_quick();
  build_qpages();
  build_game();
  build_phrases();
  build_friend_alert();
  Pomodoro idle;
  ui_set_pomodoro(idle, 0, nullptr, 0);
}

void ui_show_status(const char *title, const char *detail) {
  lv_obj_add_flag(status_bar, LV_OBJ_FLAG_HIDDEN);
  lv_label_set_text(status_title, title);
  lv_label_set_text(status_detail, detail);
  lv_obj_align(status_title, LV_ALIGN_TOP_MID, 0, 40);
  screen_load(scr_status, false);
}

void ui_show_progress(const char *title, const char *detail, int percent) {
  ui_show_status(title, detail);
  lv_bar_set_value(status_bar, percent < 0 ? 0 : percent > 100 ? 100 : percent, LV_ANIM_OFF);
  lv_obj_clear_flag(status_bar, LV_OBJ_FLAG_HIDDEN);
}

void ui_show_provision(const char *ap_name) {
  char detail[256];
  snprintf(detail, sizeof(detail),
           "1. 用手机连接 WiFi 热点\n#58A6FF %s#\n\n"
           "2. 在自动弹出的页面里选择你家的 WiFi\n"
           "没有弹出就用浏览器打开 192.168.4.1",
           ap_name);
  ui_show_status("首次使用：配置 WiFi", detail);
}

void ui_show_page(Page page, bool animate, bool back) {
  screen_load(scr_pages[page], animate, back);
}

// 画月亮：每个像素取 3x3 个点，按亮面、暗面、背景的比例混色，边缘不会有锯齿
static void draw_moon(float phase) {
  const lv_color_t lit = lv_color_hex(pal->moon_lit), dark = lv_color_hex(pal->moon_dark);
  const float r = kMoonPx / 2.0f - 0.5f;
  for (int py = 0; py < kMoonPx; py++) {
    for (int px = 0; px < kMoonPx; px++) {
      int n_lit = 0, n_dark = 0;
      for (int sy = 0; sy < 3; sy++) {
        for (int sx = 0; sx < 3; sx++) {
          float x = px + (sx + 0.5f) / 3 - kMoonPx / 2.0f;
          float y = py + (sy + 0.5f) / 3 - kMoonPx / 2.0f;
          if (x * x + y * y > r * r) continue;
          if (moon_lit(phase, x, y, r)) n_lit++;
          else n_dark++;
        }
      }
      lv_color_t c = lv_color_mix(dark, COLOR_BG, (uint8_t)(n_dark * 255 / 9));
      c = lv_color_mix(lit, c, (uint8_t)(n_lit * 255 / 9));
      moon_buf[py * kMoonPx + px] = c;
    }
  }
  lv_obj_invalidate(moon_canvas);
}

// 「盈凸月 · 日出 06:04 · 日落 17:21」，没有天气预报时只有月相
static void update_sun_row() {
  if (moon_now < 0) return;
  char text[64];
  int n = snprintf(text, sizeof(text), "%s", moon_phase_name(moon_now));
  if (sun_rise >= 0 && sun_set >= 0)
    snprintf(text + n, sizeof(text) - n, " · 日出 %02d:%02d · 日落 %02d:%02d", sun_rise / 60, sun_rise % 60,
             sun_set / 60, sun_set % 60);
  lv_label_set_text(lbl_sun, text);
  if (!ticker_shown && !music_bar_shown) lv_obj_clear_flag(sun_row, LV_OBJ_FLAG_HIDDEN);
}

void ui_set_sun(int sunrise, int sunset) {
  sun_rise = sunrise;
  sun_set = sunset;
  update_sun_row();
}

void ui_tick_clock() {
  static int last_sec = -1;
  static const char *const weekdays[] = {"周日", "周一", "周二", "周三", "周四", "周五", "周六"};

  struct tm t;
  if (!getLocalTime(&t, 0) || t.tm_year < 2024 - 1900) return;  // NTP 还没同步
  if (t.tm_sec == last_sec) return;
  last_sec = t.tm_sec;

  lv_label_set_text_fmt(lbl_time, "%02d:%02d", t.tm_hour, t.tm_min);
  lv_label_set_text_fmt(lbl_sec, "%02d", t.tm_sec);
  lv_obj_align_to(lbl_sec, lbl_time, LV_ALIGN_OUT_RIGHT_BOTTOM, 4, -6);
  lv_label_set_text_fmt(lbl_date, "%d月%d日 %s", t.tm_mon + 1, t.tm_mday, weekdays[t.tm_wday]);

  static int last_mday = -1;
  if (t.tm_mday != last_mday) {  // 月相每天算一次
    last_mday = t.tm_mday;
    moon_now = moon_phase(t.tm_year + 1900, t.tm_mon + 1, t.tm_mday);
    draw_moon(moon_now);
    update_sun_row();
  }
}

void ui_set_city(const char *city) {
  city_name = city ? city : "";
  if (!friends_here_shown) lv_label_set_text(lbl_city, city);
  lv_obj_align_to(lbl_city, dot_wifi[PAGE_CLOCK], LV_ALIGN_OUT_LEFT_MID, -8, 0);
  if (city && city[0]) lv_label_set_text_fmt(lbl_fc_city, "%s · 天气预报", city);
  else lv_label_set_text(lbl_fc_city, "天气预报");
}

void ui_set_room(float temp, float humidity, bool hot, bool humid) {
  // 超过阈值的数字用橙色；LVGL 的 # 重新着色语法：#RRGGBB 文字#
  char t[32], h[32];
  snprintf(t, sizeof(t), hot ? "#DB6D28 %.1f\xC2\xB0" "C#" : "%.1f\xC2\xB0" "C", temp);
  snprintf(h, sizeof(h), humid ? "#DB6D28 %d%%#" : "%d%%", (int)lroundf(humidity));
  lv_label_set_text_fmt(lbl_room, "室内 %s · 湿度 %s", t, h);
  lv_obj_align(lbl_room, LV_ALIGN_TOP_MID, 0, kRoomY);
  lv_obj_clear_flag(lbl_room, LV_OBJ_FLAG_HIDDEN);
  lv_obj_align(sun_row, LV_ALIGN_TOP_MID, 0, kSunY);
  lv_obj_align(ticker_row, LV_ALIGN_TOP_MID, 0, kSunY - 2);
  lv_obj_align(music_bar, LV_ALIGN_TOP_MID, 0, kSunY - 1);
}

// 天气预报页
static void set_forecast(const WeatherData &w) {
  char text[96];
  weather_rain_summary(w, text, sizeof(text));
  bool wet = weather_rain_index(w) >= 0;
  lv_label_set_text(lbl_rain_title, text[0] ? text : "没有逐小时预报");
  lv_obj_set_style_text_color(lbl_rain_title, wet ? COLOR_RAIN : COLOR_TEXT, 0);
  for (int i = 0; i < kRainHours; i++) {
    lv_obj_t *b = rain_bars[i];
    if (i >= w.nhours) {
      lv_obj_add_flag(b, LV_OBJ_FLAG_HIDDEN);
      continue;
    }
    const HourRain &h = w.hours[i];
    int ht = 2 + h.prob * (kRainBarMaxH - 2) / 100;
    lv_obj_set_height(b, ht);
    lv_obj_set_y(b, kRainBase - ht);
    lv_obj_set_style_bg_color(b, weather_hour_wet(h) ? COLOR_RAIN : h.prob >= 30 ? lv_color_hex(pal->rain_soft) : COLOR_FLAT, 0);
    lv_obj_clear_flag(b, LV_OBJ_FLAG_HIDDEN);
  }
  for (int i = 0; i < kRainHours / 3; i++) {
    if (i * 3 < w.nhours) lv_label_set_text_fmt(lbl_rain_hours[i], "%d", w.hours[i * 3].hour);
    else lv_label_set_text(lbl_rain_hours[i], "");
  }
  for (int i = 0; i < kForecastDays - 1; i++) {
    if (i + 1 >= w.ndays) {
      lv_label_set_text(lbl_fc_name[i], "");
      lv_label_set_text(lbl_fc_desc[i], "--");
      lv_label_set_text(lbl_fc_range[i], "");
      lv_label_set_text(lbl_fc_rain[i], "");
      continue;
    }
    const DayForecast &d = w.days[i + 1];
    lv_label_set_text(lbl_fc_name[i], weather_day_name(d, i + 1));
    lv_label_set_text(lbl_fc_desc[i], weather_text(d.code));
    lv_obj_set_style_text_color(lbl_fc_desc[i], lv_color_hex(weather_color_on(d.code)), 0);
    lv_label_set_text_fmt(lbl_fc_range[i], "%d\xC2\xB0 / %d\xC2\xB0", (int)lroundf(d.temp_min), (int)lroundf(d.temp_max));
    if (d.rain_prob > 0) lv_label_set_text_fmt(lbl_fc_rain[i], "降水 %d%%", d.rain_prob);
    else lv_label_set_text(lbl_fc_rain[i], "不下雨");
    lv_obj_set_style_text_color(lbl_fc_rain[i], d.rain_prob >= 50 ? COLOR_RAIN : COLOR_SUBTLE, 0);
  }
}

// 最近一次成功拿到的数据，换主题时用它重新上色（天气描述、雨量条、贡献格子、涨跌标签的颜色跟着数据变）
static WeatherData last_weather;
static GithubData last_github;
static StockData last_stocks;
static bool have_weather, have_github, have_stocks;

void ui_set_weather(const WeatherData &w) {
  if (w.error) {
    // 只有开机后一次都没成功过才会收到错误，天气描述那行保持「获取中…」
    lv_label_set_text_fmt(lbl_humidity, "获取失败（%d）", w.error);
    lv_label_set_text_fmt(lbl_rain_title, "获取失败（%d），稍后自动重试", w.error);
    return;
  }
  if (&w != &last_weather) last_weather = w;
  have_weather = true;
  if (w.ndays > 0) ui_set_sun(w.days[0].sunrise, w.days[0].sunset);
  lv_label_set_text_fmt(lbl_temp, "%d\xC2\xB0", (int)lroundf(w.temp));
  lv_label_set_text(lbl_desc, weather_text(w.code));
  lv_obj_set_style_text_color(lbl_desc, lv_color_hex(weather_color_on(w.code)), 0);
  lv_label_set_text_fmt(lbl_range, "%d\xC2\xB0 / %d\xC2\xB0", (int)lroundf(w.temp_min),
                        (int)lroundf(w.temp_max));
  char rain[48];
  weather_rain_short(w, rain, sizeof(rain));
  if (rain[0]) lv_label_set_text_fmt(lbl_humidity, "#%06X %s#", (unsigned)pal->rain, rain);  // 卡片上放不下两句，有雨时先说雨
  else lv_label_set_text_fmt(lbl_humidity, "湿度 %d%%", w.humidity);
  set_forecast(w);
}

void ui_set_github(const GithubData &g) {
  const char *msg = nullptr;
  switch (g.status) {
    case GithubStatus::NoToken: msg = "请在设置页面填写 GitHub Token"; break;
    case GithubStatus::Loading: msg = "获取中…"; break;
    case GithubStatus::BadToken: msg = "Token 无效或已过期\n请在设置页面重新填写"; break;
    case GithubStatus::Error: msg = "获取失败（%d），稍后自动重试"; break;
    case GithubStatus::Ok: break;
  }
  if (msg) {
    have_github = false;
    lv_label_set_text_fmt(lbl_gh_msg, msg, g.error);
    lv_obj_clear_flag(lbl_gh_msg, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(gh_grid, LV_OBJ_FLAG_HIDDEN);
    return;
  }

  if (&g != &last_github) last_github = g;
  have_github = true;
  lv_obj_add_flag(lbl_gh_msg, LV_OBJ_FLAG_HIDDEN);
  lv_obj_clear_flag(gh_grid, LV_OBJ_FLAG_HIDDEN);
  lv_label_set_text_fmt(lbl_gh_login, "@%s", g.login);
  lv_label_set_text_fmt(lbl_gh_today, "%u", g.today);
  lv_label_set_text_fmt(lbl_gh_streak, "%u", g.streak);
  lv_label_set_text_fmt(lbl_gh_total, "%u", g.total);
  lv_obj_set_style_text_color(lbl_gh_today, g.today > 0 ? lv_color_hex(pal->level[4]) : COLOR_TEXT, 0);

  for (int w = 0; w < kGithubWeeks; w++) {
    for (int d = 0; d < 7; d++) {
      uint8_t lv = g.level[w][d];
      lv_obj_t *c = gh_cells[w][d];
      if (lv == kGithubNoDay) {
        lv_obj_add_flag(c, LV_OBJ_FLAG_HIDDEN);
      } else {
        lv_obj_clear_flag(c, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_bg_color(c, lv_color_hex(pal->level[lv > 4 ? 4 : lv]), 0);
      }
    }
  }
}

void ui_set_stocks(const StockData &s) {
  const char *msg = nullptr;
  switch (s.status) {
    case StockStatus::NotSet: msg = "请在设置页面填写自选股代码"; break;
    case StockStatus::Loading: msg = "获取中…"; break;
    case StockStatus::Error: msg = "获取失败（%d），稍后自动重试"; break;
    case StockStatus::Ok: break;
  }
  if (msg) {
    have_stocks = false;
    lv_label_set_text_fmt(lbl_stock_msg, msg, s.error);
    lv_obj_clear_flag(lbl_stock_msg, LV_OBJ_FLAG_HIDDEN);
    for (StockRow &r : stock_rows) lv_obj_add_flag(r.row, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(lbl_stock_updated, "");
    return;
  }

  if (&s != &last_stocks) last_stocks = s;
  have_stocks = true;
  lv_obj_add_flag(lbl_stock_msg, LV_OBJ_FLAG_HIDDEN);
  if (s.updated[0]) lv_label_set_text_fmt(lbl_stock_updated, "%s 更新", s.updated);
  lv_obj_align_to(lbl_stock_updated, dot_wifi[PAGE_STOCKS], LV_ALIGN_OUT_LEFT_MID, -8, 0);

  // LVGL 自带的格式化不支持 %f，数字先用 snprintf 转成文字
  char buf[24];
  for (int i = 0; i < kMaxStocks; i++) {
    StockRow &r = stock_rows[i];
    if (i >= s.count) {
      lv_obj_add_flag(r.row, LV_OBJ_FLAG_HIDDEN);
      continue;
    }
    const StockQuote &q = s.quotes[i];
    lv_obj_clear_flag(r.row, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(r.name, q.name);

    if (!q.valid) {
      lv_label_set_text(r.price, "--");
      lv_label_set_text(r.pct, "--");
      lv_obj_set_style_bg_color(r.pill, COLOR_FLAT, 0);
      continue;
    }
    snprintf(buf, sizeof(buf), q.price < 10 ? "%.3f" : "%.2f", q.price);
    lv_label_set_text(r.price, buf);
    snprintf(buf, sizeof(buf), "%+.2f%%", q.change_pct);
    lv_label_set_text(r.pct, buf);
    lv_obj_set_style_bg_color(r.pill,
                              q.change_pct > 0 ? COLOR_UP : q.change_pct < 0 ? COLOR_DOWN : COLOR_FLAT, 0);
    lv_obj_set_style_text_color(r.pct, q.change_pct == 0 ? lv_color_hex(pal->flat_text) : lv_color_white(), 0);
  }
}

void ui_set_pomodoro(const Pomodoro &p, uint32_t now_ms, const char *msg, int done_today) {
  uint32_t left = p.remaining_ms(now_ms);
  uint32_t total = p.total_ms();
  uint32_t secs = (left + 999) / 1000;  // 向上取整，开始时显示 25:00 而不是 24:59
  lv_label_set_text_fmt(lbl_pomo_time, "%02u:%02u", (unsigned)(secs / 60), (unsigned)(secs % 60));
  lv_arc_set_value(pomo_arc, total ? (int32_t)((uint64_t)left * 1000 / total) : 0);

  bool on_break = p.phase() == PomoPhase::ShortBreak || p.phase() == PomoPhase::LongBreak;
  lv_color_t color = !p.running() ? COLOR_SUBTLE : on_break ? COLOR_OK : COLOR_ERR;
  lv_obj_set_style_arc_color(pomo_arc, color, LV_PART_INDICATOR);

  const char *state;
  if (p.phase() == PomoPhase::Idle) {
    state = "点圆环开始专注";
  } else if (!p.running()) {
    state = p.remaining_ms(now_ms) == total ? "点圆环开始专注" : "已暂停";
  } else if (p.phase() == PomoPhase::Focus) {
    state = "专注中";
  } else {
    state = p.phase() == PomoPhase::LongBreak ? "长休息" : "短休息";
  }
  lv_label_set_text(lbl_pomo_state, state);
  lv_label_set_text(lbl_pomo_msg, msg ? msg : "");
  lv_label_set_text_fmt(lbl_pomo_today, "今日完成 %d 个", done_today);

  // 点靠右对齐：只显示最右边的 long_every 个，从左到右标记本轮已完成的番茄
  int every = p.config().long_every < kPomoMaxDots ? p.config().long_every : kPomoMaxDots;
  for (int i = 0; i < kPomoMaxDots; i++) {
    int k = i - (kPomoMaxDots - every);
    if (k < 0) {
      lv_obj_add_flag(pomo_dots[i], LV_OBJ_FLAG_HIDDEN);
      continue;
    }
    lv_obj_clear_flag(pomo_dots[i], LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_bg_color(pomo_dots[i], k < p.cycle_done() ? COLOR_ERR : COLOR_LINE, 0);
  }
}

void ui_set_pet(PetMood mood, bool blink, const char *line, const char *status) {
  static int last_mood = -1;
  static int last_blink = -1;
  static uint8_t epoch;
  if (pet_buf && ((int)mood != last_mood || (int)blink != last_blink || epoch != theme_epoch)) {
    last_mood = (int)mood;
    last_blink = blink;
    epoch = theme_epoch;
    lv_color_t colors[sizeof(kPetPalette) / sizeof(kPetPalette[0])];
    for (size_t i = 0; i < sizeof(kPetPalette) / sizeof(kPetPalette[0]); i++) colors[i] = pet_color(i);
    for (int y = 0; y < kPetPx; y++) {
      for (int x = 0; x < kPetPx; x++) {
        pet_buf[y * kPetPx + x] = colors[pet_pixel(mood, blink, x / kPetScale, y / kPetScale)];
      }
    }
    lv_obj_invalidate(pet_canvas);
  }
  if (strcmp(lv_label_get_text(lbl_pet_line), line) != 0) lv_label_set_text(lbl_pet_line, line);
  if (strcmp(lv_label_get_text(lbl_pet_status), status) != 0) lv_label_set_text(lbl_pet_status, status);
}

void ui_set_pet_music(const float *bands) {
  if (!bands) {
    for (lv_obj_t *b : pet_bars) lv_obj_add_flag(b, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(lbl_pet_status, LV_OBJ_FLAG_HIDDEN);
    return;
  }
  lv_obj_add_flag(lbl_pet_status, LV_OBJ_FLAG_HIDDEN);
  for (int i = 0; i < kMusicBands; i++) {
    int h = 2 + (int)(bands[i] * (kPetBarMaxH - 2));
    lv_obj_set_height(pet_bars[i], h);
    lv_obj_set_y(pet_bars[i], kPetBarBase - h);
    lv_obj_clear_flag(pet_bars[i], LV_OBJ_FLAG_HIDDEN);
  }
}

void ui_pet_anim(int dx, int bob, int zzz) {
  lv_obj_align(pet_canvas, LV_ALIGN_TOP_MID, dx, kPetTop + bob);
  if (zzz < 0) {
    lv_obj_add_flag(lbl_pet_zzz, LV_OBJ_FLAG_HIDDEN);
    return;
  }
  lv_obj_clear_flag(lbl_pet_zzz, LV_OBJ_FLAG_HIDDEN);
  lv_label_set_text(lbl_pet_zzz, zzz ? "Z z" : "z");
  lv_obj_align(lbl_pet_zzz, LV_ALIGN_TOP_MID, kPetPx / 2 + 10, kPetTop + (zzz ? 0 : 12));
}

void ui_show_alert(AlertIcon icon, const char *title, const char *body, const char *foot) {
  switch (icon) {
    case AlertIcon::Message:
      lv_label_set_text(alert_icon, LV_SYMBOL_ENVELOPE);
      lv_obj_set_style_bg_color(alert_badge, COLOR_ACCENT, 0);
      break;
    case AlertIcon::Sit:
      lv_label_set_text(alert_icon, LV_SYMBOL_REFRESH);
      lv_obj_set_style_bg_color(alert_badge, lv_color_hex(0xD29922), 0);
      break;
    case AlertIcon::Alarm:
      lv_label_set_text(alert_icon, LV_SYMBOL_BELL);
      lv_obj_set_style_bg_color(alert_badge, COLOR_ERR, 0);
      break;
    case AlertIcon::Water:
      lv_label_set_text(alert_icon, LV_SYMBOL_TINT);
      lv_obj_set_style_bg_color(alert_badge, lv_color_hex(0x1F6FEB), 0);
      break;
    case AlertIcon::Climate:
      lv_label_set_text(alert_icon, LV_SYMBOL_WARNING);
      lv_obj_set_style_bg_color(alert_badge, lv_color_hex(0xDB6D28), 0);
      break;
    case AlertIcon::Rain:
      lv_label_set_text(alert_icon, LV_SYMBOL_TINT);
      lv_obj_set_style_bg_color(alert_badge, lv_color_hex(0x0E7490), 0);
      break;
    case AlertIcon::Pair:
      lv_label_set_text(alert_icon, LV_SYMBOL_SETTINGS);
      lv_obj_set_style_bg_color(alert_badge, COLOR_ACCENT, 0);
      break;
  }
  lv_obj_center(alert_icon);
  lv_label_set_text(alert_title, title);
  lv_obj_align(alert_title, LV_ALIGN_TOP_MID, 0, 84);
  lv_label_set_text(alert_body, body);
  lv_label_set_text(alert_foot, foot);
  lv_obj_align(alert_foot, LV_ALIGN_BOTTOM_MID, 0, -8);
  screen_load(scr_alert, false);
}

bool ui_pomodoro_hit(int x, int y) {
  int dx = x - SCREEN_W / 2;
  int dy = y - SCREEN_H / 2;
  int r = kPomoRing / 2 + 12;  // 比圆环稍大一点，好点中
  return dx * dx + dy * dy <= r * r;
}

// 换主题：把各页面上用到主题色的地方逐个换成另一套里对应的颜色，按数据上色的部分用最近的数据重画。
// 开机提示页（升级进度、配网）和相册页（照片底下是黑的）一直用深色，不参与
static const lv_style_prop_t kColorProps[] = {
    LV_STYLE_BG_COLOR,   LV_STYLE_BG_GRAD_COLOR, LV_STYLE_BORDER_COLOR, LV_STYLE_OUTLINE_COLOR,
    LV_STYLE_SHADOW_COLOR, LV_STYLE_TEXT_COLOR,  LV_STYLE_LINE_COLOR,   LV_STYLE_ARC_COLOR,
};
static uint32_t Palette::*const kSwap[] = {
    &Palette::bg,   &Palette::card, &Palette::text, &Palette::subtle, &Palette::accent,    &Palette::ok,
    &Palette::err,  &Palette::flat, &Palette::line, &Palette::rain,   &Palette::rain_soft,
};

static void recolor(lv_obj_t *obj, const Palette &from, const Palette &to) {
  for (uint32_t i = 0; i < obj->style_cnt; i++) {
    const _lv_obj_style_t &os = obj->styles[i];
    if (!os.is_local) continue;
    for (lv_style_prop_t prop : kColorProps) {
      lv_style_value_t v;
      if (lv_style_get_prop(os.style, prop, &v) != LV_RES_OK) continue;
      for (uint32_t Palette::*c : kSwap) {
        if (v.color.full == lv_color_hex(from.*c).full) {
          v.color = lv_color_hex(to.*c);
          lv_style_set_prop(os.style, prop, v);
          break;
        }
      }
    }
  }
  for (uint32_t i = 0; i < lv_obj_get_child_cnt(obj); i++) recolor(lv_obj_get_child(obj, i), from, to);
}

void ui_set_theme(bool light) {
  const Palette *next = light ? &kLight : &kDark;
  if (next == pal) return;
  const Palette &from = *pal;
  pal = next;
  theme_epoch++;
  for (lv_obj_t *scr : scr_pages) {
    if (scr != scr_pages[PAGE_ALBUM]) recolor(scr, from, *pal);  // 影片页的播放画面用固定的颜色，换不到
  }
  for (lv_obj_t *scr : {scr_alert, scr_quick, scr_qpages, scr_game, scr_phrases, scr_friend}) recolor(scr, from, *pal);
  for (int lv = 0; lv < 5; lv++) lv_obj_set_style_bg_color(gh_legend[lv], lv_color_hex(pal->level[lv]), 0);
  if (moon_now >= 0) draw_moon(moon_now);
  if (have_weather) ui_set_weather(last_weather);
  if (have_github) ui_set_github(last_github);
  if (have_stocks) ui_set_stocks(last_stocks);
  if (news) render_news();
  lv_obj_invalidate(lv_scr_act());
}

void ui_set_wifi(bool connected) {
  for (lv_obj_t *dot : dot_wifi) {
    lv_obj_set_style_bg_color(dot, connected ? COLOR_OK : COLOR_ERR, 0);
  }
}

void ui_refresh_now() {
  lv_refr_now(nullptr);
}
