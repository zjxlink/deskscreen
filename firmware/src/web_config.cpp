#include "web_config.h"

#include <ArduinoJson.h>
#include <WebServer.h>
#include <WiFi.h>

#include "audio.h"
#include "github.h"
#include "night.h"
#include "ota.h"
#include "settings.h"
#include "stock.h"
#include "weather.h"

static WebServer server(80);
static bool changed = false;
static void (*progress_hook)() = nullptr;
static const Pomodoro *pomodoro = nullptr;
static void (*pomo_cmd)(PomoCmd) = nullptr;

static const char PAGE[] PROGMEM = R"html(<!doctype html>
<html lang="zh-CN"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>桌面信息屏设置</title>
<style>
body{font-family:-apple-system,system-ui,sans-serif;background:#0d1117;color:#e6edf3;margin:0;padding:16px}
main{max-width:420px;margin:0 auto}
h1{font-size:20px}
label{display:block;margin:16px 0 6px;color:#8b949e;font-size:14px}
input{width:100%;box-sizing:border-box;padding:10px;border-radius:8px;border:1px solid #30363d;background:#161b22;color:#e6edf3;font-size:16px}
input[type=range]{padding:0}
.row{display:flex;gap:8px}
button{padding:10px 16px;border:0;border-radius:8px;background:#238636;color:#fff;font-size:16px}
button.sub{background:#30363d}
button.danger{background:#da3633}
.tip{color:#3fb950;margin:8px 0}
#results button{display:block;width:100%;margin-top:6px;text-align:left}
.ota{background:#161b22;border-radius:8px;padding:10px 12px}
hr{border:0;border-top:1px solid #30363d;margin:28px 0}
h2{font-size:17px;margin-top:28px}
a{color:#58a6ff}
.hint{color:#8b949e;font-size:13px;line-height:1.6}
label.check{display:flex;gap:8px;align-items:center;color:#e6edf3}
label.check input{width:auto}
.row > div{flex:1}
</style></head><body><main>
<h1>桌面信息屏设置</h1>
%SAVED%
<form method="post" action="/save">
<label>城市（显示在屏幕上）</label>
<div class="row"><input name="city" id="city" value="%CITY%" maxlength="16">
<button type="button" class="sub" onclick="search()">查坐标</button></div>
<div id="results"></div>
<label>纬度</label><input name="lat" id="lat" value="%LAT%" inputmode="decimal">
<label>经度</label><input name="lon" id="lon" value="%LON%" inputmode="decimal">
<label>屏幕亮度</label><input type="range" name="brightness" min="10" max="255" value="%BRIGHT%">
<label>自动切换页面间隔（秒，0 表示不自动切换）</label>
<input name="page_sec" value="%PAGE_SEC%" inputmode="numeric">
<h2>夜间模式</h2>
<label class="check"><input type="checkbox" name="night_on" %NIGHT_ON%> 夜间自动调暗</label>
<div class="row">
<div><label>开始</label><input type="time" name="night_start" value="%NIGHT_START%"></div>
<div><label>结束</label><input type="time" name="night_end" value="%NIGHT_END%"></div>
</div>
<label>夜间亮度（拉到最左边会关闭屏幕）</label>
<input type="range" name="night_bright" min="0" max="255" value="%NIGHT_BRIGHT%">
<p class="hint">夜间只显示时钟页，不自动轮播。点一下屏幕或按 BOOT 键，会临时恢复正常亮度 30 秒。</p>
<h2>番茄钟</h2>
<div class="row">
<div><label>专注（分钟）</label><input name="pomo_focus" value="%POMO_FOCUS%" inputmode="numeric"></div>
<div><label>短休息</label><input name="pomo_short" value="%POMO_SHORT%" inputmode="numeric"></div>
<div><label>长休息</label><input name="pomo_long" value="%POMO_LONG%" inputmode="numeric"></div>
<div><label>每几个长休息</label><input name="pomo_every" value="%POMO_EVERY%" inputmode="numeric"></div>
</div>
<p class="hint">在设备上点圆环开始或暂停，按住圆环 1.5 秒重置；下面的按钮也能控制（没有触摸屏的板子用这个）。</p>
<label class="check"><input type="checkbox" name="sound_on" %SOUND_ON%> 专注和休息结束时播放提示音%SOUND_NOTE%</label>
<label>提示音音量</label><input type="range" name="sound_volume" min="0" max="100" value="%SOUND_VOL%">
<label class="check"><input type="checkbox" name="sound_quiet" %SOUND_QUIET%> 夜间时段不响（只点亮屏幕）</label>
<h2>GitHub 贡献热力图</h2>
<label>GitHub Token（%GH_STATE%）</label>
<input type="password" name="gh_token" placeholder="%GH_PLACEHOLDER%" autocomplete="off">
<p class="hint">打开 <a href="https://github.com/settings/tokens/new?description=DeskScreen" target="_blank">GitHub 创建 Token</a>（classic），
<b>不用勾选任何权限</b>，有效期按需选择，生成后复制粘贴到这里。Token 只保存在设备上，不会显示出来。</p>
<label class="check"><input type="checkbox" name="gh_clear"> 清除已保存的 Token</label>
<h2>自选股</h2>
<label>股票代码（最多 6 个，用逗号或空格分隔）</label>
<input name="stocks" value="%STOCKS%" placeholder="600519, 000001, 00700, AAPL">
<p class="hint">A 股直接写 6 位代码；指数和深市代码冲突时加前缀，如 <b>sh000001</b> 是上证指数；
港股写 5 位代码（如 00700）；美股写代码（如 AAPL）。ETF 和股票写法一样。数据来自东方财富，交易时段每 30 秒刷新。</p>
<h2>固件升级</h2>
<label class="check"><input type="checkbox" name="auto_update" %AUTO_UPDATE%> 发现新版本后，在凌晨 3～5 点自动升级</label>
<p><button type="submit">保存</button></p>
</form>
<p class="ota">番茄钟：%POMO_STATE%</p>
<div class="row">
<form method="post" action="/pomodoro/toggle"><button>开始 / 暂停</button></form>
<form method="post" action="/pomodoro/skip"><button class="sub">跳过当前阶段</button></form>
<form method="post" action="/pomodoro/reset" onsubmit="return confirm('重置番茄钟？')"><button class="sub">重置</button></form>
<form method="post" action="/sound/test"><button class="sub">试听提示音</button></form>
</div>
<hr>
<p>当前版本：<b>%FW_VERSION%</b></p>
<p class="ota" id="ota" data-busy="%OTA_BUSY%">%OTA_TEXT%</p>
<div class="row">
<form method="post" action="/ota/check"><button class="sub">检查更新</button></form>
<form method="post" action="/ota/install" onsubmit="return confirm('现在下载并安装最新版本？升级大约需要 1 分钟，期间请不要断电。')"><button>立即升级</button></form>
</div>
<label>也可以上传固件文件（Release 里的 firmware-板子名.bin）</label>
<form method="post" action="/ota/upload" enctype="multipart/form-data" onsubmit="const b=this.querySelector('button');b.disabled=true;b.textContent='上传中，请不要关闭页面…'">
<input type="file" name="firmware" accept=".bin" required>
<p><button>上传并升级</button></p>
</form>
<hr>
<form method="post" action="/reset-wifi" onsubmit="return confirm('清除 WiFi 并重启进入配网模式？')">
<button class="danger">重新配置 WiFi</button>
</form>
</main>
<script>
async function search(){
  const q=document.getElementById('city').value.trim(); if(!q) return;
  const box=document.getElementById('results'); box.textContent='查询中…';
  try{
    const r=await fetch('https://geocoding-api.open-meteo.com/v1/search?count=5&language=zh&name='+encodeURIComponent(q));
    const j=await r.json(); box.textContent='';
    if(!j.results){box.textContent='没找到，可以手动填写经纬度';return;}
    for(const p of j.results){
      const b=document.createElement('button'); b.type='button'; b.className='sub';
      b.textContent=[p.name,p.admin1,p.country].filter(Boolean).join('，');
      b.onclick=()=>{lat.value=p.latitude.toFixed(4);lon.value=p.longitude.toFixed(4);box.textContent='';};
      box.appendChild(b);
    }
  }catch(e){box.textContent='查询失败，可以手动填写经纬度';}
}
async function poll(){
  const el=document.getElementById('ota');
  try{const j=await (await fetch('/ota/status')).json();el.textContent=j.text;if(j.busy)setTimeout(poll,2000);}
  catch(e){setTimeout(poll,3000);}
}
if(document.getElementById('ota').dataset.busy==='1')poll();
</script></body></html>)html";

static String html_escape(const String &s) {
  String out;
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    switch (c) {
      case '&': out += "&amp;"; break;
      case '<': out += "&lt;"; break;
      case '>': out += "&gt;"; break;
      case '"': out += "&quot;"; break;
      default: out += c;
    }
  }
  return out;
}

static String ota_text(const OtaStatus &s) {
  switch (s.state) {
    case OtaState::Idle: return "还没有检查过更新";
    case OtaState::Checking: return "正在检查更新…";
    case OtaState::UpToDate: return String("已是最新版本（") + s.latest + "）";
    case OtaState::Available: return String("发现新版本 ") + s.latest + "，可以点「立即升级」";
    case OtaState::Downloading: return String("正在升级：") + s.progress + "%";
    case OtaState::Done: return "升级完成，设备正在重启";
    case OtaState::Failed: return String("失败：") + s.error;
  }
  return "";
}

static String pomo_text() {
  if (!pomodoro) return "";
  const Pomodoro &p = *pomodoro;
  uint32_t secs = (p.remaining_ms(millis()) + 999) / 1000;
  char left[16];
  snprintf(left, sizeof(left), "%02u:%02u", (unsigned)(secs / 60), (unsigned)(secs % 60));
  String phase;
  switch (p.phase()) {
    case PomoPhase::Idle: return String("未开始，今日完成 ") + p.done_today() + " 个";
    case PomoPhase::Focus: phase = "专注"; break;
    case PomoPhase::ShortBreak: phase = "短休息"; break;
    case PomoPhase::LongBreak: phase = "长休息"; break;
  }
  return phase + (p.running() ? "中，剩余 " : "（已暂停），剩余 ") + left + "，今日完成 " + p.done_today() + " 个";
}

static bool ota_busy(const OtaStatus &s) {
  return s.state == OtaState::Checking || s.state == OtaState::Downloading;
}

static void handle_root() {
  Settings s = settings_get();
  String page = FPSTR(PAGE);
  page.replace("%SAVED%", server.hasArg("saved") ? "<p class=\"tip\">已保存</p>" : "");
  page.replace("%CITY%", html_escape(s.city));
  page.replace("%LAT%", String(s.lat, 4));
  page.replace("%LON%", String(s.lon, 4));
  page.replace("%BRIGHT%", String(s.brightness));
  page.replace("%PAGE_SEC%", String(s.page_seconds));
  char hhmm[8];
  page.replace("%NIGHT_ON%", s.night_on ? "checked" : "");
  night_format_hhmm(s.night_start, hhmm, sizeof(hhmm));
  page.replace("%NIGHT_START%", hhmm);
  night_format_hhmm(s.night_end, hhmm, sizeof(hhmm));
  page.replace("%NIGHT_END%", hhmm);
  page.replace("%NIGHT_BRIGHT%", String(s.night_brightness));
  page.replace("%STOCKS%", html_escape(s.stocks));
  bool has_token = s.github_token.length() > 0;
  page.replace("%GH_STATE%", has_token ? "已设置" : "未设置");
  page.replace("%GH_PLACEHOLDER%", has_token ? "已设置，留空则不修改" : "ghp_...");
  page.replace("%AUTO_UPDATE%", s.auto_update ? "checked" : "");
  page.replace("%POMO_FOCUS%", String(s.pomo_focus));
  page.replace("%POMO_SHORT%", String(s.pomo_short));
  page.replace("%POMO_LONG%", String(s.pomo_long));
  page.replace("%POMO_EVERY%", String(s.pomo_every));
  page.replace("%POMO_STATE%", html_escape(pomo_text()));
  page.replace("%SOUND_ON%", s.sound_on ? "checked" : "");
  page.replace("%SOUND_NOTE%", audio_available() ? "" : "（这块板子没有喇叭）");
  page.replace("%SOUND_VOL%", String(s.sound_volume));
  page.replace("%SOUND_QUIET%", s.sound_quiet_night ? "checked" : "");
  page.replace("%FW_VERSION%", html_escape(ota_current_version()));
  OtaStatus os = ota_status();
  page.replace("%OTA_BUSY%", ota_busy(os) ? "1" : "0");
  page.replace("%OTA_TEXT%", html_escape(ota_text(os)));
  server.send(200, "text/html; charset=utf-8", page);
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

  // 复选框不勾选时浏览器不会提交这个字段
  s.night_on = server.hasArg("night_on");
  s.auto_update = server.hasArg("auto_update");
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

  if (server.hasArg("stocks")) {
    s.stocks = server.arg("stocks");
    s.stocks.trim();
  }

  settings_save(s);
  changed = true;
  weather_request_refresh();
  github_request_refresh();
  stock_request_refresh();

  server.sendHeader("Location", "/?saved=1");
  server.send(303);
}

static void handle_pomo(PomoCmd cmd) {
  if (pomo_cmd) pomo_cmd(cmd);
  server.sendHeader("Location", "/");
  server.send(303);
}

static void redirect_to_ota() {
  server.sendHeader("Location", "/#ota");
  server.send(303);
}

static void handle_ota_status() {
  OtaStatus s = ota_status();
  JsonDocument doc;
  doc["text"] = ota_text(s);
  doc["busy"] = ota_busy(s);
  String out;
  serializeJson(doc, out);
  server.send(200, "application/json", out);
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
  String msg = upload_ok ? String("升级成功，设备正在重启，大约 10 秒后刷新设置页面即可。")
                         : String("升级失败：") + html_escape(ota_status().error);
  server.send(200, "text/html; charset=utf-8",
              "<meta charset=utf-8><meta name=viewport content='width=device-width'>"
              "<p>" + msg + "</p><p><a href='/'>返回设置页面</a></p>");
}

static void handle_reset_wifi() {
  server.send(200, "text/html; charset=utf-8",
              "<meta charset=utf-8><p>已清除 WiFi，设备正在重启进入配网模式。</p>");
  delay(500);
  WiFi.disconnect(true, true);  // 第二个参数会擦除保存的 WiFi 信息
  delay(500);
  ESP.restart();
}

void web_config_begin() {
  server.on("/", HTTP_GET, handle_root);
  server.on("/save", HTTP_POST, handle_save);
  server.on("/reset-wifi", HTTP_POST, handle_reset_wifi);
  server.on("/ota/status", HTTP_GET, handle_ota_status);
  server.on("/pomodoro/toggle", HTTP_POST, [] { handle_pomo(PomoCmd::Toggle); });
  server.on("/pomodoro/skip", HTTP_POST, [] { handle_pomo(PomoCmd::Skip); });
  server.on("/pomodoro/reset", HTTP_POST, [] { handle_pomo(PomoCmd::Reset); });
  server.on("/sound/test", HTTP_POST, [] {
    audio_play(SoundId::FocusDone);
    server.sendHeader("Location", "/");
    server.send(303);
  });
  server.on("/ota/check", HTTP_POST, [] {
    ota_check_now();
    redirect_to_ota();
  });
  server.on("/ota/install", HTTP_POST, [] {
    ota_install_now();
    redirect_to_ota();
  });
  server.on("/ota/upload", HTTP_POST, handle_upload_done, handle_upload_chunk);
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

void web_config_set_progress_hook(void (*hook)()) {
  progress_hook = hook;
}

bool web_config_take_changed() {
  bool c = changed;
  changed = false;
  return c;
}
