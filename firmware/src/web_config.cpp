#include "web_config.h"

#include <ArduinoJson.h>
#include <WebServer.h>
#include <WiFi.h>

#include "album.h"
#include "audio.h"
#include "display.h"
#include "github.h"
#include "net.h"
#include "night.h"
#include "ota.h"
#include "ota_util.h"
#include "photos.h"
#include "settings.h"
#include "stock.h"
#include "voice.h"
#include "weather.h"
#include "web_index_gz.h"

static WebServer server(80);
static bool changed = false;
static void (*progress_hook)() = nullptr;
static const Pomodoro *pomodoro = nullptr;
static void (*pomo_cmd)(PomoCmd) = nullptr;
static const PomoStats *pomo_stats = nullptr;
static int (*today)() = nullptr;
static void (*message_hook)(const String &, bool) = nullptr;
static void (*room_info)(RoomInfo &) = nullptr;
static constexpr int kMessageMaxChars = 60;

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
  doc["night_on"] = s.night_on;
  put_time("night_start", s.night_start);
  doc["night_sun"] = s.night_sun;
  put_time("night_end", s.night_end);
  doc["night_bright"] = s.night_brightness;
  doc["stocks"] = s.stocks;
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
  doc["album_sec"] = s.album_sec;
  doc["album_shuffle"] = s.album_shuffle;
  doc["saver_on"] = s.saver_on;
  doc["saver_min"] = s.saver_min;
  doc["album_local"] = s.album_local;
  doc["album_bing"] = s.album_bing;
  doc["bing_caption"] = s.bing_caption;
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
  s.ai_model = ai_model.length() > 0 ? ai_model : String("qwen3-omni-flash");
  String ai_voice = server.arg("ai_voice");
  ai_voice.trim();
  s.ai_voice = ai_voice.length() > 0 ? ai_voice : String("Cherry");

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
  }
  int rs = night_parse_hhmm(server.arg("remind_start").c_str());
  int re = night_parse_hhmm(server.arg("remind_end").c_str());
  if (rs >= 0) s.remind_start = rs;
  if (re >= 0) s.remind_end = re;

  if (server.hasArg("stocks")) {
    s.stocks = server.arg("stocks");
    s.stocks.trim();
  }

  settings_save(s);
  changed = true;
  weather_request_refresh();
  github_request_refresh();
  stock_request_refresh();

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

static void handle_reset_wifi() {
  send_ok();
  delay(500);
  WiFi.disconnect(true, true);  // 第二个参数会擦除保存的 WiFi 信息
  delay(500);
  ESP.restart();
}

void web_config_begin() {
  server.on("/", HTTP_GET, handle_index);
  server.on("/api/settings", HTTP_GET, handle_api_settings);
  server.on("/api/status", HTTP_GET, handle_api_status);
  server.on("/save", HTTP_POST, handle_save);
  server.on("/reset-wifi", HTTP_POST, handle_reset_wifi);
  server.on("/message", HTTP_POST, handle_message);
  server.on("/pomodoro/toggle", HTTP_POST, [] { handle_pomo(PomoCmd::Toggle); });
  server.on("/pomodoro/skip", HTTP_POST, [] { handle_pomo(PomoCmd::Skip); });
  server.on("/pomodoro/reset", HTTP_POST, [] { handle_pomo(PomoCmd::Reset); });
  server.on("/sound/test", HTTP_POST, [] {
    audio_play(SoundId::FocusDone);
    send_ok();
  });
  server.on("/ota/check", HTTP_POST, [] {
    ota_check_now();
    send_ok();
  });
  server.on("/ota/install", HTTP_POST, [] {
    ota_install_now();
    send_ok();
  });
  server.on("/ota/upload", HTTP_POST, handle_upload_done, handle_upload_chunk);
  server.on("/album", HTTP_GET, [] {  // 旧的相册地址，现在是设置网页里的子页面
    server.sendHeader("Location", "/#album");
    server.send(302);
  });
  server.on("/album/list", HTTP_GET, [] { server.send(200, "application/json", photos_list_json()); });
  server.on("/album/file", HTTP_GET, handle_photo_file);
  server.on("/album/upload", HTTP_POST, [] { send_json(photo_ok, photo_error); }, handle_photo_chunk);
  server.on("/album/delete", HTTP_POST, [] {
    photo_name = "";
    send_json(photos_delete(source_arg(), server.arg("name")), "删除失败");
  });
  const char *headers[] = {"Content-Length"};
  server.collectHeaders(headers, 1);
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

void web_config_set_progress_hook(void (*hook)()) {
  progress_hook = hook;
}

bool web_config_take_changed() {
  bool c = changed;
  changed = false;
  return c;
}
