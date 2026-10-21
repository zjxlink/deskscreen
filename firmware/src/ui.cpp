#include "ui.h"

#include <Arduino.h>
#include <lvgl.h>
#include <string.h>
#include <time.h>

#include "astro.h"
#include "board.h"
#include "screen_load.h"

// 配色参考 GitHub 深色主题
#define COLOR_BG      lv_color_hex(0x0D1117)
#define COLOR_CARD    lv_color_hex(0x161B22)
#define COLOR_TEXT    lv_color_hex(0xE6EDF3)
#define COLOR_SUBTLE  lv_color_hex(0x8B949E)
#define COLOR_ACCENT  lv_color_hex(0x58A6FF)
#define COLOR_OK      lv_color_hex(0x3FB950)
#define COLOR_ERR     lv_color_hex(0xF85149)

// GitHub 深色主题的贡献等级配色，0 级为空白格
static const uint32_t kLevelColors[5] = {0x161B22, 0x0E4429, 0x006D32, 0x26A641, 0x39D353};

// 行情涨跌配色，按 A 股习惯红涨绿跌
#define COLOR_UP      lv_color_hex(0xDA3633)
#define COLOR_DOWN    lv_color_hex(0x238636)
#define COLOR_FLAT    lv_color_hex(0x30363D)
#define COLOR_LINE    lv_color_hex(0x21262D)

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
static const uint32_t kPetPalette[] = {0x0D1117, 0x5C3317, 0xF0A04B, 0xFFF4E6, 0xFF8FA3, 0x58A6FF};
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
static lv_obj_t *chat_bubble;
static lv_obj_t *lbl_chat_reply;
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
  lv_obj_set_style_bg_color(c, lv_color_hex(kLevelColors[level]), 0);
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

  lv_obj_t *title = make_label(scr_quick, &font_cn_16, COLOR_SUBTLE);
  lv_label_set_text(title, "快捷设置");
  lv_obj_align(title, LV_ALIGN_TOP_LEFT, 12, 10);

  quick_close = make_button(scr_quick, LV_SYMBOL_CLOSE);
  lv_obj_align(quick_close, LV_ALIGN_TOP_RIGHT, -8, 4);

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

void ui_show_quick(const QuickState &q) {
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

QuickHit ui_quick_hit(int x, int y) {
  lv_obj_update_layout(scr_quick);
  if (hit(quick_close, x, y)) return QuickHit::Close;
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
  if (!game_cat_buf || (int)mood == last) return;
  last = (int)mood;
  const int scale = RunnerGame::kCatSize / kPetSize;
  for (int y = 0; y < RunnerGame::kCatSize; y++) {
    for (int x = 0; x < RunnerGame::kCatSize; x++) {
      game_cat_buf[y * RunnerGame::kCatSize + x] = lv_color_hex(kPetPalette[pet_pixel(mood, false, x / scale, y / scale)]);
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

  // 气泡：小橘说的话
  chat_bubble = lv_obj_create(scr);
  lv_obj_set_size(chat_bubble, SCREEN_W - 84, 112);
  lv_obj_set_pos(chat_bubble, 72, 36);
  lv_obj_set_style_radius(chat_bubble, 10, 0);
  lv_obj_set_style_border_width(chat_bubble, 0, 0);
  lv_obj_set_style_bg_color(chat_bubble, COLOR_CARD, 0);
  lv_obj_set_style_pad_all(chat_bubble, 8, 0);
  lv_obj_clear_flag(chat_bubble, LV_OBJ_FLAG_SCROLLABLE);
  lbl_chat_reply = make_label(chat_bubble, &font_cn_16, COLOR_TEXT);
  lv_obj_set_width(lbl_chat_reply, SCREEN_W - 84 - 16);
  lv_label_set_long_mode(lbl_chat_reply, LV_LABEL_LONG_WRAP);
  lv_obj_set_style_text_line_space(lbl_chat_reply, 2, 0);

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
  int key = (int)mood * 2 + blink;
  if (!chat_cat_buf || key == last) return;
  last = key;
  const int scale = kChatCat / kPetSize;
  for (int y = 0; y < kChatCat; y++) {
    for (int x = 0; x < kChatCat; x++) {
      chat_cat_buf[y * kChatCat + x] = lv_color_hex(kPetPalette[pet_pixel(mood, blink, x / scale, y / scale)]);
    }
  }
  lv_obj_invalidate(chat_cat);
}

static void set_text(lv_obj_t *label, const char *text) {
  if (strcmp(lv_label_get_text(label), text) != 0) lv_label_set_text(label, text);
}

void ui_set_chat(const ChatView &v) {
  PetMood mood = PetMood::Normal;
  const char *hint = "";
  const char *button = "挂断";
  lv_color_t button_color = COLOR_ERR;
  bool has_reply = v.reply && v.reply[0];
  const char *reply = has_reply ? v.reply
                                 : v.wake ? "喵！喊「喵喵同学」或点下面的按钮，然后直接跟我说话就行。"
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
      hint = "正在接通…";
      break;
    case VoiceState::Listening:
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
      level = true;
      hint = "在听…";
      break;
    case VoiceState::Thinking:
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
  if (!has_reply && (v.state == VoiceState::Connecting || v.state == VoiceState::Listening ||
                     v.state == VoiceState::Hearing)) {
    reply = "我在听喵，想聊点什么？";
  } else if (!has_reply && v.state == VoiceState::Thinking) {
    reply = "嗯……让我想想。";
  }
  // 小橘在想或在说的时候，提示行显示主人刚才说的话
  if ((v.state == VoiceState::Thinking || v.state == VoiceState::Speaking) && v.heard && v.heard[0]) {
    snprintf(buf, sizeof(buf), "你：%s", v.heard);
    hint = buf;
  }
  draw_chat_cat(mood, v.blink);
  lv_obj_set_y(chat_cat, 60 + v.bob);
  set_text(lbl_chat_reply, reply);
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
  build_github();
  build_stocks();
  build_pet();
  build_pomodoro();
  build_chat();
  build_album();
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
  build_game();
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
  const lv_color_t lit = lv_color_hex(0xF2E6B8), dark = lv_color_hex(0x2D333B);
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
  lv_obj_clear_flag(sun_row, LV_OBJ_FLAG_HIDDEN);
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
  lv_label_set_text(lbl_city, city);
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
}

// 天气预报页
static void set_forecast(const WeatherData &w) {
  char text[96];
  weather_rain_summary(w, text, sizeof(text));
  bool wet = weather_rain_index(w) >= 0;
  lv_label_set_text(lbl_rain_title, text[0] ? text : "没有逐小时预报");
  lv_obj_set_style_text_color(lbl_rain_title, wet ? lv_color_hex(0x4FC3F7) : COLOR_TEXT, 0);
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
    lv_obj_set_style_bg_color(b, weather_hour_wet(h) ? lv_color_hex(0x4FC3F7) : h.prob >= 30 ? lv_color_hex(0x3B5368) : COLOR_FLAT, 0);
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
    lv_obj_set_style_text_color(lbl_fc_desc[i], lv_color_hex(weather_color(d.code)), 0);
    lv_label_set_text_fmt(lbl_fc_range[i], "%d\xC2\xB0 / %d\xC2\xB0", (int)lroundf(d.temp_min), (int)lroundf(d.temp_max));
    if (d.rain_prob > 0) lv_label_set_text_fmt(lbl_fc_rain[i], "降水 %d%%", d.rain_prob);
    else lv_label_set_text(lbl_fc_rain[i], "不下雨");
    lv_obj_set_style_text_color(lbl_fc_rain[i], d.rain_prob >= 50 ? lv_color_hex(0x4FC3F7) : COLOR_SUBTLE, 0);
  }
}

void ui_set_weather(const WeatherData &w) {
  if (w.error) {
    // 只有开机后一次都没成功过才会收到错误，天气描述那行保持「获取中…」
    lv_label_set_text_fmt(lbl_humidity, "获取失败（%d）", w.error);
    lv_label_set_text_fmt(lbl_rain_title, "获取失败（%d），稍后自动重试", w.error);
    return;
  }
  if (w.ndays > 0) ui_set_sun(w.days[0].sunrise, w.days[0].sunset);
  lv_label_set_text_fmt(lbl_temp, "%d\xC2\xB0", (int)lroundf(w.temp));
  lv_label_set_text(lbl_desc, weather_text(w.code));
  lv_obj_set_style_text_color(lbl_desc, lv_color_hex(weather_color(w.code)), 0);
  lv_label_set_text_fmt(lbl_range, "%d\xC2\xB0 / %d\xC2\xB0", (int)lroundf(w.temp_min),
                        (int)lroundf(w.temp_max));
  char rain[48];
  weather_rain_short(w, rain, sizeof(rain));
  if (rain[0]) lv_label_set_text_fmt(lbl_humidity, "#4FC3F7 %s#", rain);  // 卡片上放不下两句，有雨时先说雨
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
    lv_label_set_text_fmt(lbl_gh_msg, msg, g.error);
    lv_obj_clear_flag(lbl_gh_msg, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(gh_grid, LV_OBJ_FLAG_HIDDEN);
    return;
  }

  lv_obj_add_flag(lbl_gh_msg, LV_OBJ_FLAG_HIDDEN);
  lv_obj_clear_flag(gh_grid, LV_OBJ_FLAG_HIDDEN);
  lv_label_set_text_fmt(lbl_gh_login, "@%s", g.login);
  lv_label_set_text_fmt(lbl_gh_today, "%u", g.today);
  lv_label_set_text_fmt(lbl_gh_streak, "%u", g.streak);
  lv_label_set_text_fmt(lbl_gh_total, "%u", g.total);
  lv_obj_set_style_text_color(lbl_gh_today, g.today > 0 ? lv_color_hex(kLevelColors[4]) : COLOR_TEXT, 0);

  for (int w = 0; w < kGithubWeeks; w++) {
    for (int d = 0; d < 7; d++) {
      uint8_t lv = g.level[w][d];
      lv_obj_t *c = gh_cells[w][d];
      if (lv == kGithubNoDay) {
        lv_obj_add_flag(c, LV_OBJ_FLAG_HIDDEN);
      } else {
        lv_obj_clear_flag(c, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_bg_color(c, lv_color_hex(kLevelColors[lv > 4 ? 4 : lv]), 0);
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
    lv_label_set_text_fmt(lbl_stock_msg, msg, s.error);
    lv_obj_clear_flag(lbl_stock_msg, LV_OBJ_FLAG_HIDDEN);
    for (StockRow &r : stock_rows) lv_obj_add_flag(r.row, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(lbl_stock_updated, "");
    return;
  }

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
  if (pet_buf && ((int)mood != last_mood || (int)blink != last_blink)) {
    last_mood = (int)mood;
    last_blink = blink;
    lv_color_t colors[sizeof(kPetPalette) / sizeof(kPetPalette[0])];
    for (size_t i = 0; i < sizeof(kPetPalette) / sizeof(kPetPalette[0]); i++) colors[i] = lv_color_hex(kPetPalette[i]);
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

void ui_set_wifi(bool connected) {
  for (lv_obj_t *dot : dot_wifi) {
    lv_obj_set_style_bg_color(dot, connected ? COLOR_OK : COLOR_ERR, 0);
  }
}

void ui_refresh_now() {
  lv_refr_now(nullptr);
}
