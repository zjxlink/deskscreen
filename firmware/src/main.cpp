#include <Arduino.h>
#include <ESPmDNS.h>
#include <WiFi.h>
#include <WiFiManager.h>
#include <esp_heap_caps.h>
#include <lvgl.h>

#include "audio.h"
#include "board.h"
#include "climate.h"
#include "clock_events.h"
#include "display.h"
#include "github.h"
#include "imu.h"
#include "listen.h"
#include "motion.h"
#include "net.h"
#include "night.h"
#include "ota.h"
#include "pet.h"
#include "bing.h"
#include "photos.h"
#include "pomo_stats.h"
#include "pomodoro.h"
#include "reminder.h"
#include "runner.h"
#include "screen_ctrl.h"
#include "sensors.h"
#include "settings.h"
#include "stock.h"
#include "ui.h"
#include "voice.h"
#include "weather.h"
#include "web_config.h"

static constexpr const char *kHostname = "deskscreen";
static constexpr uint32_t kWelcomeMs = 8000;      // 连上 WiFi 后显示设置地址的时间
static constexpr uint32_t kResetHoldMs = 3000;    // 长按 BOOT 键清除 WiFi
static constexpr uint32_t kShortPressMs = 1000;   // 短按 BOOT 键切换页面
static constexpr uint32_t kPomoResetMs = 1500;    // 长按番茄钟圆环重置
static constexpr uint32_t kTouchReleaseMs = 50;   // 触摸芯片偶尔漏报一次，连续这么久没读到才算松手
static constexpr uint32_t kPomoMsgMs = 60000;     // 阶段结束的提示显示多久
static constexpr uint32_t kReminderShowMs = 5 * 60000;  // 久坐、喝水提醒没人理会时多久自动关掉
static constexpr uint32_t kAlarmRingMs = 5 * 60000;     // 闹钟没人关时响多久
static constexpr uint32_t kAlarmRepeatMs = 2500;        // 闹钟铃声的间隔
static constexpr uint32_t kQuickHoldMs = 2000;          // 长按屏幕打开快捷设置
static constexpr uint32_t kQuickIdleMs = 30000;         // 快捷设置页没人操作多久自动关掉
static constexpr int kMoveSlop = 20;   // 手指移动超过这么多像素就不算点、也不算长按
static constexpr int kSwipePx = 40;    // 左右滑动超过这么多像素算翻页

static WiFiManager wm;
static String ap_name;
static uint32_t welcome_until = 0;

static ScreenCtrl screen;
static bool ota_screen = false;  // 正在显示升级进度，页面切换先暂停

static Pomodoro pomo;
static const char *pomo_msg = nullptr;  // 阶段结束时的提示文字
static uint32_t pomo_msg_until = 0;
static bool pomo_dirty = true;           // 番茄钟页需要立即刷新
static bool sound_on = true;
static bool sound_quiet_night = true;

static PomoStats pomo_stats;

static Reminder sit_reminder;
static Reminder water_reminder;

static Alarm alarm_clock;
static HourlyChime chime;
static bool alarm_ringing = false;
static uint32_t alarm_next_ring = 0;

// 室内温湿度（SENSOR 底座），已经加上校准值
static ClimateAlert climate_alert;
static RainAlert rain_alert;
static bool rain_on = true;
static int8_t temp_offset = 0;
static bool room_valid = false;
static float room_temp = 0;
static float room_humidity = 0;

// 板载加速度计：摇一摇、急刹急转、倒过来时小猫有反应
static bool imu_ok = false;
static MotionDetector motion;

// 音乐律动：听到音乐时频谱和小橘的摇摆
static constexpr uint32_t kMusicIdleMs = 60000;  // 一分钟没碰屏幕才自动切到小橘页
static MusicView music = {};
static bool music_switched = false;  // 是听到音乐自动切过来的，音乐停了切回去
static Page music_return = PAGE_CLOCK;
static uint32_t music_start = 0;
static int sway_dir = 1;          // 小橘往哪边摆，每个节拍换一次
static uint32_t last_beat_at = 0;
static PetMood reaction = PetMood::Normal;  // Normal 表示没有反应
static uint32_t reaction_start = 0;
static uint32_t reaction_until = 0;
static constexpr uint32_t kDizzyMs = 4000;
static constexpr uint32_t kStartledMs = 2500;

// 最近一次的 GitHub 和自选股数据，小猫要看
static GithubData github_data;
static StockData stock_data;

// 全屏提醒（留言、久坐、喝水）：盖在页面上，点屏幕或按 BOOT 键关掉
static bool alert_on = false;
static bool alert_timed = false;  // 久坐、喝水提醒到时自动关掉；留言一直显示到有人关掉
static uint32_t alert_until = 0;

// 快捷设置页：长按屏幕打开，点右上角、按 BOOT 键或 30 秒不操作关掉
static bool quick_on = false;
static uint32_t quick_last = 0;

// 小橘跑酷：在小猫页点小橘打开，点右上角、按 BOOT 键或 30 秒不玩关掉
static constexpr uint32_t kGameIdleMs = 30000;
static bool game_on = false;
static RunnerGame game;
static int game_best = 0;
static uint32_t game_last = 0;

// AI 聊天页：点一下开始连续对话，小橘在说时点一下打断
static constexpr uint32_t kChatStayMs = 30000;  // 回答完后至少停留这么久，留时间看字
static bool chat_has_key = false;
static uint32_t chat_busy_at = 0;

// 电子相册：照片在 TF 卡上，后台解码。在相册页时不自动轮播走；打开屏保后，空闲一段时间自动进相册，碰一下回到原来的页面
static constexpr uint32_t kAlbumRetryMs = 5000;  // 没插卡、没照片时多久再看一次
static uint8_t album_sec = 10;
static bool album_shuffle = true;
static bool saver_on = false;
static uint8_t saver_min = 5;
static bool album_local = true;
static bool album_bing = true;
static bool show_caption = true;
static const char *album_caption = "";  // 屏幕上这张图的说明（必应图片才有）
static uint32_t last_input = 0;  // 最近一次碰屏幕或按 BOOT 键
static bool saver_active = false;
static Page saver_return = PAGE_CLOCK;

// 夜间模式跟着日落开始：今天的日落时间（当天第几分钟），还没拿到天气预报时为 -1
static int sunset_min = -1;
static ScreenConfig screen_cfg;

static int night_start_of(const Settings &s) {
  return s.night_sun && sunset_min >= 0 ? sunset_min : s.night_start;
}

static void apply_settings() {
  Settings s = settings_get();
  ui_set_city(s.city.c_str());

  ScreenConfig &c = screen_cfg;
  c = ScreenConfig();
  c.day_brightness = s.brightness;
  c.night_on = s.night_on;
  c.night_start = night_start_of(s);
  c.night_end = s.night_end;
  c.night_brightness = s.night_brightness;
  c.page_seconds = s.page_seconds;
  c.page_enabled[PAGE_CLOCK] = true;
  c.page_enabled[PAGE_WEATHER] = true;
  c.page_enabled[PAGE_GITHUB] = s.github_token.length() > 0;
  c.page_enabled[PAGE_STOCKS] = s.stocks.length() > 0;
  c.page_enabled[PAGE_PET] = true;
  c.away_off = s.away_off;
  c.away_min = s.away_min;
  if (!voice_available()) c.page_unavailable |= 1u << PAGE_CHAT;  // 没有麦克风
  if (!photos_supported()) c.page_unavailable |= 1u << PAGE_ALBUM;  // 没有 TF 卡槽
  chat_has_key = s.ai_key.length() > 0;
  screen.configure(c);  // 番茄钟页、AI 聊天页和相册页不参与轮播，番茄钟进行中时固定显示
  album_sec = s.album_sec;
  album_shuffle = s.album_shuffle;
  saver_on = s.saver_on && photos_supported();
  saver_min = s.saver_min;
  album_local = s.album_local;
  album_bing = s.album_bing;
  show_caption = s.bing_caption;
  photos_set_sources(album_local, album_bing);
  ui_set_album_caption(show_caption ? album_caption : "");
  bing_kick();  // 刚打开必应时立即下载

  PomoConfig pc;
  pc.focus_min = s.pomo_focus;
  pc.short_min = s.pomo_short;
  pc.long_min = s.pomo_long;
  pc.long_every = s.pomo_every;
  pomo.configure(pc);
  pomo_dirty = true;

  ReminderConfig rc;
  rc.start = s.remind_start;
  rc.end = s.remind_end;
  rc.on = s.sit_on;
  rc.interval_min = s.sit_min;
  sit_reminder.configure(rc);
  rc.on = s.water_on;
  rc.interval_min = s.water_min;
  water_reminder.configure(rc);

  sound_on = s.sound_on;
  sound_quiet_night = s.sound_quiet_night;

  AlarmConfig ac;
  ac.on = s.alarm_on;
  ac.time = s.alarm_time;
  ac.days = s.alarm_days;
  alarm_clock.configure(ac);
  chime.set_on(s.chime_on);
  if (rain_on != s.rain_on) rain_alert.reset();
  rain_on = s.rain_on;
  listen_set_enabled(s.music_on);

  ClimateConfig cc;
  cc.on = s.climate_on;
  cc.hot_c = s.hot_c;
  cc.humid_pct = s.humid_pct;
  climate_alert.configure(cc);
  if (temp_offset != s.temp_offset && room_valid) {
    room_temp += (s.temp_offset - temp_offset) / 10.0f;  // 改了校准值，屏幕上立即按新的显示
    ui_set_room(room_temp, room_humidity, climate_is_hot(cc, room_temp), climate_is_humid(cc, room_humidity));
  }
  temp_offset = s.temp_offset;
  audio_set_volume(s.sound_volume);
}

// 当天第几分钟，还没对时返回 -1
static int local_minutes() {
  struct tm t;
  if (!getLocalTime(&t, 0) || t.tm_year < 2024 - 1900) return -1;
  return t.tm_hour * 60 + t.tm_min;
}

// 夜间时段默认不响
static void play(SoundId id) {
  if (sound_on && !(sound_quiet_night && screen.night())) audio_play(id);
}

// 今天是 1970-01-01 起第几天（北京时间），还没对时返回 -1
static int local_day() {
  time_t now = time(nullptr);
  struct tm t;
  if (!getLocalTime(&t, 0) || t.tm_year < 2024 - 1900) return -1;
  return (int)((now + 8 * 3600) / 86400);
}

static void show_alert(AlertIcon icon, const char *title, const char *body, const char *foot,
                       uint32_t show_ms) {
  uint32_t now = millis();
  alert_on = true;
  quick_on = false;  // 提醒盖掉快捷设置页和小游戏，关掉提醒后回到原来的页面
  game_on = false;
  alert_timed = show_ms > 0;
  alert_until = now + show_ms;
  if (screen.started()) screen.wake(now);
  if (!ota_screen) ui_show_alert(icon, title, body, foot);
}

static void close_alert() {
  alert_on = false;
  if (!ota_screen) ui_show_page(screen.page(), false);
}

// 设置页面发来的留言
static void on_message(const String &text, bool sound) {
  struct tm t;
  char foot[48] = "点屏幕或按 BOOT 键关闭";
  if (getLocalTime(&t, 0) && t.tm_year >= 2024 - 1900) {
    snprintf(foot, sizeof(foot), "%02d:%02d 收到 · 点屏幕或按 BOOT 键关闭", t.tm_hour, t.tm_min);
  }
  show_alert(AlertIcon::Message, "新留言", text.c_str(), foot, 0);
  if (sound) play(SoundId::Message);
}

// 触摸、设置页面的番茄钟操作都走这里
static void pomo_command(PomoCmd cmd) {
  uint32_t now = millis();
  switch (cmd) {
    case PomoCmd::Toggle: pomo.toggle(now); break;
    case PomoCmd::Skip: pomo.skip(now); break;
    case PomoCmd::Reset: pomo.reset(); break;
  }
  pomo_msg = nullptr;
  pomo_dirty = true;
}

// 推进番茄钟；阶段结束时点亮屏幕、跳到番茄钟页并提示。进行中或提示期间固定在番茄钟页
static void update_pomodoro() {
  uint32_t now = millis();
  struct tm t;
  int day = getLocalTime(&t, 0) && t.tm_year >= 2024 - 1900 ? t.tm_yday : -1;
  if (pomo.tick(now, day)) {
    bool focus_done = pomo.phase() != PomoPhase::Focus;
    pomo_msg = focus_done ? "专注完成，休息一下吧" : "休息结束，点圆环开始";
    pomo_msg_until = now + kPomoMsgMs;
    pomo_dirty = true;
    play(focus_done ? SoundId::FocusDone : SoundId::BreakDone);
    if (focus_done) {
      pomo_stats.record(local_day());
      settings_save_pomo_stats(pomo_stats.data());
      sit_reminder.restart(now);  // 休息时会起身，久坐提醒重新计时
    }
    if (screen.started()) {
      screen.wake(now);
      screen.go_to(PAGE_POMODORO, now);
    }
  }
  if (pomo_msg && (int32_t)(now - pomo_msg_until) >= 0) {
    pomo_msg = nullptr;
    pomo_dirty = true;
  }
  if (pomo.running() || pomo_msg) {
    screen.pin(PAGE_POMODORO);
  } else {
    screen.unpin();
  }

  // 秒数变了或状态变了才重画
  static uint32_t last_secs = UINT32_MAX;
  uint32_t secs = (pomo.remaining_ms(now) + 999) / 1000;
  if (secs != last_secs || pomo_dirty) {
    last_secs = secs;
    pomo_dirty = false;
    int day = local_day();
    ui_set_pomodoro(pomo, now, pomo_msg, day >= 0 ? pomo_stats.count(day) : pomo.done_today());
  }
}

// 久坐、喝水提醒。专注中先不打扰，等休息时再提醒
static void update_reminders() {
  uint32_t now = millis();
  int minutes = local_minutes();
  bool hold = pomo.phase() == PomoPhase::Focus && pomo.running();
  bool sit = sit_reminder.tick(now, minutes, hold);
  bool water = water_reminder.tick(now, minutes, hold);
  // 留言还没看就不盖掉它，这次提醒跳过，下一轮再说
  if ((sit || water) && !(alert_on && !alert_timed) && !alarm_ringing) {
    char body[96];
    const char *title;
    AlertIcon icon;
    if (sit && water) {
      title = "起来走走，顺便喝杯水";
      icon = AlertIcon::Sit;
      snprintf(body, sizeof(body), "已经坐了 %d 分钟了\n站起来活动一下，再去倒杯水吧", sit_reminder.config().interval_min);
    } else if (sit) {
      title = "该起来活动一下了";
      icon = AlertIcon::Sit;
      snprintf(body, sizeof(body), "已经坐了 %d 分钟了\n站起来走走，伸个懒腰，看看远处", sit_reminder.config().interval_min);
    } else {
      title = "该喝水了";
      icon = AlertIcon::Water;
      snprintf(body, sizeof(body), "每 %d 分钟喝一杯水\n小口慢慢喝，别等口渴了再喝", water_reminder.config().interval_min);
    }
    show_alert(icon, title, body, "点屏幕或按 BOOT 键关闭", kReminderShowMs);
    play(SoundId::Reminder);
  }
  if (alert_on && alert_timed && (int32_t)(now - alert_until) >= 0) close_alert();
}

// 新的天气预报到了：快下雨时提醒一次。夜间时段、留言没看、闹钟在响时先不提醒，下次拿到预报时再看
static void on_weather(const WeatherData &w) {
  Settings s = settings_get();
  // 日落时间每天变一点，夜间模式的开始时间跟着改
  int sunset = !w.error && w.ndays > 0 ? w.days[0].sunset : -1;
  if (sunset >= 0 && sunset != sunset_min) {
    sunset_min = sunset;
    screen_cfg.night_start = night_start_of(s);
    screen.configure(screen_cfg);
  }

  int minutes = local_minutes();
  bool quiet = !rain_on || minutes < 0 || night_is_active(minutes, night_start_of(s), s.night_end) ||
               (alert_on && !alert_timed) || alarm_ringing;
  if (!rain_alert.update(w, quiet)) return;
  char body[96];
  weather_rain_alert_text(w, body, sizeof(body));
  show_alert(AlertIcon::Rain, weather_rain_index(w) == 0 ? "外面在下雨" : "快下雨了", body, "点屏幕或按 BOOT 键关闭",
             kReminderShowMs);
  play(SoundId::Reminder);
}

// 整点报时和闹钟。闹钟夜里也响，不受提示音开关影响；响铃期间每隔几秒响一次，关掉提醒就停
static void update_clock_events() {
  uint32_t now = millis();
  struct tm t;
  bool synced = getLocalTime(&t, 0) && t.tm_year >= 2024 - 1900;
  int day = synced ? local_day() : -1;
  int minutes = synced ? t.tm_hour * 60 + t.tm_min : -1;
  if (alarm_clock.tick(day, minutes, synced ? t.tm_wday : -1)) {
    char body[48];
    snprintf(body, sizeof(body), "%02d:%02d 到了", t.tm_hour, t.tm_min);
    show_alert(AlertIcon::Alarm, "闹钟", body, "点屏幕或按 BOOT 键关闭", kAlarmRingMs);
    alarm_ringing = true;
    alarm_next_ring = now;
  } else if (chime.tick(day, minutes) && !alarm_ringing) {
    play(SoundId::Chime);
  }
  if (alarm_ringing && !alert_on) alarm_ringing = false;  // 被关掉或者到时自动关了
  if (alarm_ringing && (int32_t)(now - alarm_next_ring) >= 0) {
    audio_play(SoundId::Alarm);
    alarm_next_ring = now + kAlarmRepeatMs;
  }
}

// SENSOR 底座：雷达决定屏幕亮灭，温湿度显示在时钟页，太热太闷时提醒。没有底座时什么也不做
static void update_room() {
  if (!sensors_dock()) return;
  uint32_t now = millis();
  screen.set_presence(sensors_presence(), now);

  float t, h;
  if (!sensors_take_climate(t, h)) return;
  room_valid = true;
  room_temp = t + temp_offset / 10.0f;
  room_humidity = h;
  const ClimateConfig &cc = climate_alert.config();
  ui_set_room(room_temp, room_humidity, climate_is_hot(cc, room_temp), climate_is_humid(cc, room_humidity));

  // 夜里不吵人；已经有提醒在显示时等它关掉再说
  ClimateEvent ev = climate_alert.tick(now, room_temp, room_humidity, screen.night() || alert_on);
  if (ev == ClimateEvent::None) return;
  char body[96];
  if (ev == ClimateEvent::Hot) {
    snprintf(body, sizeof(body), "室内 %.1f\xC2\xB0" "C，超过了 %d\xC2\xB0" "C\n开窗通通风，或者开空调降降温吧", room_temp, cc.hot_c);
    show_alert(AlertIcon::Climate, "有点热了", body, "点屏幕或按 BOOT 键关闭", kReminderShowMs);
  } else {
    snprintf(body, sizeof(body), "室内湿度 %d%%，超过了 %d%%\n开窗通通风，或者开个除湿吧", (int)lroundf(room_humidity), cc.humid_pct);
    show_alert(AlertIcon::Climate, "有点闷了", body, "点屏幕或按 BOOT 键关闭", kReminderShowMs);
  }
  play(SoundId::Reminder);
}

// 设置页上显示的底座状态
static void room_info(RoomInfo &info) {
  info.dock = sensors_dock();
  info.valid = info.dock && room_valid;
  info.temp_c = room_temp;
  info.humidity = (int)lroundf(room_humidity);
  info.presence = sensors_presence();
  info.away = screen.away();
}

static void react(PetMood mood, uint32_t ms, bool show) {
  uint32_t now = millis();
  reaction = mood;
  reaction_start = now;
  reaction_until = now + ms;
  if (!screen.started()) return;
  screen.activity(now);  // 有人在动它，当然有人
  // 摇一摇、倒过来时跳到小猫页让你看到；急刹车只在已经显示小猫时有反应，开车时不乱翻页
  if (show && !alert_on && !quick_on && !game_on && !ota_screen) screen.go_to(PAGE_PET, now);
}

// 每 20ms 读一次加速度
static void update_motion() {
  static uint32_t last = 0;
  uint32_t now = millis();
  if (!imu_ok || now - last < 20) return;
  last = now;
  float x, y, z;
  if (!imu_read(x, y, z)) return;
  bool was_upside = motion.upside_down();
  MotionEvent ev = motion.update(now, x, y, z);
  if (ev == MotionEvent::Shake) {
    react(PetMood::Dizzy, kDizzyMs, true);
  } else if (ev == MotionEvent::Jolt && reaction != PetMood::Dizzy) {
    react(PetMood::Startled, kStartledMs, false);
  }
  if (motion.upside_down() && !was_upside) react(PetMood::Flipped, 0, true);
  if (reaction == PetMood::Flipped && !motion.upside_down()) {
    react(PetMood::Startled, kStartledMs, false);  // 放正了，惊魂未定
  } else if (reaction != PetMood::Normal && reaction != PetMood::Flipped && (int32_t)(now - reaction_until) >= 0) {
    reaction = motion.upside_down() ? PetMood::Flipped : PetMood::Normal;
  }
}

// 音乐律动：听到音乐、又一分钟没人碰屏幕时切到小橘页；音乐停了，如果是自动切过来的就切回去。
// 番茄钟、提醒、聊天、相册、夜间和熄屏时不切
static void update_music() {
  uint32_t now = millis();
  bool was = music.playing;
  bool beat_pending = music.beat;
  listen_get(music);
  music.beat = music.beat || beat_pending;  // 留给小猫页用（它每 50ms 才更新一次）
  if (music.playing && !was) {
    music_start = now;
    Page p = screen.page();
    if (screen.started() && p != PAGE_PET && p != PAGE_POMODORO && p != PAGE_CHAT && p != PAGE_ALBUM &&
        now - last_input >= kMusicIdleMs && !screen.night() && !screen.dimmed() && !alert_on && !quick_on &&
        !game_on && !ota_screen && !pomo.running()) {
      music_return = p;
      music_switched = true;
      screen.go_to(PAGE_PET, now);
    }
  } else if (!music.playing && was) {
    if (music_switched && screen.page() == PAGE_PET && last_input < music_start && !alert_on && !quick_on &&
        !game_on && !ota_screen)
      screen.go_to(music_return, now);
    music_switched = false;
  }
  if (music.playing && screen.page() == PAGE_PET) screen.stay(now);  // 放音乐时不轮播走
}

// 像素小猫：只在小猫页显示时更新，每 100ms 一帧（放音乐时 50ms，跟得上节拍）
static void update_pet() {
  static uint32_t last = 0;
  static bool showing_music = false;
  uint32_t now = millis();
  if (screen.page() != PAGE_PET || alert_on || quick_on || game_on || ota_screen ||
      now - last < (music.playing ? 50u : 100u))
    return;
  last = now;

  PetInputs in;
  in.night = screen.night();
  in.minutes = local_minutes();
  in.github_ok = github_data.status == GithubStatus::Ok;
  in.github_today = github_data.today;
  int n = 0;
  float sum = 0;
  if (stock_data.status == StockStatus::Ok) {
    for (int i = 0; i < stock_data.count; i++) {
      if (!stock_data.quotes[i].valid) continue;
      sum += stock_data.quotes[i].change_pct;
      n++;
    }
  }
  in.stocks_ok = n > 0;
  in.stock_avg_pct = n ? sum / n : 0;
  int day = local_day();
  in.pomo_today = day >= 0 ? pomo_stats.count(day) : pomo.done_today();

  bool dancing = music.playing && reaction == PetMood::Normal && !in.night;
  PetMood mood = reaction != PetMood::Normal ? reaction : dancing ? PetMood::Happy : pet_mood(in);
  char line[96];
  if (dancing) snprintf(line, sizeof(line), "听到音乐啦，跟着节拍摇起来");
  else pet_line(mood, in, line, sizeof(line));

  // 底部一行：今天的提交、番茄和自选股
  char status[96] = "";
  size_t len = 0;
  if (in.github_ok) len += snprintf(status + len, sizeof(status) - len, "提交 %d · ", in.github_today);
  len += snprintf(status + len, sizeof(status) - len, "番茄 %d", in.pomo_today);
  if (in.stocks_ok && len < sizeof(status)) snprintf(status + len, sizeof(status) - len, " · 自选股 %+.1f%%", in.stock_avg_pct);

  bool sleeping = mood == PetMood::Sleep;
  bool blink = !sleeping && now % 4000 < 150;  // 每 4 秒眨一次眼
  ui_set_pet(mood, blink, line, status);

  if (music.playing) {
    ui_set_pet_music(music.bands);
    showing_music = true;
  } else if (showing_music) {
    ui_set_pet_music(nullptr);
    showing_music = false;
  }

  int bob, dx = 0;
  uint32_t since = now - reaction_start;
  if (dancing) {
    // 每个节拍往另一边摆，摆过去的路上跳一下
    if (music.beat) {
      sway_dir = -sway_dir;
      last_beat_at = now;
      music.beat = false;
    }
    uint32_t t = now - last_beat_at;
    int reach = t < 150 ? (int)(t * 8 / 150) : 8;
    dx = sway_dir * reach;
    bob = t < 120 ? -6 : 0;
  } else if (mood == PetMood::Dizzy) {
    dx = (now / 120) % 2 ? 6 : -6;  // 晃来晃去
    bob = (now / 240) % 2 * 3;
  } else if (mood == PetMood::Startled) {
    bob = since < 300 ? -12 : 0;  // 吓得跳起来
  } else if (mood == PetMood::Flipped) {
    dx = (now / 200) % 2 ? 3 : -3;  // 挣扎
    bob = (now / 400) % 2 * -4;
  } else if (sleeping) {
    bob = (now / 1500) % 2 * 2;  // 睡觉时慢慢呼吸
  } else if (mood == PetMood::Happy) {
    bob = (now / 300) % 2 * -6;  // 开心得蹦起来
  } else {
    bob = (now / 800) % 2 * 3;
  }
  ui_pet_anim(dx, bob, sleeping ? (int)(now / 800 % 2) : -1);
}

// 把 ScreenCtrl 的状态同步到屏幕：亮度和当前页面
static void update_screen() {
  static int last_brightness = -1;
  screen.tick(millis(), local_minutes());
  if (screen.brightness() != last_brightness) {
    last_brightness = screen.brightness();
    display_set_brightness(last_brightness);
  }
  Page p;
  bool animate, back;
  // 升级进度和全屏提醒期间页面照常计算，只是先不显示
  if (screen.take_page_change(p, animate, &back) && !ota_screen && !alert_on && !quick_on && !game_on)
    ui_show_page(p, animate, back);
}

// 在线升级或网页上传固件时显示进度；成功后重启，失败时提示几秒再回到原来的页面
static void update_ota_screen() {
  static int last_progress = -1;
  OtaStatus s = ota_status();
  if (s.state == OtaState::Downloading) {
    if (!ota_screen || s.progress != last_progress) {
      ota_screen = true;
      quick_on = false;  // 升级完回到原来的页面
      game_on = false;
      last_progress = s.progress;
      char detail[48];
      snprintf(detail, sizeof(detail), "请不要断电（%d%%）", s.progress);
      ui_show_progress("正在升级固件", detail, s.progress);
    }
  } else if (s.state == OtaState::Done) {
    ui_show_progress("升级完成", "正在重启…", 100);
    ui_refresh_now();
    delay(1500);
    ESP.restart();
  } else if (ota_screen) {
    ota_screen = false;
    last_progress = -1;
    ui_show_status("升级失败", s.error);
    ui_refresh_now();
    delay(3000);
    if (alert_on) {
      close_alert();  // 提醒的内容已经被升级画面盖掉了，直接回到页面
    } else {
      ui_show_page(screen.page(), false);
    }
  }
}

static void connect_wifi() {
  uint8_t mac[6];
  WiFi.macAddress(mac);
  char name[32];
  snprintf(name, sizeof(name), "DeskScreen-%02X%02X", mac[4], mac[5]);
  ap_name = name;

  WiFi.setHostname(kHostname);
  wm.setTitle("桌面信息屏");
  wm.setConfigPortalTimeout(300);
  wm.setAPCallback([](WiFiManager *) {
    ui_show_provision(ap_name.c_str());
    ui_refresh_now();
  });

  if (!wm.autoConnect(ap_name.c_str())) {
    ui_show_status("配网超时", "5 分钟内没有完成配置\n即将重启后重试");
    ui_refresh_now();
    delay(3000);
    ESP.restart();
  }
}

static void show_quick() {
  Settings s = settings_get();
  String url = WiFi.isConnected() ? "http://" + WiFi.localIP().toString() : "";
  QuickState q;
  q.brightness = s.brightness;
  q.sound_on = s.sound_on;
  q.has_dock = sensors_dock();
  q.away_off = s.away_off;
  q.url = url.c_str();
  ui_show_quick(q);
}

static void open_quick(uint32_t now) {
  quick_on = true;
  quick_last = now;
  screen.activity(now);
  show_quick();
}

static void close_quick() {
  quick_on = false;
  ui_show_page(screen.page(), false);
}

// 快捷设置页上点了一下：改完立即保存，和在网页上改一样
static void quick_tap(int x, int y, uint32_t now) {
  quick_last = now;
  bool was_dimmed = screen.dimmed();
  screen.activity(now);
  if (was_dimmed) return;  // 屏幕调暗时第一下只点亮
  QuickHit h = ui_quick_hit(x, y);
  if (h == QuickHit::Close) {
    close_quick();
    return;
  }
  Settings s = settings_get();
  switch (h) {
    case QuickHit::BrightDown: s.brightness = s.brightness > 35 ? s.brightness - 25 : 10; break;
    case QuickHit::BrightUp: s.brightness = s.brightness < 230 ? s.brightness + 25 : 255; break;
    case QuickHit::Sound: s.sound_on = !s.sound_on; break;
    case QuickHit::Away:
      if (!sensors_dock()) return;
      s.away_off = !s.away_off;
      break;
    default: return;
  }
  settings_save(s);
  apply_settings();
  show_quick();
}

static void update_quick() {
  if (quick_on && millis() - quick_last >= kQuickIdleMs) close_quick();
}

static void update_chat() {
  static uint32_t last = 0;
  uint32_t now = millis();
  if (now - last < 50) return;
  last = now;
  VoiceStatus st = voice_status();
  bool busy = st.state == VoiceState::Connecting || st.state == VoiceState::Listening ||
              st.state == VoiceState::Hearing || st.state == VoiceState::Thinking || st.state == VoiceState::Speaking;
  bool on_page = screen.page() == PAGE_CHAT && !alert_on && !quick_on && !game_on && !ota_screen;
  if (busy && screen.page() != PAGE_CHAT) {
    voice_cancel();  // 切到别的页面就不聊了
    return;
  }
  if (busy) chat_busy_at = now;
  if (screen.page() == PAGE_CHAT && chat_busy_at && now - chat_busy_at < kChatStayMs) {
    screen.stay(now);  // 聊天中和刚回答完不自动轮播走
    static uint32_t last_activity = 0;
    if (busy && now - last_activity > 5000) {
      last_activity = now;
      screen.activity(now);  // 也不调暗
    }
  }
  if (!on_page) return;
  ChatView v;
  v.state = st.state;
  v.level = st.level;
  v.latency_ms = st.latency_ms;
  v.idle_left_ms = st.idle_left_ms;
  v.heard = st.heard;
  v.reply = st.reply;
  v.error = st.error;
  v.has_key = chat_has_key;
  v.blink = st.state != VoiceState::Thinking && now % 4000 < 150;
  v.bob = st.state == VoiceState::Speaking ? (now / 250) % 2 * -4 : 0;
  ui_set_chat(v);
}

static void open_game(uint32_t now) {
  game_on = true;
  game_last = now;
  game = RunnerGame(micros());
  screen.activity(now);
  ui_show_game();
  ui_set_game(game, game_best);
}

static void close_game() {
  game_on = false;
  ui_show_page(screen.page(), false);
}

// 按下屏幕就起跳，比松手再跳跟手
static void game_press(int x, int y, uint32_t now) {
  bool was_dimmed = screen.dimmed();
  game_last = now;
  screen.activity(now);
  if (was_dimmed) return;
  if (ui_game_close_hit(x, y)) {
    close_game();
    return;
  }
  game.tap(now);
}

// 游戏逻辑每圈都跑，画面约 30 帧
static void update_game() {
  static uint32_t last_draw = 0;
  static uint32_t last_activity = 0;
  if (!game_on) return;
  uint32_t now = millis();
  RunnerGame::State before = game.state();
  game.tick(now);
  if (game.state() == RunnerGame::State::Playing) {
    game_last = now;
    if (now - last_activity > 5000) {
      last_activity = now;
      screen.activity(now);  // 玩的时候不调暗、不翻页
    }
  } else if (before == RunnerGame::State::Playing && game.score() > game_best) {
    game_best = game.score();
    settings_save_game_best(game_best);
  }
  if (game.state() != RunnerGame::State::Playing && now - game_last >= kGameIdleMs) {
    close_game();
    return;
  }
  if (now - last_draw >= 33 || game.state() != before) {
    last_draw = now;
    ui_set_game(game, game_best);
  }
}

// 空闲够久就进相册当屏保。番茄钟、聊天、提醒、夜间和熄屏时都不打扰
static void update_saver(uint32_t now) {
  if (!saver_on || saver_active || !screen.started() || now - last_input < saver_min * 60000UL) return;
  if (screen.page() == PAGE_ALBUM || screen.night() || screen.dimmed() || alert_on || quick_on || game_on ||
      ota_screen || pomo.running() || pomo_msg || (chat_busy_at && now - chat_busy_at < kChatStayMs))
    return;
  if (photos_status().count == 0) return;
  saver_return = screen.page();
  saver_active = true;
  screen.go_to(PAGE_ALBUM, now);
}

static const char *album_message(const PhotoStatus &st, bool has_photo) {
  if (st.count > 0) {
    if (has_photo) return nullptr;
    if (photos_busy() || st.failed < st.count) return "正在读取照片…";
    return "#E6EDF3 照片都读不出来#\n\n支持 JPG、PNG、GIF 和 MJPEG 视频（.avi），HEIC、MP4 和渐进式 JPEG 读不了。用网页设置上传会自动转换好";
  }
  if (!album_local && !album_bing)
    return "#E6EDF3 图片来源都关掉了#\n\n在网页设置的「显示 › 相册与屏保」里打开自己的照片或必应每日一图";
  if (!album_local) {
    return WiFi.isConnected() ? "#E6EDF3 正在下载必应每日一图…#\n\n下载好后会在这里播放"
                              : "#E6EDF3 还没有必应每日一图#\n\n连上网后会自动下载";
  }
  switch (st.card) {
    case PhotoCard::NoSlot:
    case PhotoCard::NoCard:
      if (photos_busy()) return "正在读取 TF 卡…";
      return album_bing ? "#E6EDF3 没有检测到 TF 卡#\n\n必应每日一图下载好后会先在这里播放。插入 FAT32 格式的 TF 卡，就能放自己的照片，也能存下更多必应图片"
                        : "#E6EDF3 没有检测到 TF 卡#\n\n插入 FAT32 格式的 TF 卡，照片放在卡里的 photos 文件夹，也可以在网页设置里用手机上传";
    case PhotoCard::Empty:
    case PhotoCard::Ok:
      break;
  }
  return album_bing ? "#E6EDF3 TF 卡里还没有照片#\n\n把照片放进卡里的 photos 文件夹，或者在网页设置里用手机上传。必应每日一图下载好后也会在这里播放"
                    : "#E6EDF3 TF 卡里还没有照片#\n\n把照片、动图或视频放进卡里的 photos 文件夹，或者在网页设置里用手机上传";
}

// 相册页：按间隔请后台解码下一张，解码好了就换上。动图和视频一帧一帧地换，放完马上请下一张
static void update_album() {
  static bool has_photo = false;
  static bool last_anim = false;
  static uint32_t shown_at = 0;
  static uint32_t last_req = 0;
  static const char *last_msg = "";
  uint32_t now = millis();
  if (saver_active && screen.page() != PAGE_ALBUM) saver_active = false;  // 被番茄钟、提醒、夜间模式等切走了
  update_saver(now);
  static bool arrows = true;
  if (arrows == saver_active) {
    arrows = !saver_active;
    ui_set_album_arrows(arrows);  // 当屏保时照片上不放翻页提示
  }

  const uint16_t *px;
  const char *caption;
  bool anim;
  if (photos_take_frame(px, caption, anim)) {
    ui_set_album_photo(px);
    last_anim = anim;
    album_caption = caption;
    ui_set_album_caption(show_caption ? caption : "");
    has_photo = true;
    shown_at = now;
  }
  PhotoStatus st = photos_status();
  if (st.count == 0 && has_photo) {
    has_photo = false;  // 卡被拔了、照片都删了，或者来源都关了
    ui_set_album_photo(nullptr);
    album_caption = "";
    ui_set_album_caption("");
  }
  const char *msg = album_message(st, has_photo);
  if (msg != last_msg) {
    last_msg = msg;
    ui_set_album_msg(msg);
  }

  bool on_page = screen.page() == PAGE_ALBUM && !alert_on && !quick_on && !game_on && !ota_screen;
  if (!on_page) {
    photos_stop();  // 不在相册页时不放动图和视频，回来时换下一个
    return;
  }
  if (has_photo) screen.stay(now);  // 有照片时一直放，不轮播到别的页面
  uint32_t wait = has_photo ? (last_anim ? 0 : album_sec * 1000UL) : last_req ? kAlbumRetryMs : 0;
  if (!photos_busy() && now - (has_photo ? shown_at : last_req) >= wait) {
    photos_request_next(album_shuffle, album_sec * 1000UL);
    last_req = now;
  }
}

// 点屏幕或短按 BOOT 键：有全屏提醒时先关掉提醒（屏幕调暗时第一下只点亮），否则切换页面。
// back 为 true 时切到上一页（点屏幕左半边）；BOOT 键总是下一页
static void on_tap(uint32_t now, bool back = false) {
  if (saver_active && !alert_on && !quick_on && !ota_screen) {
    saver_active = false;  // 屏保：碰一下回到原来的页面
    screen.activity(now);
    screen.go_to(saver_return, now);
    return;
  }
  if (quick_on) {
    close_quick();
    return;
  }
  if (game_on) {
    close_game();
    return;
  }
  if (alert_on && !ota_screen && !screen.dimmed()) {
    close_alert();
    return;
  }
  screen.on_input(now, back);
}

// 短按切换页面，长按 3 秒清除 WiFi
static void check_button() {
  static uint32_t pressed_at = 0;
  if (digitalRead(PIN_BUTTON) == LOW) {
    if (pressed_at == 0) pressed_at = millis();
    last_input = millis();
    if (millis() - pressed_at > kResetHoldMs) {
      ui_show_status("重新配置 WiFi", "已清除 WiFi 信息，正在重启…");
      ui_refresh_now();
      wm.resetSettings();
      delay(1000);
      ESP.restart();
    }
  } else {
    if (pressed_at != 0 && millis() - pressed_at < kShortPressMs) on_tap(millis());
    pressed_at = 0;
  }
}

// 点一下屏幕切换页面（松手时触发）：点左半边回到上一页，点右半边到下一页；也可以左右滑动翻页
// （手指往左滑到下一页，往右滑回上一页，和手机一样）；按住 2 秒打开快捷设置。
// 番茄钟页的圆环例外：松手时开始/暂停，按住 1.5 秒重置。小猫页点小橘打开小游戏
static void check_touch() {
  static bool was_touched = false;
  static bool on_ring = false;
  static bool long_done = false;
  static uint32_t down_at = 0;
  static int down_x = 0, down_y = 0;
  static int last_x = 0, last_y = 0;
  static bool moved = false;
  static uint32_t last_poll = 0;
  static uint32_t last_seen = 0;
  uint32_t now = millis();
  if (now - last_poll < 30) return;  // 每次读触摸都要走 I2C，没必要太频繁
  last_poll = now;

  int x = 0, y = 0;
  bool raw = display_touch(x, y);
  if (raw) last_seen = now;
  // 按住时漏报一次不当成松手再按下，否则一下会被算成两下，番茄钟也会开始又暂停
  bool touched = raw || (was_touched && now - last_seen < kTouchReleaseMs);
  if (touched) last_input = now;
  if (raw && was_touched) {
    last_x = x;
    last_y = y;
    if (abs(x - down_x) > kMoveSlop || abs(y - down_y) > kMoveSlop) moved = true;
  }
  if (touched && !was_touched) {
    down_at = now;
    down_x = last_x = x;
    down_y = last_y = y;
    moved = false;
    long_done = false;
    if (game_on) {
      long_done = true;  // 小游戏里按下就起跳，松手和长按都不再算
      game_press(x, y, now);
      was_touched = touched;
      return;
    }
    if (screen.started() && !screen.dimmed() && !ota_screen && !alert_on && !quick_on &&
        screen.page() == PAGE_CHAT && ui_chat_button_hit(x, y)) {
      long_done = true;  // 按下就算：开始、挂断或打断；不算翻页，也不打开快捷设置
      chat_busy_at = now;
      screen.activity(now);
      voice_tap();
      was_touched = touched;
      return;
    }
    // 屏幕调暗时第一下只点亮，交给 ScreenCtrl 处理
    on_ring = screen.started() && !screen.dimmed() && !ota_screen && !alert_on && !quick_on &&
              screen.page() == PAGE_POMODORO && ui_pomodoro_hit(x, y);
  } else if (touched && !long_done && !moved && now - down_at >= (on_ring ? kPomoResetMs : kQuickHoldMs)) {
    // 有提醒或在快捷设置页时长按不算数，松手时照常当成点一下
    if (on_ring) {
      long_done = true;
      pomo_command(PomoCmd::Reset);
    } else if (screen.started() && !ota_screen && !alert_on && !quick_on) {
      long_done = true;
      open_quick(now);
    }
  } else if (!touched && was_touched && !long_done && moved) {
    // 滑动：横着滑够远才翻页，竖着滑或者滑得太短什么也不做
    int dx = last_x - down_x;
    if (!quick_on && abs(dx) >= kSwipePx && abs(dx) > abs(last_y - down_y)) on_tap(now, dx > 0);
  } else if (!touched && was_touched && !long_done) {
    if (on_ring) {
      pomo_command(PomoCmd::Toggle);
    } else if (quick_on) {
      quick_tap(down_x, down_y, now);
    } else if (screen.started() && !screen.dimmed() && !ota_screen && !alert_on && screen.page() == PAGE_PET &&
               ui_pet_hit(down_x, down_y)) {
      open_game(now);
    } else {
      on_tap(now, down_x < SCREEN_W / 2);
    }
  }
  was_touched = touched;
}

void setup() {
  Serial.begin(115200);
  net_tls_use_psram();
  pinMode(PIN_BUTTON, INPUT_PULLUP);

  settings_begin();
  PomoStatsData sd;
  settings_load_pomo_stats(sd);
  pomo_stats.load(sd);
  game_best = settings_load_game_best();
  display_init();
  audio_begin();  // 解码芯片和触摸共用 I2C，要在屏幕初始化之后
  voice_begin();
  listen_begin();
  sensors_begin();
  imu_ok = imu_begin();
  photos_begin();
  ui_init();
  apply_settings();
  update_screen();  // 还没对时，先用白天亮度

  ui_show_status("桌面信息屏", "正在连接 WiFi…");
  ui_refresh_now();
  connect_wifi();

  configTzTime(TIMEZONE, "ntp.aliyun.com", "cn.pool.ntp.org", "pool.ntp.org");
  MDNS.begin(kHostname);
  MDNS.addService("http", "tcp", 80);
  web_config_begin();
  web_config_set_pomodoro(&pomo, pomo_command);
  web_config_set_pomo_stats(&pomo_stats, local_day);
  web_config_set_message_hook(on_message);
  web_config_set_room_info(room_info);
  // 网页上传固件期间主循环被阻塞，由上传处理顺带刷新进度画面
  web_config_set_progress_hook([] {
    update_ota_screen();
    ui_refresh_now();
  });
  weather_begin();
  github_begin();
  stock_begin();
  ota_begin();
  bing_begin();
  log_i("heap: internal %u (largest block %u), psram %u", heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
        heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL), heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

  String detail = "用同一 WiFi 下的手机或电脑打开\n#58A6FF http://" + WiFi.localIP().toString() +
                  "#\n或 #58A6FF http://" + kHostname + ".local#\n即可设置城市、亮度和 GitHub";
  ui_show_status("已连接 WiFi", detail.c_str());
  welcome_until = millis() + kWelcomeMs;
}

void loop() {
  display_check_panel();
  lv_timer_handler();
  web_config_loop();
  check_button();
  check_touch();

  if (welcome_until && (int32_t)(millis() - welcome_until) >= 0) {
    welcome_until = 0;
    screen.start(millis());
    last_input = millis();
  }
  if (web_config_take_changed()) apply_settings();
  update_pomodoro();
  update_reminders();
  update_clock_events();
  sensors_loop();
  update_room();
  update_motion();
  update_music();
  update_pet();
  update_quick();
  update_game();
  update_chat();
  update_album();
  update_screen();
  update_ota_screen();

  WeatherData w;
  if (weather_take_update(w)) {
    ui_set_weather(w);
    on_weather(w);
  }

  if (github_take_update(github_data)) ui_set_github(github_data);
  if (stock_take_update(stock_data)) ui_set_stocks(stock_data);

  static bool last_wifi = false;
  bool wifi = WiFi.isConnected();
  if (wifi != last_wifi) {
    last_wifi = wifi;
    ui_set_wifi(wifi);
  }

  ui_tick_clock();
  delay(5);
}
