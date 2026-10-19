#include "ui.h"

#include <Arduino.h>
#include <lvgl.h>
#include <time.h>

#include "board.h"

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

// 热力图格子尺寸：24 周 x 13px = 309px，正好放进 320 宽的屏幕
static constexpr int kCell = 10;
static constexpr int kGap = 3;

// 中文字体由 tools/gen_fonts.py 生成
LV_FONT_DECLARE(font_cn_16);
LV_FONT_DECLARE(font_cn_24);

static lv_obj_t *scr_status;
static lv_obj_t *status_title;
static lv_obj_t *status_detail;

static lv_obj_t *scr_pages[PAGE_COUNT];

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

static lv_obj_t *lbl_gh_login;
static lv_obj_t *lbl_gh_today;
static lv_obj_t *lbl_gh_streak;
static lv_obj_t *lbl_gh_total;
static lv_obj_t *gh_grid;
static lv_obj_t *gh_cells[kGithubWeeks][7];
static lv_obj_t *lbl_gh_msg;

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
  lv_obj_align(lbl_time, LV_ALIGN_TOP_MID, -14, 46);

  lbl_sec = make_label(scr_main, &lv_font_montserrat_20, COLOR_SUBTLE);
  lv_label_set_text(lbl_sec, "--");
  lv_obj_align_to(lbl_sec, lbl_time, LV_ALIGN_OUT_RIGHT_BOTTOM, 4, -6);

  // 底部：天气卡片
  lv_obj_t *card = lv_obj_create(scr_main);
  lv_obj_set_size(card, SCREEN_W - 24, 96);
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
  lv_obj_align(lbl_humidity, LV_ALIGN_BOTTOM_LEFT, 140, 0);
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

void ui_init() {
  build_status();
  build_main();
  scr_pages[PAGE_CLOCK] = scr_main;
  build_github();
}

void ui_show_status(const char *title, const char *detail) {
  lv_label_set_text(status_title, title);
  lv_label_set_text(status_detail, detail);
  lv_obj_align(status_title, LV_ALIGN_TOP_MID, 0, 40);
  if (lv_scr_act() != scr_status) lv_scr_load(scr_status);
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

void ui_show_page(Page page, bool animate) {
  lv_obj_t *scr = scr_pages[page];
  if (lv_scr_act() == scr) return;
  if (animate) {
    lv_scr_load_anim(scr, LV_SCR_LOAD_ANIM_MOVE_LEFT, 250, 0, false);
  } else {
    lv_scr_load(scr);
  }
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
}

void ui_set_city(const char *city) {
  lv_label_set_text(lbl_city, city);
  lv_obj_align_to(lbl_city, dot_wifi[PAGE_CLOCK], LV_ALIGN_OUT_LEFT_MID, -8, 0);
}

void ui_set_weather(const WeatherData &w) {
  lv_label_set_text_fmt(lbl_temp, "%d\xC2\xB0", (int)lroundf(w.temp));
  lv_label_set_text(lbl_desc, weather_text(w.code));
  lv_obj_set_style_text_color(lbl_desc, lv_color_hex(weather_color(w.code)), 0);
  lv_label_set_text_fmt(lbl_range, "%d\xC2\xB0 / %d\xC2\xB0", (int)lroundf(w.temp_min),
                        (int)lroundf(w.temp_max));
  lv_label_set_text_fmt(lbl_humidity, "湿度 %d%%", w.humidity);
}

void ui_set_github(const GithubData &g) {
  const char *msg = nullptr;
  switch (g.status) {
    case GithubStatus::NoToken: msg = "请在设置页面填写 GitHub Token"; break;
    case GithubStatus::Loading: msg = "获取中…"; break;
    case GithubStatus::BadToken: msg = "Token 无效或已过期\n请在设置页面重新填写"; break;
    case GithubStatus::Error: msg = "获取失败，稍后自动重试"; break;
    case GithubStatus::Ok: break;
  }
  if (msg) {
    lv_label_set_text(lbl_gh_msg, msg);
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

void ui_set_wifi(bool connected) {
  for (lv_obj_t *dot : dot_wifi) {
    lv_obj_set_style_bg_color(dot, connected ? COLOR_OK : COLOR_ERR, 0);
  }
}

void ui_refresh_now() {
  lv_refr_now(nullptr);
}
