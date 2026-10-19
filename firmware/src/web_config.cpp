#include "web_config.h"

#include <WebServer.h>
#include <WiFi.h>

#include "settings.h"
#include "weather.h"

static WebServer server(80);
static bool changed = false;

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
hr{border:0;border-top:1px solid #30363d;margin:28px 0}
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
<p><button type="submit">保存</button></p>
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

static void handle_root() {
  Settings s = settings_get();
  String page = FPSTR(PAGE);
  page.replace("%SAVED%", server.hasArg("saved") ? "<p class=\"tip\">已保存</p>" : "");
  page.replace("%CITY%", html_escape(s.city));
  page.replace("%LAT%", String(s.lat, 4));
  page.replace("%LON%", String(s.lon, 4));
  page.replace("%BRIGHT%", String(s.brightness));
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

  settings_save(s);
  changed = true;
  weather_request_refresh();

  server.sendHeader("Location", "/?saved=1");
  server.send(303);
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
  server.onNotFound([] { server.send(404, "text/plain", "Not found"); });
  server.begin();
}

void web_config_loop() {
  server.handleClient();
}

bool web_config_take_changed() {
  bool c = changed;
  changed = false;
  return c;
}
