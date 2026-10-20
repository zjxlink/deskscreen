#include "web_config.h"

#include <ArduinoJson.h>
#include <WebServer.h>
#include <WiFi.h>

#include "audio.h"
#include "display.h"
#include "github.h"
#include "net.h"
#include "night.h"
#include "ota.h"
#include "photos.h"
#include "settings.h"
#include "stock.h"
#include "voice.h"
#include "weather.h"

static WebServer server(80);
static bool changed = false;
static void (*progress_hook)() = nullptr;
static const Pomodoro *pomodoro = nullptr;
static void (*pomo_cmd)(PomoCmd) = nullptr;
static const PomoStats *pomo_stats = nullptr;
static int (*today)() = nullptr;
static void (*message_hook)(const String &, bool) = nullptr;
static String (*room_hook)() = nullptr;
static constexpr int kMessageMaxChars = 60;

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
textarea{width:100%;box-sizing:border-box;padding:10px;border-radius:8px;border:1px solid #30363d;background:#161b22;color:#e6edf3;font-size:16px;font-family:inherit;resize:vertical}
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
<h2>留言板</h2>
<form method="post" action="/message">
<textarea name="text" rows="2" maxlength="60" placeholder="写一句话，会立即显示在屏幕上" required></textarea>
<label class="check"><input type="checkbox" name="sound" checked> 叮咚响一声%SOUND_NOTE%</label>
<p><button>发送到屏幕</button></p>
</form>
<p class="hint">最多 60 个字，点屏幕或按 BOOT 键关闭。表情符号显示不出来。</p>
<hr>
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
<h2>健康提醒</h2>
<div class="row">
<div><label class="check"><input type="checkbox" name="sit_on" %SIT_ON%> 久坐提醒</label></div>
<div><label>每隔（分钟）</label><input name="sit_min" value="%SIT_MIN%" inputmode="numeric"></div>
</div>
<div class="row">
<div><label class="check"><input type="checkbox" name="water_on" %WATER_ON%> 喝水提醒</label></div>
<div><label>每隔（分钟）</label><input name="water_min" value="%WATER_MIN%" inputmode="numeric"></div>
</div>
<div class="row">
<div><label>提醒时段开始</label><input type="time" name="remind_start" value="%REMIND_START%"></div>
<div><label>结束</label><input type="time" name="remind_end" value="%REMIND_END%"></div>
</div>
<h2>闹钟和整点报时</h2>
<label class="check"><input type="checkbox" name="chime_on" %CHIME_ON%> 整点报时（每到整点叮一声，夜间时段跟提示音一样不响）</label>
<div class="row">
<div><label class="check"><input type="checkbox" name="alarm_on" %ALARM_ON%> 闹钟</label></div>
<div><label>时间</label><input type="time" name="alarm_time" value="%ALARM_TIME%"></div>
</div>
<div class="row" style="flex-wrap:wrap;margin-top:8px">%ALARM_DAYS%</div>
<p class="hint">闹钟响时屏幕全屏提示、每隔几秒响一次，夜里也会响；点屏幕或按 BOOT 键关掉，5 分钟后自动停止。%NO_SPEAKER%</p>
<p class="hint">只在提醒时段内提醒，屏幕会弹出全屏提示并响一声（音量和夜间静音跟番茄钟一样）。番茄钟专注时先不打扰，等休息时再提醒；开始休息时久坐提醒重新计时。%REMIND_STATE%</p>
<h2>室内温湿度和人体感应</h2>
<p class="hint">%ROOM_STATE%</p>
<label class="check"><input type="checkbox" name="climate_on" %CLIMATE_ON%> 太热或太闷时提醒（持续 3 分钟才提醒，同一种提醒 1 小时最多一次，夜间时段不提醒）</label>
<div class="row">
<div><label>温度达到（°C）</label><input name="hot_c" value="%HOT_C%" inputmode="numeric"></div>
<div><label>湿度达到（%）</label><input name="humid_pct" value="%HUMID_PCT%" inputmode="numeric"></div>
<div><label>温度校准（°C）</label><input name="temp_offset" value="%TEMP_OFFSET%" inputmode="decimal"></div>
</div>
<p class="hint">温度校准：如果显示的温度比实际偏高 1.5 度，就填 -1.5。</p>
<div class="row">
<div><label class="check"><input type="checkbox" name="away_off" %AWAY_OFF%> 没人时熄屏，有人回来自动亮</label></div>
<div><label>多久没人算离开（分钟）</label><input name="away_min" value="%AWAY_MIN%" inputmode="numeric"></div>
</div>
<p class="hint">雷达感应的是动作，人坐着一动不动时可能感应不到，所以离开时间别设太短。熄屏后点屏幕或按 BOOT 键也能亮；有提醒时会自动亮屏。</p>
<h2>AI 语音聊天</h2>
<p class="hint">%AI_STATE%在「AI 聊天」页按住麦克风按钮说话，松手后小橘用语音回答。用的是阿里云百炼的 Qwen-Omni 模型，按用量计费，新用户有免费额度。</p>
<label>百炼 API Key（%AI_KEY_STATE%）</label>
<input type="password" name="ai_key" placeholder="%AI_KEY_PLACEHOLDER%" autocomplete="off">
<p class="hint">登录 <a href="https://bailian.console.aliyun.com/" target="_blank">阿里云百炼控制台</a>，在「API Key」里创建一个（北京地域），复制粘贴到这里。Key 只保存在设备上，不会显示出来。</p>
<label class="check"><input type="checkbox" name="ai_clear"> 清除已保存的 Key</label>
<div class="row">
<div><label>模型</label><input name="ai_model" value="%AI_MODEL%"></div>
<div><label>音色</label><input name="ai_voice" value="%AI_VOICE%"></div>
</div>
<p class="hint">模型默认 qwen3-omni-flash；音色默认 Cherry（女声），也可以填 Ethan（男声）等百炼文档里列出的音色名。</p>
<h2>电子相册</h2>
<p class="hint">%ALBUM_STATE%</p>
<div class="row">
<div><label>每张照片显示（秒）</label><input name="album_sec" value="%ALBUM_SEC%" inputmode="numeric"></div>
<div><label class="check" style="margin-top:38px"><input type="checkbox" name="album_shuffle" %ALBUM_SHUFFLE%> 随机顺序</label></div>
</div>
<div class="row">
<div><label class="check"><input type="checkbox" name="saver_on" %SAVER_ON%> 空闲时自动播放相册（屏保）</label></div>
<div><label>多久没碰屏幕算空闲（分钟）</label><input name="saver_min" value="%SAVER_MIN%" inputmode="numeric"></div>
</div>
<p class="hint">点屏幕切到「相册」页就开始播放，会一直放下去不自动切走。屏保：碰一下屏幕或按 BOOT 键回到原来的页面；番茄钟进行中、夜间时段和没人熄屏时不会启动。</p>
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
<h2>网络诊断</h2>
<ul class="hint">%NET_STATUS%<li>%PANEL_STATUS%</li></ul>
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

static const char ALBUM_PAGE[] PROGMEM = R"html(<!doctype html>
<html lang="zh-CN"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>电子相册</title>
<style>
body{font-family:-apple-system,system-ui,sans-serif;background:#0d1117;color:#e6edf3;margin:0;padding:16px}
main{max-width:420px;margin:0 auto}
h1{font-size:20px}
h2{font-size:17px;margin-top:28px}
a{color:#58a6ff}
label{display:block;margin:16px 0 6px;color:#8b949e;font-size:14px}
input{width:100%;box-sizing:border-box;padding:10px;border-radius:8px;border:1px solid #30363d;background:#161b22;color:#e6edf3;font-size:16px}
button{padding:10px 16px;border:0;border-radius:8px;background:#238636;color:#fff;font-size:16px}
button:disabled{opacity:.6}
.hint{color:#8b949e;font-size:13px;line-height:1.6}
.tip{color:#3fb950;margin:8px 0}
.grid{display:grid;grid-template-columns:repeat(3,1fr);gap:8px}
.ph{position:relative;background:#161b22;border-radius:6px;aspect-ratio:4/3;overflow:hidden;display:flex;align-items:center;justify-content:center;font-size:11px;color:#8b949e;word-break:break-all;text-align:center}
.ph img{width:100%;height:100%;object-fit:contain}
.ph button{position:absolute;top:4px;right:4px;padding:2px 8px;font-size:14px;background:#da3633cc;border-radius:12px}
</style></head><body><main>
<p><a href="/">← 返回设置</a></p>
<h1>电子相册</h1>
<p class="hint" id="state">正在读取 TF 卡…</p>
<div id="up" hidden>
<label>选择照片（可以一次选多张）</label>
<input type="file" id="files" accept="image/*" multiple>
<p class="hint">照片会先在手机上缩小到屏幕大小（320x240）再上传，每张只有几十 KB，很快；iPhone 的 HEIC 照片也会自动转成 JPG。</p>
<p><button id="go" onclick="upload()">上传到相框</button></p>
<p class="tip" id="msg"></p>
</div>
<h2 id="count"></h2>
<div class="grid" id="grid"></div>
</main>
<script>
const $=id=>document.getElementById(id);
function fmt(b){return b>=1073741824?(b/1073741824).toFixed(1)+' GB':Math.round(b/1048576)+' MB';}
async function load(){
  let j;
  try{j=await (await fetch('/album/list')).json();}catch(e){$('state').textContent='读取失败，刷新页面再试';return;}
  const grid=$('grid');grid.textContent='';$('count').textContent='';
  if(j.card==='noslot'){$('state').textContent='这块板子没有 TF 卡槽，用不了相册。';return;}
  if(j.card!=='ok'){$('state').textContent='没有检测到 TF 卡。请插入 FAT32 格式的 TF 卡（32GB 以内的卡出厂就是 FAT32），插好后刷新这个页面。';$('up').hidden=true;return;}
  $('up').hidden=false;
  $('state').textContent='TF 卡已用 '+fmt(j.used)+'，共 '+fmt(j.total)+'。照片在卡里的 photos 文件夹，也可以拔下卡用电脑管理。'+
    (j.failed?'有 '+j.failed+' 张照片读不出来（可能是 HEIC 或渐进式 JPEG），可以删掉后在这里重新上传。':'');
  $('count').textContent='照片（'+j.photos.length+' 张）';
  for(const p of j.photos){
    const d=document.createElement('div');d.className='ph';
    if(p.s<=1048576){const i=document.createElement('img');i.loading='lazy';i.src='/album/file?name='+encodeURIComponent(p.n);i.alt=p.n;d.appendChild(i);}
    else d.textContent=p.n+'（原图，不显示缩略图）';
    const b=document.createElement('button');b.textContent='×';b.title='删除';
    b.onclick=async()=>{if(!confirm('删除 '+p.n+'？'))return;
      const r=await fetch('/album/delete',{method:'POST',body:new URLSearchParams({name:p.n})});
      if(!(await r.json()).ok)alert('删除失败');load();};
    d.appendChild(b);grid.appendChild(d);
  }
}
function canvas(w,h){const c=document.createElement('canvas');c.width=w;c.height=h;const x=c.getContext('2d');x.imageSmoothingQuality='high';return [c,x];}
// 缩小到能完整放进 320x240（不放大），转成 JPG；每次缩一半，最后一步缩到目标大小，比一步缩到位清晰
async function shrink(file){
  let src;
  try{src=await createImageBitmap(file,{imageOrientation:'from-image'});}
  catch(e){src=await new Promise((ok,no)=>{const i=new Image();i.onload=()=>ok(i);i.onerror=no;i.src=URL.createObjectURL(file);});}
  let w=src.width,h=src.height;const s=Math.min(1,320/w,240/h);
  const tw=Math.max(1,Math.round(w*s)),th=Math.max(1,Math.round(h*s));
  let cur=src;
  while(w>tw*2){const [c,x]=canvas(Math.round(w/2),Math.round(h/2));x.drawImage(cur,0,0,c.width,c.height);cur=c;w=c.width;h=c.height;}
  const [c,x]=canvas(tw,th);x.fillStyle='#000';x.fillRect(0,0,tw,th);x.drawImage(cur,0,0,tw,th);
  return await new Promise(r=>c.toBlob(r,'image/jpeg',0.88));
}
async function upload(){
  const files=[...$('files').files];if(!files.length){$('msg').textContent='先选择照片';return;}
  const go=$('go');go.disabled=true;let ok=0;const bad=[];
  for(let i=0;i<files.length;i++){
    $('msg').textContent='正在上传 '+(i+1)+' / '+files.length+'…';
    try{
      const blob=await shrink(files[i]);
      const fd=new FormData();fd.append('photo',blob,files[i].name.replace(/\.[^.]*$/,'')+'.jpg');
      const j=await (await fetch('/album/upload',{method:'POST',body:fd})).json();
      if(j.ok)ok++;else bad.push(files[i].name+'（'+j.error+'）');
    }catch(e){bad.push(files[i].name+'（浏览器打不开这张图）');}
  }
  $('msg').textContent='上传了 '+ok+' 张'+(bad.length?'，失败：'+bad.join('，'):'');
  go.disabled=false;$('files').value='';load();
}
load();
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
    case PomoPhase::Idle: return "未开始";
    case PomoPhase::Focus: phase = "专注"; break;
    case PomoPhase::ShortBreak: phase = "短休息"; break;
    case PomoPhase::LongBreak: phase = "长休息"; break;
  }
  return phase + (p.running() ? "中，剩余 " : "（已暂停），剩余 ") + left;
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
  bool has_ai = s.ai_key.length() > 0;
  page.replace("%AI_STATE%", voice_available() ? "" : "这块板子没有麦克风，用不了语音聊天。");
  page.replace("%AI_KEY_STATE%", has_ai ? "已设置" : "未设置");
  page.replace("%AI_KEY_PLACEHOLDER%", has_ai ? "已设置，留空则不修改" : "sk-...");
  page.replace("%AI_MODEL%", html_escape(s.ai_model));
  page.replace("%AI_VOICE%", html_escape(s.ai_voice));
  page.replace("%ALBUM_STATE%", photos_supported()
                                    ? "照片放在 TF 卡的 photos 文件夹里，支持 JPG 和 PNG。<a href=\"/album\">用手机上传和管理照片 →</a>"
                                    : "这块板子没有 TF 卡槽，用不了相册。");
  page.replace("%ALBUM_SEC%", String(s.album_sec));
  page.replace("%ALBUM_SHUFFLE%", s.album_shuffle ? "checked" : "");
  page.replace("%SAVER_ON%", s.saver_on ? "checked" : "");
  page.replace("%SAVER_MIN%", String(s.saver_min));
  page.replace("%POMO_FOCUS%", String(s.pomo_focus));
  page.replace("%POMO_SHORT%", String(s.pomo_short));
  page.replace("%POMO_LONG%", String(s.pomo_long));
  page.replace("%POMO_EVERY%", String(s.pomo_every));
  String stats = stats_text();
  page.replace("%POMO_STATE%", html_escape(stats.length() ? pomo_text() + "。" + stats : pomo_text()));
  page.replace("%SIT_ON%", s.sit_on ? "checked" : "");
  page.replace("%SIT_MIN%", String(s.sit_min));
  page.replace("%WATER_ON%", s.water_on ? "checked" : "");
  page.replace("%WATER_MIN%", String(s.water_min));
  night_format_hhmm(s.remind_start, hhmm, sizeof(hhmm));
  page.replace("%REMIND_START%", hhmm);
  night_format_hhmm(s.remind_end, hhmm, sizeof(hhmm));
  page.replace("%REMIND_END%", hhmm);
  page.replace("%CHIME_ON%", s.chime_on ? "checked" : "");
  page.replace("%ALARM_ON%", s.alarm_on ? "checked" : "");
  night_format_hhmm(s.alarm_time, hhmm, sizeof(hhmm));
  page.replace("%ALARM_TIME%", hhmm);
  static const char *const day_names[] = {"日", "一", "二", "三", "四", "五", "六"};
  String days;
  for (int i = 1; i <= 7; i++) {  // 从周一排到周日
    int d = i % 7;
    days += String("<label class=\"check\" style=\"margin:0 12px 0 0\"><input type=\"checkbox\" name=\"ad") + d +
            "\" " + ((s.alarm_days & (1 << d)) ? "checked" : "") + "> 周" + day_names[d] + "</label>";
  }
  page.replace("%ALARM_DAYS%", days);
  page.replace("%NO_SPEAKER%", audio_available() ? "" : "这块板子没有喇叭，只会在屏幕上提示。");
  page.replace("%REMIND_STATE%", s.sit_on || s.water_on ? "" : "两个提醒默认都关着。");
  page.replace("%ROOM_STATE%", room_hook ? html_escape(room_hook()) : "");
  page.replace("%CLIMATE_ON%", s.climate_on ? "checked" : "");
  page.replace("%HOT_C%", String(s.hot_c));
  page.replace("%HUMID_PCT%", String(s.humid_pct));
  page.replace("%TEMP_OFFSET%", String(s.temp_offset / 10.0f, 1));
  page.replace("%AWAY_OFF%", s.away_off ? "checked" : "");
  page.replace("%AWAY_MIN%", String(s.away_min));
  page.replace("%SOUND_ON%", s.sound_on ? "checked" : "");
  page.replace("%SOUND_NOTE%", audio_available() ? "" : "（这块板子没有喇叭）");
  page.replace("%SOUND_VOL%", String(s.sound_volume));
  page.replace("%SOUND_QUIET%", s.sound_quiet_night ? "checked" : "");
  page.replace("%FW_VERSION%", html_escape(ota_current_version()));
  OtaStatus os = ota_status();
  page.replace("%OTA_BUSY%", ota_busy(os) ? "1" : "0");
  page.replace("%OTA_TEXT%", html_escape(ota_text(os)));
  page.replace("%NET_STATUS%", net_status_html());
  String panel = String("屏幕：已运行 ") + millis() / 3600000 + " 小时，";
  if (display_panel_watching()) {
    panel += String("自动恢复白屏、花屏 ") + display_panel_recoveries() + " 次";
  } else {
    panel += "读不出屏幕状态，每 10 分钟重新初始化一次";
  }
  page.replace("%PANEL_STATUS%", panel);
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
  minutes_arg("sit_min", s.sit_min, 10, 240);
  minutes_arg("water_min", s.water_min, 10, 240);
  s.sit_on = server.hasArg("sit_on");
  s.water_on = server.hasArg("water_on");
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

  server.sendHeader("Location", "/?saved=1");
  server.send(303);
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
  server.sendHeader("Location", "/");
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

// 电子相册：照片列表、上传、删除、缩略图
static constexpr size_t kPhotoMaxBytes = 4 * 1024 * 1024;  // 网页会先缩小，正常每张几十 KB
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
    photo_ok = photos_upload_begin(photo_name);
    photo_error = photo_ok ? "" : "没有 TF 卡或者卡满了";
  } else if (up.status == UPLOAD_FILE_WRITE && photo_ok) {
    if (up.totalSize > kPhotoMaxBytes) {
      photo_ok = false;
      photo_error = "照片太大";
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

static void handle_photo_file() {
  String name = server.arg("name");
  String lower = name;
  lower.toLowerCase();
  bool png = lower.endsWith(".png");
  bool ok = photos_read(name, [](void *ctx, const uint8_t *data, size_t len, size_t total) {
    if (!data) {  // 开头先发一次文件大小
      server.sendHeader("Cache-Control", "max-age=3600");
      server.setContentLength(total);
      server.send(200, *(bool *)ctx ? "image/png" : "image/jpeg", "");
      return;
    }
    server.sendContent((const char *)data, len);
  }, &png);
  if (!ok) server.send(404, "text/plain", "Not found");
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
  server.on("/message", HTTP_POST, handle_message);
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
  server.on("/album", HTTP_GET, [] { server.send_P(200, "text/html; charset=utf-8", ALBUM_PAGE); });
  server.on("/album/list", HTTP_GET, [] { server.send(200, "application/json", photos_list_json()); });
  server.on("/album/file", HTTP_GET, handle_photo_file);
  server.on("/album/upload", HTTP_POST, [] { send_json(photo_ok, photo_error); }, handle_photo_chunk);
  server.on("/album/delete", HTTP_POST, [] {
    photo_name = "";
    send_json(photos_delete(server.arg("name")), "删除失败");
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

void web_config_set_room_hook(String (*hook)()) {
  room_hook = hook;
}

void web_config_set_progress_hook(void (*hook)()) {
  progress_hook = hook;
}

bool web_config_take_changed() {
  bool c = changed;
  changed = false;
  return c;
}
