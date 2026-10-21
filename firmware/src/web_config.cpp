#include "web_config.h"

#include <ArduinoJson.h>
#include <Preferences.h>
#include <WebServer.h>
#include <esp_system.h>

#include <functional>
#include <WiFi.h>

#include "album.h"
#include "audio.h"
#include "display.h"
#include "friends.h"
#include "github.h"
#include "net.h"
#include "movie.h"
#include "movie_util.h"
#include "news.h"
#include "night.h"
#include "ota.h"
#include "ota_util.h"
#include "pages.h"
#include "player.h"
#include "photos.h"
#include "settings.h"
#include "stock.h"
#include "voice.h"
#include "weather.h"
#include "web_auth.h"
#include "web_index_gz.h"

static WebServer server(80);
static bool changed = false;
static void (*progress_hook)() = nullptr;
static void (*movie_hook)(uint64_t done, uint64_t total) = nullptr;
static void (*music_hook)(uint64_t done, uint64_t total) = nullptr;
static const Pomodoro *pomodoro = nullptr;
static void (*pomo_cmd)(PomoCmd) = nullptr;
static const PomoStats *pomo_stats = nullptr;
static int (*today)() = nullptr;
static void (*message_hook)(const String &, bool) = nullptr;
static void (*room_info)(RoomInfo &) = nullptr;
static constexpr int kMessageMaxChars = 60;
static void (*pair_hook)(const char *) = nullptr;

// ---- 配对登录：除了网页本身，所有接口都要带配对后拿到的令牌 ----
static WebAuth auth;
static constexpr const char *kAuthCookie = "ds_auth";

static uint32_t hw_random() { return esp_random(); }

static void save_tokens() {
  char buf[kWebAuthMaxTokens * (kWebAuthTokenLen + 1)];
  web_auth_save_tokens(auth, buf, sizeof(buf));
  Preferences p;
  p.begin("webauth", false);
  p.putString("tokens", buf);
  p.end();
}

static void load_tokens() {
  Preferences p;
  p.begin("webauth", true);
  String s = p.getString("tokens", "");
  p.end();
  web_auth_load_tokens(auth, s.c_str());
}

// 浏览器用 Cookie，电脑上的脚本可以用 X-Auth-Token 头
static bool authed() {
  char token[kWebAuthTokenLen + 1];
  if (web_auth_cookie(server.header("Cookie").c_str(), kAuthCookie, token, sizeof(token)) &&
      web_auth_check(auth, token)) {
    return true;
  }
  return web_auth_check(auth, server.header("X-Auth-Token").c_str());
}

using Handler = std::function<void()>;

static Handler guard(Handler h) {
  return [h] {
    if (!authed()) {
      server.send(401, "application/json", "{\"ok\":false,\"error\":\"需要配对\"}");
      return;
    }
    h();
  };
}

// 上传时没配对的请求直接丢掉数据，结束时由 guard 过的完成回调返回 401
static Handler guard_upload(Handler h) {
  return [h] {
    if (authed()) h();
  };
}

// 网页打不开接口时调用：屏幕上显示配对码
static void handle_pair_start() {
  const char *code = web_auth_start(auth, millis(), hw_random);
  if (pair_hook) pair_hook(code);
  server.send(200, "application/json", "{\"ok\":true}");
}

static void handle_pair() {
  char token[kWebAuthTokenLen + 1];
  if (!web_auth_pair(auth, server.arg("code").c_str(), millis(), hw_random, token)) {
    bool active = web_auth_code_active(auth, millis());
    if (!active && pair_hook) pair_hook(nullptr);
    JsonDocument doc;
    doc["ok"] = false;
    doc["error"] = active ? "配对码不对，再看一下屏幕" : "配对码已失效，请重新获取";
    doc["expired"] = !active;
    String out;
    serializeJson(doc, out);
    server.send(403, "application/json", out);
    return;
  }
  save_tokens();
  if (pair_hook) pair_hook(nullptr);
  server.sendHeader("Set-Cookie", String(kAuthCookie) + "=" + token +
                                      "; Path=/; Max-Age=31536000; HttpOnly; SameSite=Strict");
  server.send(200, "application/json", "{\"ok\":true}");
}

static void handle_pair_forget() {
  web_auth_forget_all(auth);
  save_tokens();
  server.sendHeader("Set-Cookie", String(kAuthCookie) + "=; Path=/; Max-Age=0");
  server.send(200, "application/json", "{\"ok\":true}");
}

// 板子上有没有这一页的硬件，和 main.cpp 的 page_unavailable 一致
static bool page_available(int p) {
  if (p == PAGE_CHAT) return voice_available();
  if (p == PAGE_ALBUM || p == PAGE_MOVIE) return photos_supported();
  if (p == PAGE_MUSIC) return player_supported();
  return true;
}

static String ota_text(const OtaStatus &s) {
  switch (s.state) {
    case OtaState::Idle: return "还没有检查过更新";
    case OtaState::Checking: return "正在检查更新…";
    case OtaState::UpToDate: return String("已是最新版本（") + s.latest + (s.mirror ? "，来自自建服务器）" : "）");
    case OtaState::Available:
      return String("发现新版本 ") + s.latest + (s.mirror ? "（来自自建服务器）" : "") + "，可以点「立即升级」";
    case OtaState::Downloading: return String("正在升级：") + s.progress + "%";
    case OtaState::Done: return "升级完成，设备正在重启";
    case OtaState::Failed: return String("失败：") + s.error;
  }
  return "";
}

static String stats_text() {
  if (!pomo_stats || !today) return "";
  int day = today();
  if (day < 0) return "";
  return String("今日完成 ") + pomo_stats->count(day) + " 个，近 7 天 " + pomo_stats->week_total(day) + " 个";
}

static bool ota_busy(const OtaStatus &s) {
  return s.state == OtaState::Checking || s.state == OtaState::Downloading;
}

// 设置网页是一个压缩好的静态文件（web/index.html，编译时由 tools/web_gz.py 生成），
// 设置和状态由网页用下面两个 JSON 接口读取
static void handle_index() {
  server.sendHeader("Content-Encoding", "gzip");
  server.sendHeader("Cache-Control", "no-cache");
  server.send_P(200, "text/html; charset=utf-8", (const char *)WEB_INDEX_GZ, WEB_INDEX_GZ_LEN);
}

static void send_doc(const JsonDocument &doc) {
  String out;
  serializeJson(doc, out);
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", out);
}

static void send_ok() {
  server.send(200, "application/json", "{\"ok\":true}");
}

// 字段名和 /save 提交的名字一致，网页按名字填进表单
static void handle_api_settings() {
  Settings s = settings_get();
  JsonDocument doc;
  char hhmm[8];
  auto put_time = [&](const char *key, uint16_t minutes) {
    night_format_hhmm(minutes, hhmm, sizeof(hhmm));
    doc[key] = hhmm;
  };
  doc["city"] = s.city;
  doc["lat"] = serialized(String(s.lat, 4));
  doc["lon"] = serialized(String(s.lon, 4));
  doc["brightness"] = s.brightness;
  doc["page_sec"] = s.page_seconds;
  // 页面开关：只列这块板子有的页面，时钟页一直显示也列出来（网页上灰着）
  uint32_t hidden = pages_parse_hidden(s.pages_off.c_str());
  JsonArray pages = doc["pages"].to<JsonArray>();
  for (int p = 0; p < PAGE_COUNT; p++) {
    if (!page_available(p)) continue;
    JsonObject o = pages.add<JsonObject>();
    o["k"] = kPageInfo[p].key;
    o["n"] = kPageInfo[p].name;
    o["on"] = !(hidden & (1u << p));
  }
  doc["night_on"] = s.night_on;
  put_time("night_start", s.night_start);
  doc["night_sun"] = s.night_sun;
  put_time("night_end", s.night_end);
  doc["theme"] = s.screen_theme;
  doc["night_bright"] = s.night_brightness;
  doc["stocks"] = s.stocks;
  doc["fr_url"] = s.fr_url;
  doc["fr_name"] = s.fr_name;
  doc["fr_here"] = s.fr_here;
  doc["fr_manage"] = friends_manage_url();  // 带着板子的 token，只给局域网里的设置网页
  doc["has_token"] = s.github_token.length() > 0;  // Token 和 Key 不发给网页，只告诉有没有
  doc["auto_update"] = s.auto_update;
  doc["ota_url"] = s.ota_url;
  doc["pomo_focus"] = s.pomo_focus;
  doc["pomo_short"] = s.pomo_short;
  doc["pomo_long"] = s.pomo_long;
  doc["pomo_every"] = s.pomo_every;
  doc["sound_on"] = s.sound_on;
  doc["sound_volume"] = s.sound_volume;
  doc["sound_quiet"] = s.sound_quiet_night;
  doc["sit_on"] = s.sit_on;
  doc["sit_min"] = s.sit_min;
  doc["water_on"] = s.water_on;
  doc["water_min"] = s.water_min;
  put_time("remind_start", s.remind_start);
  put_time("remind_end", s.remind_end);
  doc["chime_on"] = s.chime_on;
  doc["alarm_on"] = s.alarm_on;
  put_time("alarm_time", s.alarm_time);
  doc["alarm_days"] = s.alarm_days;
  doc["climate_on"] = s.climate_on;
  doc["rain_on"] = s.rain_on;
  doc["music_on"] = s.music_on;
  doc["hot_c"] = s.hot_c;
  doc["humid_pct"] = s.humid_pct;
  doc["temp_offset"] = serialized(String(s.temp_offset / 10.0f, 1));
  doc["away_off"] = s.away_off;
  doc["away_min"] = s.away_min;
  doc["has_ai"] = s.ai_key.length() > 0;
  doc["ai_model"] = s.ai_model;
  doc["ai_voice"] = s.ai_voice;
  doc["wake_on"] = s.wake_on;
  doc["barge_on"] = s.barge_on;
  doc["album_sec"] = s.album_sec;
  doc["album_shuffle"] = s.album_shuffle;
  doc["saver_on"] = s.saver_on;
  doc["saver_min"] = s.saver_min;
  doc["album_local"] = s.album_local;
  doc["album_bing"] = s.album_bing;
  doc["bing_caption"] = s.bing_caption;
  doc["album_sound"] = s.album_sound;
  for (int i = 0; i < kNewsSourceCount; i++) {
    doc[String("ns_") + kNewsSources[i].id] = news_has_source(s.news_src.c_str(), kNewsSources[i].id);
  }
  doc["news_rss"] = s.news_rss;
  doc["news_style"] = s.news_style;
  doc["news_ticker"] = s.news_ticker;
  doc["news_voice"] = s.news_voice;
  // 这块板子有没有这些硬件
  doc["audio"] = audio_available();
  doc["voice"] = voice_available();
  doc["photos"] = photos_supported();
  send_doc(doc);
}

static const char *ota_state_name(OtaState s) {
  switch (s) {
    case OtaState::Idle: return "idle";
    case OtaState::Checking: return "checking";
    case OtaState::UpToDate: return "uptodate";
    case OtaState::Available: return "available";
    case OtaState::Downloading: return "downloading";
    case OtaState::Done: return "done";
    case OtaState::Failed: return "failed";
  }
  return "";
}

static void handle_api_status() {
  JsonDocument doc;
  doc["ver"] = ota_current_version();
  doc["uptime"] = millis() / 1000;
  JsonObject wifi = doc["wifi"].to<JsonObject>();
  wifi["ssid"] = WiFi.SSID();
  wifi["rssi"] = WiFi.RSSI();
  wifi["ip"] = WiFi.localIP().toString();

  OtaStatus os = ota_status();
  JsonObject ota = doc["ota"].to<JsonObject>();
  ota["state"] = ota_state_name(os.state);
  ota["text"] = ota_text(os);
  ota["rolled_back"] = ota_rolled_back();
  ota["busy"] = ota_busy(os);
  ota["latest"] = os.latest;

  JsonObject po = doc["pomo"].to<JsonObject>();
  if (pomodoro) {
    const Pomodoro &p = *pomodoro;
    static const char *const phases[] = {"idle", "focus", "short", "long"};
    PomoPhase ph = p.phase();
    po["phase"] = ph == PomoPhase::Focus        ? phases[1]
                  : ph == PomoPhase::ShortBreak ? phases[2]
                  : ph == PomoPhase::LongBreak  ? phases[3]
                                                : phases[0];
    po["running"] = p.running();
    po["left"] = (p.remaining_ms(millis()) + 999) / 1000;
  } else {
    po["phase"] = "idle";
    po["running"] = false;
    po["left"] = 0;
  }
  int day = today ? today() : -1;
  po["today"] = pomo_stats && day >= 0 ? pomo_stats->count(day) : -1;
  po["stats"] = stats_text();

  JsonObject room = doc["room"].to<JsonObject>();
  RoomInfo ri = {};
  if (room_info) room_info(ri);
  room["dock"] = ri.dock;
  room["valid"] = ri.valid;
  room["temp"] = serialized(String(ri.temp_c, 1));
  room["hum"] = ri.humidity;
  room["presence"] = ri.presence;
  room["away"] = ri.away;

  doc["friends"] = friends_brief();
  doc["net"] = net_status_html();
  String panel = "屏幕：";
  if (display_panel_watching()) {
    panel += String("自动恢复白屏、花屏 ") + display_panel_recoveries() + " 次";
  } else {
    panel += "读不出屏幕状态，每 10 分钟重新初始化一次";
  }
  doc["panel"] = panel;
  send_doc(doc);
}

static void handle_save() {
  Settings s = settings_get();
  String city = server.arg("city");
  city.trim();
  if (city.length() > 0) s.city = city;

  float lat = server.arg("lat").toFloat();
  float lon = server.arg("lon").toFloat();
  if (lat >= -90 && lat <= 90 && lon >= -180 && lon <= 180 && (lat != 0 || lon != 0)) {
    s.lat = lat;
    s.lon = lon;
  }

  long b = server.arg("brightness").toInt();
  if (b >= 10 && b <= 255) s.brightness = b;

  String sec = server.arg("page_sec");
  sec.trim();
  if (sec.length() > 0 && sec.toInt() >= 0 && sec.toInt() <= 255) s.page_seconds = sec.toInt();

  if (server.hasArg("pg_form")) {  // 确认是设置表单提交的，再更新页面开关；没勾的复选框不会提交
    uint32_t hidden = pages_parse_hidden(s.pages_off.c_str());
    for (int p = PAGE_CLOCK + 1; p < PAGE_COUNT; p++) {
      if (!page_available(p)) continue;  // 网页上没列出来的保持原样
      if (server.hasArg(String("pg_") + kPageInfo[p].key)) {
        hidden &= ~(1u << p);
      } else {
        hidden |= 1u << p;
      }
    }
    char list[96];
    pages_format_hidden(hidden, list, sizeof(list));
    s.pages_off = list;
  }

  String token = server.arg("gh_token");
  token.trim();
  if (server.hasArg("gh_clear")) {
    s.github_token = "";
  } else if (token.length() > 0) {
    s.github_token = token;
  }

  String ai_key = server.arg("ai_key");
  ai_key.trim();
  if (server.hasArg("ai_clear")) {
    s.ai_key = "";
  } else if (ai_key.length() > 0) {
    s.ai_key = ai_key;
  }
  String ai_model = server.arg("ai_model");
  ai_model.trim();
  s.ai_model = ai_model.length() > 0 ? ai_model : String("qwen3.8-omni-flash-realtime");
  String ai_voice = server.arg("ai_voice");
  ai_voice.trim();
  s.ai_voice = ai_voice == "Cherry" ? String() : ai_voice;  // 留空用模型默认的

  // 复选框不勾选时浏览器不会提交这个字段
  s.night_on = server.hasArg("night_on");
  s.night_sun = server.hasArg("night_sun");
  s.auto_update = server.hasArg("auto_update");
  if (server.hasArg("ota_url")) {
    String ota_url = server.arg("ota_url");
    ota_url.trim();
    // 填错了就当没填，照样用 GitHub；网页上会提示格式
    s.ota_url = ota_is_valid_source_url(ota_url.c_str()) ? ota_url : String("");
  }
  s.sound_on = server.hasArg("sound_on");
  s.sound_quiet_night = server.hasArg("sound_quiet");
  long vol = server.arg("sound_volume").toInt();
  if (server.hasArg("sound_volume") && vol >= 0 && vol <= 100) s.sound_volume = vol;
  int start = night_parse_hhmm(server.arg("night_start").c_str());
  int end = night_parse_hhmm(server.arg("night_end").c_str());
  if (start >= 0) s.night_start = start;
  if (end >= 0) s.night_end = end;
  long nb = server.arg("night_bright").toInt();
  if (server.hasArg("night_bright") && nb >= 0 && nb <= 255) s.night_brightness = nb;
  long theme = server.arg("theme").toInt();
  if (server.hasArg("theme") && theme >= THEME_DARK && theme <= THEME_AUTO) s.screen_theme = theme;

  // 番茄钟时长：1~180 分钟，长休息间隔 1~8 个；留空或不合法就保持原值
  auto minutes_arg = [](const char *name, uint8_t &field, int lo, int hi) {
    String v = server.arg(name);
    v.trim();
    if (v.length() > 0 && v.toInt() >= lo && v.toInt() <= hi) field = v.toInt();
  };
  minutes_arg("pomo_focus", s.pomo_focus, 1, 180);
  minutes_arg("pomo_short", s.pomo_short, 1, 180);
  minutes_arg("pomo_long", s.pomo_long, 1, 180);
  minutes_arg("pomo_every", s.pomo_every, 1, 8);
  minutes_arg("sit_min", s.sit_min, 10, 240);
  minutes_arg("water_min", s.water_min, 10, 240);
  s.sit_on = server.hasArg("sit_on");
  s.water_on = server.hasArg("water_on");
  s.rain_on = server.hasArg("rain_on");
  s.music_on = server.hasArg("music_on");
  s.wake_on = server.hasArg("wake_on");
  s.barge_on = server.hasArg("barge_on");
  s.chime_on = server.hasArg("chime_on");
  s.alarm_on = server.hasArg("alarm_on");
  int at = night_parse_hhmm(server.arg("alarm_time").c_str());
  if (at >= 0) s.alarm_time = at;
  if (server.hasArg("alarm_time")) {  // 确认是设置表单提交的，再更新星期
    uint8_t mask = 0;
    for (int d = 0; d < 7; d++) {
      if (server.hasArg(String("ad") + d)) mask |= 1 << d;
    }
    s.alarm_days = mask;
  }
  minutes_arg("hot_c", s.hot_c, 20, 45);
  minutes_arg("humid_pct", s.humid_pct, 40, 95);
  minutes_arg("away_min", s.away_min, 1, 60);
  if (server.hasArg("hot_c")) {  // 确认是设置表单提交的，再更新复选框和校准值
    s.climate_on = server.hasArg("climate_on");
    s.away_off = server.hasArg("away_off");
    String ofs = server.arg("temp_offset");
    ofs.trim();
    float o = ofs.toFloat();
    if (ofs.length() > 0 && o >= -10 && o <= 10) s.temp_offset = (int8_t)lroundf(o * 10);
  }
  minutes_arg("album_sec", s.album_sec, 3, 255);
  minutes_arg("saver_min", s.saver_min, 1, 120);
  if (server.hasArg("album_sec")) {  // 确认是设置表单提交的，再更新复选框
    s.album_shuffle = server.hasArg("album_shuffle");
    s.saver_on = server.hasArg("saver_on");
    s.album_local = server.hasArg("album_local");
    s.album_bing = server.hasArg("album_bing");
    s.bing_caption = server.hasArg("bing_caption");
    s.album_sound = server.hasArg("album_sound");
  }
  int rs = night_parse_hhmm(server.arg("remind_start").c_str());
  int re = night_parse_hhmm(server.arg("remind_end").c_str());
  if (rs >= 0) s.remind_start = rs;
  if (re >= 0) s.remind_end = re;

  if (server.hasArg("stocks")) {
    s.stocks = server.arg("stocks");
    s.stocks.trim();
  }
  if (server.hasArg("news_style")) {  // 确认是设置表单提交的，再更新资讯的勾选项
    String src;
    for (int i = 0; i < kNewsSourceCount; i++) {
      if (!server.hasArg(String("ns_") + kNewsSources[i].id)) continue;
      if (src.length()) src += ',';
      src += kNewsSources[i].id;
    }
    s.news_src = src;
    std::string urls[kNewsMaxRss];
    int n = news_split_urls(server.arg("news_rss").c_str(), urls, kNewsMaxRss);
    s.news_rss = "";
    for (int i = 0; i < n; i++) {
      if (i) s.news_rss += '\n';
      s.news_rss += urls[i].c_str();
    }
    s.news_style = server.arg("news_style").toInt() == 1 ? 1 : 0;
    s.news_ticker = server.hasArg("news_ticker");
    s.news_voice = server.hasArg("news_voice");
  }

  if (server.hasArg("fr_form")) {  // 设置表单里有好友圈子这张卡片
    String url = server.arg("fr_url");
    url.trim();
    if (url.length() && url.indexOf("://") < 0) url = "https://" + url;  // 只写了域名
    char clean[160];
    friends_clean_url(url.c_str(), clean, sizeof(clean));
    s.fr_url = clean;
    String name = server.arg("fr_name");
    name.trim();
    char short_name[kFrNameLen];
    friends_copy(short_name, sizeof(short_name), name.c_str());  // 服务器只留 10 个字，这里先截一下
    if (short_name[0]) s.fr_name = short_name;
    s.fr_here = server.hasArg("fr_here");
  }

  settings_save(s);
  changed = true;
  weather_request_refresh();
  github_request_refresh();
  stock_request_refresh();
  news_request_refresh();

  send_ok();
}

// 留言：去掉首尾空白，超过 60 个字截断（按 UTF-8 字符数，不会切坏汉字）
static void handle_message() {
  String text = server.arg("text");
  text.trim();
  int chars = 0;
  for (size_t i = 0; i < text.length(); i++) {
    if (((uint8_t)text[i] & 0xC0) == 0x80) continue;  // UTF-8 后续字节
    if (++chars > kMessageMaxChars) {
      text.remove(i);
      break;
    }
  }
  if (text.length() > 0 && message_hook) message_hook(text, server.hasArg("sound"));
  send_ok();
}

static void handle_pomo(PomoCmd cmd) {
  if (pomo_cmd) pomo_cmd(cmd);
  send_ok();
}

static bool upload_ok = false;
static bool upload_started = false;

// 浏览器分块发来的固件，每块约 1.4KB
static void handle_upload_chunk() {
  HTTPUpload &up = server.upload();
  if (up.status == UPLOAD_FILE_START) {
    upload_ok = true;
    upload_started = false;
  } else if (up.status == UPLOAD_FILE_WRITE && upload_ok) {
    if (!upload_started) {
      upload_started = true;
      upload_ok = ota_upload_begin(up.buf, up.currentSize, server.header("Content-Length").toInt());
    } else {
      upload_ok = ota_upload_write(up.buf, up.currentSize);
    }
    static size_t last_hook = 0;
    if (progress_hook && (up.totalSize - last_hook >= 32 * 1024 || up.totalSize < last_hook)) {
      last_hook = up.totalSize;
      progress_hook();
    }
  } else if (up.status == UPLOAD_FILE_END) {
    if (upload_ok && upload_started) upload_ok = ota_upload_end();
    if (!upload_started) upload_ok = false;
  } else if (up.status == UPLOAD_FILE_ABORTED) {
    if (upload_started) ota_upload_abort();
    upload_ok = false;
  }
}

static void handle_upload_done() {
  JsonDocument doc;
  doc["ok"] = upload_ok;
  if (!upload_ok) doc["error"] = String(ota_status().error);
  send_doc(doc);
}

// 电子相册：照片列表、上传、删除、缩略图
static constexpr size_t kPhotoMaxBytes = 4 * 1024 * 1024;  // 网页会先缩小，正常每张几十 KB
static constexpr size_t kGifMaxBytes = 8 * 1024 * 1024;     // 和 photos.cpp 能读进内存的上限一样
static constexpr size_t kVideoMaxBytes = 200 * 1024 * 1024;  // 网页转好的视频每分钟十几 MB
static size_t photo_max = kPhotoMaxBytes;
static bool photo_ok = false;
static const char *photo_error = "";
static String photo_name;

static void send_json(bool ok, const char *error) {
  JsonDocument doc;
  doc["ok"] = ok;
  if (!ok) doc["error"] = error;
  if (ok && photo_name.length()) doc["name"] = photo_name;
  String out;
  serializeJson(doc, out);
  server.send(200, "application/json", out);
}

static void handle_photo_chunk() {
  HTTPUpload &up = server.upload();
  if (up.status == UPLOAD_FILE_START) {
    photo_name = up.filename;
    MediaKind kind = album_media_kind(photo_name.c_str());
    photo_max = kind == MediaKind::Video ? kVideoMaxBytes : kind == MediaKind::Gif ? kGifMaxBytes : kPhotoMaxBytes;
    photo_ok = photos_upload_begin(photo_name);
    photo_error = photo_ok ? "" : "没有 TF 卡或者卡满了";
  } else if (up.status == UPLOAD_FILE_WRITE && photo_ok) {
    if (up.totalSize > photo_max) {
      photo_ok = false;
      photo_error = "文件太大";
      photos_upload_abort();
    } else if (!photos_upload_write(up.buf, up.currentSize)) {
      photo_ok = false;
      photo_error = "写入 TF 卡失败";
      photos_upload_abort();
    }
  } else if (up.status == UPLOAD_FILE_END && photo_ok) {
    photo_ok = photos_upload_end(photo_name);
    if (!photo_ok) photo_error = "写入 TF 卡失败";
  } else if (up.status == UPLOAD_FILE_ABORTED) {
    if (photo_ok) photos_upload_abort();
    photo_ok = false;
    photo_error = "上传中断了";
  }
}

// 网页里 src=bing 表示必应图片，否则是自己的照片
static PhotoSource source_arg() {
  return server.arg("src") == "bing" ? PhotoSource::Bing : PhotoSource::Local;
}

static void handle_photo_file() {
  String name = server.arg("name");
  String lower = name;
  lower.toLowerCase();
  const char *type = lower.endsWith(".png")   ? "image/png"
                     : lower.endsWith(".gif") ? "image/gif"
                     : lower.endsWith(".avi") ? "video/x-msvideo"
                                              : "image/jpeg";
  bool ok = photos_read(source_arg(), name, [](void *ctx, const uint8_t *data, size_t len, size_t total) {
    if (!data) {  // 开头先发一次文件大小
      server.sendHeader("Cache-Control", "max-age=3600");
      server.setContentLength(total);
      server.send(200, (const char *)ctx, "");
      return;
    }
    server.sendContent((const char *)data, len);
  }, (void *)type);
  if (!ok) server.send(404, "text/plain", "Not found");
}

// 影片：TF 卡 movies 文件夹的列表、上传、删除。电脑转好的 .avi 原样传，几百 MB 到几 GB，
// 收的时候网页服务器一直占着主循环，所以每收一段调一次 movie_hook，让屏幕上显示进度
static bool movie_ok = false;
static uint64_t movie_size = 0;
static char movie_saved[kMovieNameMax];

static void handle_movie_list() {
  movie_rescan_now();  // 刚用读卡器拷进去的片子也能看到
  JsonDocument doc;
  MovieCard c = movie_card();
  doc["card"] = c == MovieCard::NoSlot ? "noslot" : c == MovieCard::NoCard ? "none" : "ok";
  uint64_t total = 0, used = 0;
  if (c != MovieCard::NoSlot && c != MovieCard::NoCard && movie_card_space(total, used)) {
    doc["total"] = (double)total;  // 字节数可能超过 32 位
    doc["used"] = (double)used;
  }
  JsonArray arr = doc["movies"].to<JsonArray>();
  MovieInfo m;
  for (int i = 0; movie_info(i, m); i++) {
    JsonObject o = arr.add<JsonObject>();
    o["n"] = m.name;
    o["d"] = m.dur_ms / 1000;
    o["p"] = m.pos_ms / 1000;
  }
  send_doc(doc);
}

static void handle_movie_chunk() {
  HTTPUpload &up = server.upload();
  if (up.status == UPLOAD_FILE_START) {
    // 整个请求比文件多出几百字节的表单头，当文件大小用足够了
    movie_size = strtoull(server.header("Content-Length").c_str(), nullptr, 10);
    movie_saved[0] = '\0';
    movie_ok = movie_upload_begin(movie_size);
    if (movie_hook) movie_hook(0, movie_size);
  } else if (up.status == UPLOAD_FILE_WRITE && movie_ok) {
    movie_ok = movie_upload_write(up.buf, up.currentSize);
    static size_t last_hook = 0;
    if (movie_hook && (up.totalSize - last_hook >= 256 * 1024 || up.totalSize < last_hook)) {
      last_hook = up.totalSize;
      movie_hook(up.totalSize, movie_size);
    }
  } else if (up.status == UPLOAD_FILE_END && movie_ok) {
    movie_ok = movie_upload_end(up.filename.c_str(), movie_saved, sizeof(movie_saved));
    if (movie_hook) movie_hook(0, 0);
  } else if (up.status == UPLOAD_FILE_ABORTED) {
    if (movie_ok) movie_upload_abort();
    movie_ok = false;
    if (movie_hook) movie_hook(0, 0);
  }
}

static void handle_movie_done() {
  if (movie_hook) movie_hook(0, 0);  // 出错时没走到 END，这里也收起进度画面
  JsonDocument doc;
  doc["ok"] = movie_ok;
  if (movie_ok) doc["name"] = movie_saved;
  else doc["error"] = movie_upload_error();
  send_doc(doc);
}

// 音乐：TF 卡 music 文件夹的列表、上传（可以选歌单文件夹）、删除、新建歌单。
// 一首歌几 MB 到几十 MB，收的时候和影片一样在屏幕上显示进度
static bool music_ok = false;
static uint64_t music_size = 0;
static char music_saved[kTrackPathMax];

static void handle_music_chunk() {
  HTTPUpload &up = server.upload();
  if (up.status == UPLOAD_FILE_START) {
    music_size = strtoull(server.header("Content-Length").c_str(), nullptr, 10);
    music_saved[0] = '\0';
    music_ok = player_upload_begin(server.arg("folder").c_str(), music_size);
    if (music_hook) music_hook(0, music_size);
  } else if (up.status == UPLOAD_FILE_WRITE && music_ok) {
    music_ok = player_upload_write(up.buf, up.currentSize);
    static size_t last_hook = 0;
    if (music_hook && (up.totalSize - last_hook >= 128 * 1024 || up.totalSize < last_hook)) {
      last_hook = up.totalSize;
      music_hook(up.totalSize, music_size);
    }
  } else if (up.status == UPLOAD_FILE_END && music_ok) {
    music_ok = player_upload_end(up.filename.c_str(), music_saved, sizeof(music_saved));
  } else if (up.status == UPLOAD_FILE_ABORTED) {
    if (music_ok) player_upload_abort();
    music_ok = false;
  }
}

static void handle_music_done() {
  if (music_hook) music_hook(0, 0);  // 收完、出错都收起进度画面
  JsonDocument doc;
  doc["ok"] = music_ok;
  if (music_ok) doc["name"] = music_saved;
  else doc["error"] = player_upload_error();
  send_doc(doc);
}

static void handle_reset_wifi() {
  send_ok();
  delay(500);
  WiFi.disconnect(true, true);  // 第二个参数会擦除保存的 WiFi 信息
  delay(500);
  ESP.restart();
}

void web_config_begin() {
  load_tokens();
  server.on("/", HTTP_GET, handle_index);
  server.on("/pair/start", HTTP_POST, handle_pair_start);
  server.on("/pair", HTTP_POST, handle_pair);
  server.on("/pair/forget", HTTP_POST, guard(handle_pair_forget));
  server.on("/api/settings", HTTP_GET, guard(handle_api_settings));
  server.on("/api/status", HTTP_GET, guard(handle_api_status));
  server.on("/save", HTTP_POST, guard(handle_save));
  server.on("/reset-wifi", HTTP_POST, guard(handle_reset_wifi));
  server.on("/message", HTTP_POST, guard(handle_message));
  server.on("/pomodoro/toggle", HTTP_POST, guard([] { handle_pomo(PomoCmd::Toggle); }));
  server.on("/pomodoro/skip", HTTP_POST, guard([] { handle_pomo(PomoCmd::Skip); }));
  server.on("/pomodoro/reset", HTTP_POST, guard([] { handle_pomo(PomoCmd::Reset); }));
  server.on("/sound/test", HTTP_POST, guard([] {
    audio_play(SoundId::FocusDone);
    send_ok();
  }));
  server.on("/ota/check", HTTP_POST, guard([] {
    ota_check_now();
    send_ok();
  }));
  server.on("/ota/install", HTTP_POST, guard([] {
    ota_install_now();
    send_ok();
  }));
  server.on("/ota/upload", HTTP_POST, guard(handle_upload_done), guard_upload(handle_upload_chunk));
  server.on("/album", HTTP_GET, [] {  // 旧的相册地址，现在是设置网页里的子页面
    server.sendHeader("Location", "/#album");
    server.send(302);
  });
  server.on("/album/list", HTTP_GET, guard([] { server.send(200, "application/json", photos_list_json()); }));
  server.on("/album/file", HTTP_GET, guard(handle_photo_file));
  server.on("/album/upload", HTTP_POST, guard([] { send_json(photo_ok, photo_error); }),
            guard_upload(handle_photo_chunk));
  server.on("/album/delete", HTTP_POST, guard([] {
    photo_name = "";
    send_json(photos_delete(source_arg(), server.arg("name")), "删除失败");
  }));
  server.on("/movies/list", HTTP_GET, guard(handle_movie_list));
  server.on("/movies/upload", HTTP_POST, guard(handle_movie_done), guard_upload(handle_movie_chunk));
  server.on("/movies/delete", HTTP_POST, guard([] {
    JsonDocument doc;
    bool ok = movie_delete(server.arg("name").c_str());
    doc["ok"] = ok;
    if (!ok) doc["error"] = "删除失败";
    send_doc(doc);
  }));
  server.on("/music/list", HTTP_GET, guard([] {
    String out;
    player_list_json(out);
    server.send(200, "application/json", out);
  }));
  server.on("/music/upload", HTTP_POST, guard(handle_music_done), guard_upload(handle_music_chunk));
  server.on("/music/delete", HTTP_POST, guard([] {
    JsonDocument doc;
    bool ok = player_delete(server.arg("name").c_str());
    doc["ok"] = ok;
    if (!ok) doc["error"] = "删除失败";
    send_doc(doc);
  }));
  server.on("/music/mkdir", HTTP_POST, guard([] {
    JsonDocument doc;
    bool ok = player_mkdir(server.arg("name").c_str());
    doc["ok"] = ok;
    if (!ok) doc["error"] = "建不了这个歌单，换个名字试试";
    send_doc(doc);
  }));
  const char *headers[] = {"Content-Length", "Cookie", "X-Auth-Token"};
  server.collectHeaders(headers, 3);
  server.onNotFound([] { server.send(404, "text/plain", "Not found"); });
  server.begin();
}

void web_config_loop() {
  server.handleClient();
}

void web_config_set_pomodoro(const Pomodoro *p, void (*cmd)(PomoCmd)) {
  pomodoro = p;
  pomo_cmd = cmd;
}

void web_config_set_pomo_stats(const PomoStats *s, int (*day)()) {
  pomo_stats = s;
  today = day;
}

void web_config_set_message_hook(void (*hook)(const String &text, bool sound)) {
  message_hook = hook;
}

void web_config_set_room_info(void (*hook)(RoomInfo &info)) {
  room_info = hook;
}

void web_config_set_pair_hook(void (*hook)(const char *code)) {
  pair_hook = hook;
}

bool web_config_pair_active() {
  return web_auth_code_active(auth, millis());
}

void web_config_set_progress_hook(void (*hook)()) {
  progress_hook = hook;
}

void web_config_set_movie_hook(void (*hook)(uint64_t done, uint64_t total)) {
  movie_hook = hook;
}

void web_config_set_music_hook(void (*hook)(uint64_t done, uint64_t total)) {
  music_hook = hook;
}

bool web_config_take_changed() {
  bool c = changed;
  changed = false;
  return c;
}
