#include "wifi_web.h"

#include "bindings.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <BleKeyboard.h>
#include <Preferences.h>
#include <Update.h>
#include <WebServer.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <esp_coexist.h>

extern Preferences prefs;
extern uint8_t platform;
extern bool bleConn;
extern bool dirty;

static WebServer server(80);
static bool wifiStarted = false;
static uint32_t lastWifiTry = 0;
static char ipStr[16] = "0.0.0.0";
static char ssidShown[33] = "";
static bool otaOK = false;
static bool staTrying = false;

static void startSta(const String& ssid, const String& pass);

static String jsonEscape(const String& s) {
  String o;
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    if (c == '\\') o += "\\\\";
    else if (c == '"') o += "\\\"";
    else o += c;
  }
  return o;
}

static void appendComboJson(String& json, const KeyCombo& c, bool advSide) {
  json += "{\"mods\":[";
  for (uint8_t i = 0; i < c.nmod; i++) {
    if (i) json += ",";
    json += "\"";
    if (advSide) json += physKeyName(c.mods[i]);
    else json += hidModName(c.mods[i]);
    json += "\"";
  }
  json += "],\"keys\":[";
  for (uint8_t i = 0; i < c.nkey; i++) {
    if (i) json += ",";
    json += "\"";
    if (advSide) {
      const char* n = physKeyName(c.keys[i]);
      if (n[0] == '\\') json += "\\\\";
      else if (n[0] == '"') json += "\\\"";
      else json += n;
    } else {
      char kn[16];
      hidKeyToName(c.keys[i], kn, sizeof(kn));
      json += jsonEscape(String(kn));
    }
    json += "\"";
  }
  json += "]}";
}

static bool parseComboJson(JsonObject o, KeyCombo& c, bool advSide) {
  memset(&c, 0, sizeof(c));
  if (o.isNull()) return false;
  JsonArray mods = o["mods"].as<JsonArray>();
  JsonArray keys = o["keys"].as<JsonArray>();
  if (!mods.isNull()) {
    for (JsonVariant v : mods) {
      uint8_t m = advSide ? physKeyFromName(v.as<const char*>())
                          : hidModFromName(v.as<const char*>());
      if (m && c.nmod < 4) c.mods[c.nmod++] = m;
    }
  }
  if (!keys.isNull()) {
    for (JsonVariant v : keys) {
      uint8_t k = advSide ? physKeyFromName(v.as<const char*>())
                          : hidKeyFromName(v.as<const char*>());
      if (k && c.nkey < 6) c.keys[c.nkey++] = k;
    }
  }
  // 规则：无修饰 → 必须恰好 1 键；有修饰 → 键数随意（含 0）
  if (c.nmod == 0) return c.nkey == 1;
  return c.nmod >= 1;
}

static const char PAGE_HTML[] PROGMEM = R"HTML(<!DOCTYPE html>
<html lang="zh-CN" data-theme="night"><head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1,viewport-fit=cover">
<title>Cardputer Keyboard</title>
<link rel="preconnect" href="https://fonts.googleapis.com">
<link rel="preconnect" href="https://fonts.gstatic.com" crossorigin>
<link href="https://fonts.googleapis.com/css2?family=Inter:wght@400;500;700;800&family=Space+Grotesk:wght@500;700&display=swap" rel="stylesheet">
<style>
:root{
  --green:#006941;--green-dim:#005c38;--green-soft:#7bfeb8;
  --zinc-50:#fafafa;--zinc-200:#e4e4e7;--zinc-400:#a1a1aa;--zinc-500:#71717a;--zinc-600:#52525b;--zinc-700:#3f3f46;--zinc-800:#27272a;--zinc-900:#18181b;--zinc-950:#09090b;--white:#fff;
  --accent:var(--green);--accent-dim:var(--green-dim);--accent-soft:var(--green-soft);
  --fg-0:#2d2f2f;--fg-1:#5a5c5c;--fg-2:#9a9c9c;--fg-inv:var(--white);
  --bg-0:#f6f6f6;--bg-1:var(--white);--bg-2:#f0f1f1;--line:var(--zinc-200);--line-strong:var(--zinc-400);
  --font-headline:'Inter',system-ui,sans-serif;--font-body:'Inter',system-ui,sans-serif;--font-label:'Space Grotesk',system-ui,sans-serif;--font-mono:ui-monospace,Menlo,Consolas,monospace;
  --r-1:4px;--r-2:8px;--r-full:9999px;
  --sp-2:8px;--sp-3:12px;--sp-4:16px;--sp-5:20px;--sp-6:24px;
  --shadow-focus:0 0 0 3px rgba(0,105,65,.18);
  --ease-std:cubic-bezier(.4,0,.2,1);--dur-fast:100ms;--dur-base:150ms;
  --key:var(--zinc-800);--keymod:#0f2a1c;
}
[data-theme=night]{
  --fg-0:var(--white);--fg-1:var(--zinc-400);--fg-2:var(--zinc-600);--fg-inv:var(--zinc-950);
  --bg-0:var(--zinc-950);--bg-1:var(--zinc-900);--bg-2:var(--zinc-800);
  --line:var(--zinc-700);--line-strong:var(--zinc-500);
}
*{box-sizing:border-box}html{overscroll-behavior:contain}
body{margin:0;min-height:100dvh;background:var(--bg-0);color:var(--fg-0);font-family:var(--font-body);font-size:16px;line-height:1.5;-webkit-tap-highlight-color:transparent}
button,.k,.ck{cursor:pointer}
button:focus-visible,.k:focus-visible,input:focus-visible{outline:none;box-shadow:var(--shadow-focus)}
.wrap{max-width:1240px;margin:0 auto;padding:var(--sp-4) var(--sp-4) 100px}
.speed-line{height:1px;width:100%;background:var(--accent);margin:0 0 var(--sp-4)}
header{display:flex;flex-wrap:wrap;gap:var(--sp-3);justify-content:space-between;align-items:flex-start;margin-bottom:var(--sp-4)}
h1{margin:0;font-family:var(--font-headline);font-size:20px;font-weight:800;letter-spacing:-.02em}
.sub{margin:6px 0 0;font-family:var(--font-label);font-size:11px;font-weight:700;letter-spacing:.08em;text-transform:uppercase;color:var(--fg-2)}
.chips{display:flex;flex-wrap:wrap;gap:var(--sp-2)}
.chip{display:inline-flex;align-items:center;gap:6px;min-height:28px;padding:4px 10px;border-radius:var(--r-full);border:1px solid var(--line);background:var(--bg-2);font-family:var(--font-label);font-size:11px;font-weight:700;letter-spacing:.06em;text-transform:uppercase;color:var(--fg-1)}
.chip b{color:var(--fg-0);font-weight:700;text-transform:none;letter-spacing:0;font-family:var(--font-mono);font-size:12px}
.dot{width:6px;height:6px;border-radius:50%;background:var(--fg-2)}.dot.on{background:var(--accent);box-shadow:0 0 0 3px rgba(0,105,65,.25)}.dot.warn{background:var(--zinc-500)}
.tabs{display:grid;grid-template-columns:repeat(3,1fr);gap:var(--sp-2);margin-bottom:var(--sp-4)}
.tabs button{min-height:44px;border-radius:var(--r-1);border:1px solid var(--line);background:var(--bg-1);color:var(--fg-1);font-family:var(--font-label);font-weight:700;font-size:12px;letter-spacing:.06em;text-transform:uppercase}
.tabs button.on{background:rgba(0,105,65,.16);border-color:var(--accent);color:var(--fg-0)}
.tabs button.hidden{display:none}
.panel{display:none}.panel.on{display:block}
.card{background:var(--bg-1);border:1px solid var(--line);border-radius:var(--r-2);padding:var(--sp-4);margin:0 0 var(--sp-3)}
.card h2{margin:0 0 var(--sp-3);font-family:var(--font-headline);font-size:18px;font-weight:700}
.input-label,label{display:block;margin:0 0 6px;font-family:var(--font-label);font-size:11px;font-weight:700;letter-spacing:.06em;text-transform:uppercase;color:var(--fg-1)}
.input,input[type=password]{width:100%;min-height:44px;padding:10px 14px;border-radius:var(--r-1);border:1.5px solid var(--line);background:var(--bg-0);color:var(--fg-0);font-family:var(--font-body);font-size:16px}
.input:focus,input[type=password]:focus{border-color:var(--accent);box-shadow:var(--shadow-focus);outline:none}
.row{display:flex;gap:var(--sp-2);align-items:center}.row.grow>*{flex:1;min-width:0}
.btn{display:inline-flex;align-items:center;justify-content:center;gap:var(--sp-2);min-height:44px;padding:10px var(--sp-5);border:none;border-radius:var(--r-1);font-family:var(--font-label);font-weight:700;font-size:14px;letter-spacing:.04em;text-transform:uppercase;white-space:nowrap;writing-mode:horizontal-tb;flex:0 0 auto;transition:all var(--dur-base) var(--ease-std);background:var(--bg-2);color:var(--fg-0)}
.btn:active{transform:scale(.97)}.btn:disabled{opacity:.5;cursor:not-allowed}.btn:disabled:active{transform:none}
.btn-primary,.btn-a{background:var(--accent);color:var(--fg-inv)}.btn-primary:hover,.btn-a:hover{background:var(--accent-dim)}
.btn-ghost,.btn-g{background:transparent;color:var(--accent);border:1.5px solid var(--accent);padding:9px var(--sp-5)}
.btn-ghost:hover,.btn-g:hover{background:rgba(0,105,65,.08)}
.btn-surface{background:var(--bg-1);color:var(--fg-0);border:1px solid var(--line);padding:9px var(--sp-5)}
.btn-surface:hover{background:var(--bg-2);border-color:var(--line-strong)}
.btn.full{width:100%}
.file-pick{display:flex;flex-wrap:wrap;align-items:center;gap:var(--sp-3);margin:var(--sp-3) 0}
.file-pick input[type=file]{position:absolute;width:1px;height:1px;opacity:0;overflow:hidden}
.file-name{font-family:var(--font-mono);font-size:12px;color:var(--fg-2)}
.hint{margin:8px 0 0;font-size:13px;color:var(--fg-2);line-height:1.45}
.seg{display:grid;grid-template-columns:1fr 1fr;gap:var(--sp-2)}
.seg button{min-height:44px;border-radius:var(--r-1);border:1px solid var(--line);background:var(--bg-0);color:var(--fg-1);font-family:var(--font-label);font-weight:700;font-size:12px;letter-spacing:.04em;text-transform:uppercase}
.seg button.on{background:rgba(0,105,65,.16);border-color:var(--accent);color:var(--fg-0)}
.nets{display:flex;flex-direction:column;gap:var(--sp-2);margin-top:var(--sp-3);max-height:240px;overflow:auto}
.net{display:flex;justify-content:space-between;gap:10px;min-height:48px;padding:12px 14px;border-radius:var(--r-1);border:1px solid var(--line);background:var(--bg-0);color:var(--fg-0);text-align:left;font-family:var(--font-body)}
.net.on{border-color:var(--accent);background:rgba(0,105,65,.12)}.net .m{color:var(--fg-2);font-family:var(--font-mono);font-size:12px}
.preview{display:flex;flex-wrap:wrap;justify-content:space-between;align-items:center;gap:var(--sp-3);padding:var(--sp-3);border-radius:var(--r-1);background:var(--bg-0);border:1px solid var(--line);margin:var(--sp-3) 0}
.preview .live{font-family:var(--font-headline);font-weight:700;font-size:1.05rem}
.preview .actions{display:flex;flex-direction:row;flex-wrap:nowrap;gap:var(--sp-2);flex:0 0 auto;align-items:center}
.preview .actions .btn{min-width:7.5rem}
.combo-chips{display:flex;flex-wrap:wrap;gap:6px;min-height:28px;margin:0 0 8px}
.combo-chips span{display:inline-flex;align-items:center;min-height:28px;padding:0 10px;border-radius:var(--r-full);background:rgba(0,105,65,.14);border:1px solid var(--accent);color:var(--green-soft);font-family:var(--font-label);font-size:11px;font-weight:700;letter-spacing:.04em}
.kb-shell{overflow-x:auto;padding-bottom:4px}
.kb{display:flex;flex-direction:column;gap:5px;min-width:640px;width:100%}
.kr{display:flex;gap:4px;width:100%}
.k{display:inline-flex;align-items:center;justify-content:center;height:44px;padding:0 4px;border-radius:var(--r-1);border:1px solid var(--line);background:var(--key);color:var(--fg-0);font-family:var(--font-label);font-size:11px;font-weight:700;flex:1 1 0;min-width:0;user-select:none}
.k.mod{background:var(--keymod);color:var(--green-soft)}.k.on{background:var(--accent);border-color:var(--accent-dim);color:var(--fg-inv)}.k.mod.on{background:var(--accent-dim);border-color:var(--accent);color:var(--fg-inv)}
.k.dim{opacity:.35}.k.u125{flex:1.25 1 0}.k.u15{flex:1.5 1 0}.k.u175{flex:1.75 1 0}.k.u2{flex:2 1 0}.k.u225{flex:2.25 1 0}.k.u275{flex:2.75 1 0}.k.u625{flex:6.25 1 0}
.kb-sec{margin:0 0 10px}.kb-sec .hint{margin:0 0 6px;font-family:var(--font-label);font-size:11px;font-weight:700;letter-spacing:.06em;text-transform:uppercase;color:var(--fg-2)}
.kb-sec .kr{display:flex;gap:4px;width:100%;margin:0 0 5px}.kb-sec .k{flex:1 1 0;min-width:0;height:40px;font-size:10px}
.kb-sec.media .k{background:#152033;border-color:#243044}.kb-sec.media .k.on{background:var(--accent);border-color:var(--accent-dim)}
.fullkb{display:grid;grid-template-columns:minmax(0,1fr) 118px 156px;gap:12px;min-width:1040px;align-items:start}
.fullkb-main,.fullkb-nav{display:flex;flex-direction:column;gap:4px}
.fullkb .kr{display:flex;gap:3px;width:100%}
.fullkb .k{height:34px;font-size:10px;padding:0 2px;flex:1 1 0;min-width:0}
.fullkb .gap{flex:0.6 1 0;min-width:6px;pointer-events:none;border:none;background:transparent;height:34px;visibility:hidden}
.fullkb .nav-gap{height:34px;flex-shrink:0}
.fullkb-num-wrap{display:flex;flex-direction:column;gap:4px;width:156px}
.fullkb-num-cap{font-family:var(--font-label);font-size:9px;font-weight:700;letter-spacing:.04em;text-transform:uppercase;color:var(--fg-2);line-height:1.2}
.fullkb-num{display:grid;grid-template-columns:repeat(4,minmax(0,1fr));grid-template-rows:repeat(5,34px);gap:3px;width:156px}
.fullkb-num .k{width:100%;height:100%;flex:none}
.fullkb-num .k.span2r{grid-row:span 2;height:100%}
.fullkb-num .k.span2c{grid-column:span 2}
.fullkb-num .k.num-led{box-shadow:inset 0 0 0 1px var(--accent);color:var(--green-soft)}
.fullkb-num .k.dead{opacity:.25;pointer-events:none}
.k.u1{flex:1 1 0}.k.u3{flex:3 1 0}.k.u4{flex:4 1 0}.k.u5{flex:5 1 0}.k.u6{flex:6 1 0}.k.tall{height:76px}
.kr.indent-05{padding-left:calc((100% - 13*4px)/15 * .5)}
.kr.indent-075{padding-left:calc((100% - 12*4px)/14 * .75)}
.ckb{display:flex;flex-direction:column;gap:4px;overflow-x:auto}
.cr{display:grid;grid-template-columns:repeat(14,minmax(26px,1fr));gap:3px;min-width:460px}
.ck{min-width:0;min-height:44px;padding:0 2px;border-radius:var(--r-1);border:1px solid var(--line);background:var(--key);color:var(--fg-0);font-family:var(--font-label);font-size:10px;font-weight:700;line-height:1.1}
.ck.mod{background:var(--keymod);color:var(--green-soft)}.ck.on{background:var(--accent);border-color:var(--accent-dim);color:var(--fg-inv)}.ck.mod.on{background:var(--accent-dim);color:var(--fg-inv)}.ck.dim{opacity:.35}
.tbl-wrap{overflow:auto;border:1px solid var(--line);border-radius:var(--r-1)}
table.map{width:100%;border-collapse:collapse;font-size:14px;min-width:480px}
table.map th,table.map td{padding:12px 14px;border-bottom:1px solid var(--line);text-align:left}
table.map th{background:var(--bg-0);color:var(--fg-2);font-family:var(--font-label);font-weight:700;font-size:11px;letter-spacing:.06em;text-transform:uppercase}
table.map tr:last-child td{border-bottom:none}
table.map .arrow{color:var(--fg-2);text-align:center;width:36px}
table.map .del{min-height:34px;padding:0 12px;font-size:12px}
.mask{position:fixed;inset:0;background:rgba(9,9,11,.72);backdrop-filter:blur(4px);z-index:50;display:none;align-items:flex-end;justify-content:center;padding:var(--sp-4)}
.mask.show{display:flex}
.modal{width:100%;max-width:820px;max-height:90dvh;overflow:auto;background:var(--bg-1);border:1px solid var(--line);border-radius:var(--r-2);padding:var(--sp-4)}
.modal h3{margin:0 0 8px;font-family:var(--font-headline);font-size:18px;font-weight:700}
.modal-actions{display:flex;gap:var(--sp-2);margin-top:var(--sp-3)}.modal-actions .btn{flex:1}
.dock{position:fixed;left:0;right:0;bottom:0;padding:12px 14px calc(12px + env(safe-area-inset-bottom));background:rgba(9,9,11,.92);backdrop-filter:blur(10px);border-top:1px solid var(--line)}
.dock .inner{max-width:1240px;margin:0 auto}.dock .btn{width:100%}
.toast{position:fixed;left:50%;bottom:80px;transform:translateX(-50%) translateY(12px);opacity:0;padding:10px 16px;border-radius:var(--r-full);background:var(--accent-dim);color:var(--green-soft);font-family:var(--font-label);font-size:12px;font-weight:700;letter-spacing:.04em;text-transform:uppercase;transition:opacity .2s,transform .2s;pointer-events:none;z-index:60;max-width:90vw}
.toast.show{opacity:1;transform:translateX(-50%) translateY(0)}.toast.err{background:#3f1515;color:#fecaca}
code{font-family:var(--font-mono);font-size:13px;color:var(--green-soft)}
</style></head><body>
<div class="wrap">
<div class="speed-line"></div>
<header>
  <div><h1>Cardputer Keyboard</h1><p class="sub">Suzuka · remap console</p></div>
  <div class="chips">
    <div class="chip"><span class="dot" id="wifiDot"></span>IP <b id="ip">…</b></div>
    <div class="chip"><span class="dot" id="bleDot"></span>BLE <b id="ble">…</b></div>
  </div>
</header>
<nav class="tabs" id="tabNav">
  <button type="button" data-tab="keys" id="tabKeysBtn" class="on">键盘</button>
  <button type="button" data-tab="ota">OTA</button>
</nav>
<section class="panel on" id="tab-keys">
  <div class="card">
    <h2>平台</h2>
    <p class="hint" style="margin-top:0">切换编辑 Mac / Windows 映射（一起保存）。带 <b>ADV</b> 标记的是设备当前平台（蓝牙探测或 Opt+P）。</p>
    <div class="seg" id="platSeg"><button type="button" data-p="0">Mac</button><button type="button" data-p="1">Windows</button></div>
  </div>
  <div class="card">
    <h2>选择电脑端要发送的键</h2>
    <p class="hint" style="margin-top:0">电脑端为 <b>108 键全尺寸</b>（主区 + 导航区 + 小键盘）。无控制键时只能选 1 个主键；有控制键时可多选。笔记本 <b>Fn</b> 不在 HID 里，请直接点 F / 媒体键。<br>小键盘点 <b>Num</b> 切换数字/导航层；先点 <b>Shift</b> 再点 Num、或 <b>双击 Num</b>，可选中 NumLk。</p>
    <div class="combo-chips" id="pcChips"></div>
    <div class="preview">
      <div style="flex:1;min-width:12rem"><div class="hint" style="margin:0">电脑将收到</div><div class="live" id="pcLive">（未选择）</div></div>
      <div class="actions">
        <button type="button" class="btn btn-surface" id="pcClear" style="min-height:40px">清空</button>
        <button type="button" class="btn btn-primary" id="nextAdv" style="min-height:40px" disabled>下一步：选 ADV</button>
      </div>
    </div>
    <div class="kb-shell">
      <div class="kb-sec media" id="pcMedia"></div>
      <div id="pcKb"></div>
    </div>
  </div>
  <div class="card">
    <h2>映射表</h2>
    <div class="row grow" style="margin-bottom:10px">
      <button type="button" class="btn btn-surface" id="clearAllBtn">清空当前平台映射</button>
    </div>
    <div class="tbl-wrap">
      <table class="map">
        <thead><tr><th>#</th><th>ADV 触发</th><th class="arrow"></th><th>电脑发送</th><th></th></tr></thead>
        <tbody id="mapBody"><tr><td colspan="5" class="hint" style="text-align:center">暂无映射</td></tr></tbody>
      </table>
    </div>
  </div>
</section>

<section class="panel" id="tab-ota">
  <div class="card">
    <h2>固件 OTA</h2>
    <p class="hint" style="margin-top:0">上传 <code>firmware.bin</code></p>
    <div class="file-pick">
      <input type="file" id="otaFile" accept=".bin">
      <button type="button" class="btn btn-surface" id="otaPickBtn">选择文件</button>
      <span class="file-name" id="otaFileName">未选择文件</span>
    </div>
    <button type="button" class="btn btn-primary full" id="otaBtn">开始升级</button>
    <p class="hint" id="otaMsg"></p>
  </div>
</section>
</div>


<div class="mask" id="advMask">
  <div class="modal" onclick="event.stopPropagation()">
    <h3>选择 ADV 上的触发组合</h3>
    <p class="hint" style="margin-top:0">电脑端：<b id="modalPc">—</b><br>同样规则：无控制键只能 1 键；有控制键不限个数。</p>
    <div class="combo-chips" id="advChips"></div>
    <div class="preview" style="margin-top:8px">
      <div><div class="hint" style="margin:0">ADV 触发</div><div class="live" id="advLive">（未选择）</div></div>
      <button type="button" class="btn btn-surface" id="advClear" style="min-height:40px">清空</button>
    </div>
    <div class="ckb" id="advKb"></div>
    <div class="modal-actions">
      <button type="button" class="btn btn-surface" id="modalClose">取消</button>
      <button type="button" class="btn btn-primary" id="addMap" disabled>加入映射表</button>
    </div>
  </div>
</div>

<div class="dock"><div class="inner"><button type="button" class="btn btn-primary" id="saveBtn">保存并应用</button></div></div>
<div class="toast" id="toast"></div>
<script>
const $=id=>document.getElementById(id);
let platform=0, advPlatform=0, remapsMac=[], remapsWin=[];
let pc={mods:[],keys:[]}, adv={mods:[],keys:[]};
let numLockOn=true; // 小键盘两种状态：ON=数字 / OFF=导航
const curRemaps=()=>platform?remapsWin:remapsMac;
const mapRow=r=>({adv:{mods:r.adv?.mods||[],keys:r.adv?.keys||[]},pc:{mods:r.pc?.mods||[],keys:r.pc?.keys||[]}});
const PC_MODS=new Set(['ctrl','shift','alt','gui','ralt','rctrl','rshift','rgui']);
const ADV_MODS=new Set(['ctrl','opt','alt','fn','shift']);
const MOD_ORDER_PC=['ctrl','alt','gui','shift','ralt','rctrl','rshift','rgui'];
const MOD_ORDER_ADV=['fn','ctrl','opt','alt','shift'];
const LAB_PC={ctrl:{0:'⌃',1:'Ctrl'},shift:{0:'⇧',1:'Shift'},alt:{0:'⌥',1:'Alt'},gui:{0:'⌘',1:'Win'},ralt:{0:'⌥R',1:'AltGr'},rctrl:{0:'⌃R',1:'RCtrl'},rshift:{0:'⇧R',1:'RShift'},rgui:{0:'⌘R',1:'RWin'}};
const LAB_ADV={fn:'fn',ctrl:'ctrl',opt:'opt',alt:'alt',shift:'Aa'};
const KEY_LAB={space:'Space',enter:'Enter',tab:'Tab',esc:'Esc',bksp:'⌫',del:'Del',up:'↑',down:'↓',left:'←',right:'→',
  f1:'F1',f2:'F2',f3:'F3',f4:'F4',f5:'F5',f6:'F6',f7:'F7',f8:'F8',f9:'F9',f10:'F10',f11:'F11',f12:'F12',
  caps:'Caps',ins:'Ins',home:'Home',end:'End',pgup:'PgUp',pgdn:'PgDn',prtsc:'PrtSc',scrlk:'ScrLk',pause:'Pause',numlk:'Num',menu:'Menu',
  num0:'0',num1:'1',num2:'2',num3:'3',num4:'4',num5:'5',num6:'6',num7:'7',num8:'8',num9:'9',
  num_slash:'/',num_asterisk:'*',num_minus:'-',num_plus:'+',num_enter:'Enter',num_period:'.',
  vol_up:'Vol+',vol_down:'Vol-',mute:'Mute',play:'Play',next:'Next',prev:'Prev',stop:'Stop',
  www_home:'Web',calc:'Calc',search:'Search',back:'Back',email:'Mail'};
const PC_MEDIA=[
  {k:'vol_down',l:'Vol-'},{k:'vol_up',l:'Vol+'},{k:'mute',l:'Mute'},
  {k:'prev',l:'⏮'},{k:'play',l:'⏯'},{k:'next',l:'⏭'},{k:'stop',l:'⏹'},
  {k:'www_home',l:'Web'},{k:'search',l:'Search'},{k:'back',l:'Back'},{k:'calc',l:'Calc'},{k:'email',l:'Mail'}
];

function empty(){return {mods:[],keys:[]}}
function clone(c){return {mods:[...c.mods],keys:[...c.keys]}}
function toast(m,err){const t=$('toast');t.textContent=m;t.className='toast show'+(err?' err':'');setTimeout(()=>t.classList.remove('show'),2400)}
function labPcMod(id){const x=LAB_PC[id];return x?(platform?x[1]:x[0]):id}
function labKey(k,advSide){if(advSide&&LAB_ADV[k])return LAB_ADV[k];if(KEY_LAB[k])return KEY_LAB[k];return String(k).length===1?String(k).toUpperCase():k}
function parts(c,advSide){
  const order=advSide?MOD_ORDER_ADV:MOD_ORDER_PC;
  const out=[];
  order.forEach(id=>{if(c.mods.includes(id))out.push(advSide?labKey(id,true):labPcMod(id))});
  c.keys.forEach(k=>out.push(labKey(k,advSide)));
  return out;
}
function text(c,advSide){const p=parts(c,advSide);return p.length?p.join(' + '):'—'}
function sig(c){return [...c.mods].sort().join(',')+'|'+[...c.keys].sort().join(',')}
function valid(c){if(c.mods.length===0)return c.keys.length===1;return c.mods.length>=1}
function canToggleKey(c,id,isMod){
  if(isMod)return true;
  if(c.mods.length===0){
    if(c.keys.includes(id))return true;
    return c.keys.length===0;
  }
  return c.keys.length<6||c.keys.includes(id);
}

function toggle(c,id,isMod){
  if(isMod){
    const i=c.mods.indexOf(id);
    if(i>=0){
      c.mods.splice(i,1);
      if(c.mods.length===0&&c.keys.length>1)c.keys=[c.keys[0]];
    }else if(c.mods.length<4)c.mods.push(id);
    return;
  }
  const i=c.keys.indexOf(id);
  if(i>=0){c.keys.splice(i,1);return}
  if(!canToggleKey(c,id,false)){toast('无控制键时只能选 1 个主键',1);return}
  c.keys.push(id);
}

const ADV=[
[{p:'esc',l:'esc'},{p:'1',l:'1'},{p:'2',l:'2'},{p:'3',l:'3'},{p:'4',l:'4'},{p:'5',l:'5'},{p:'6',l:'6'},{p:'7',l:'7'},{p:'8',l:'8'},{p:'9',l:'9'},{p:'0',l:'0'},{p:'-',l:'-'},{p:'=',l:'='},{p:'del',l:'del'}],
[{p:'tab',l:'tab'},{p:'q',l:'Q'},{p:'w',l:'W'},{p:'e',l:'E'},{p:'r',l:'R'},{p:'t',l:'T'},{p:'y',l:'Y'},{p:'u',l:'U'},{p:'i',l:'I'},{p:'o',l:'O'},{p:'p',l:'P'},{p:'[',l:'['},{p:']',l:']'},{p:'\\',l:'\\'}],
[{p:'fn',l:'fn'},{p:'shift',l:'Aa'},{p:'a',l:'A'},{p:'s',l:'S'},{p:'d',l:'D'},{p:'f',l:'F'},{p:'g',l:'G'},{p:'h',l:'H'},{p:'j',l:'J'},{p:'k',l:'K'},{p:'l',l:'L'},{p:';',l:'↑'},{p:"'",l:"'"},{p:'enter',l:'ok'}],
[{p:'ctrl',l:'ctrl'},{p:'opt',l:'opt'},{p:'alt',l:'alt'},{p:'z',l:'Z'},{p:'x',l:'X'},{p:'c',l:'C'},{p:'v',l:'V'},{p:'b',l:'B'},{p:'n',l:'N'},{p:'m',l:'M'},{p:',',l:'←'},{p:'.',l:'↓'},{p:'/',l:'→'},{p:'space',l:'_'}]
];

document.querySelectorAll('.tabs button').forEach(b=>b.onclick=()=>{
  if(b.classList.contains('hidden'))return;
  document.querySelectorAll('.tabs button').forEach(x=>x.classList.toggle('on',x===b));
  document.querySelectorAll('.panel').forEach(p=>p.classList.toggle('on',p.id==='tab-'+b.dataset.tab));
  syncDock();
});
function activeTab(){
  const b=document.querySelector('.tabs button.on');
  return b?b.dataset.tab:'keys';
}
function syncDock(){
  const dock=document.querySelector('.dock');
  const b=$('saveBtn');
  if(!dock||!b)return;
  const t=activeTab();
  if(t==='ota'){dock.style.display='none';return}
  dock.style.display='';
  b.textContent='保存并应用';
}
function showTab(name){
  const b=document.querySelector(`.tabs button[data-tab="${name}"]`);
  if(!b)return;
  b.click();
}
function renderPlat(){
  document.querySelectorAll('#platSeg button').forEach(b=>{
    const p=Number(b.dataset.p);
    const name=p?'Windows':'Mac';
    b.classList.toggle('on',p===platform);
    // ADV 当前平台打标记；默认选中也跟 ADV 走（见 applyStatus）
    b.textContent=p===advPlatform?(name+' · ADV'):name;
  });
}
document.querySelectorAll('#platSeg button').forEach(b=>b.onclick=()=>{
  platform=Number(b.dataset.p);
  renderPlat();renderPc();renderTable();
  toast(platform===advPlatform?('正在编辑 ADV 当前平台：'+(platform?'Windows':'Mac')):('正在编辑 '+(platform?'Windows':'Mac')+' 映射'));
});

function updatePc(){
  const p=parts(pc,false);
  $('pcLive').textContent=p.length?p.join(' + '):'（未选择）';
  $('pcChips').innerHTML=p.map(x=>`<span>${x}</span>`).join('')||'<span style="opacity:.45;border-color:var(--border);background:transparent;color:var(--muted)">点按键组成电脑端组合</span>';
  $('nextAdv').disabled=!valid(pc);
}
function bindPcKeys(root){
  if(!root)return;
  root.querySelectorAll('[data-mod]').forEach(el=>el.onclick=()=>{toggle(pc,el.dataset.mod,true);renderPc();updatePc()});
  root.querySelectorAll('[data-key]').forEach(el=>{
    const pick=()=>{toggle(pc,el.dataset.key,false);renderPc();updatePc()};
    el.onclick=e=>{
      const id=el.dataset.key;
      if(!id)return;
      // NumLk：单击切换数字/导航层；物理 Shift 或已点选屏上 Shift/RShift，或双击 → 选中 NumLk
      if(id==='numlk'&&el.getAttribute('data-num-toggle')==='1'){
        const shiftHeld=!!(e.shiftKey||pc.mods.includes('shift')||pc.mods.includes('rshift'));
        if(shiftHeld){toggle(pc,'numlk',false);renderPc();updatePc();return}
        numLockOn=!numLockOn;
        toast(numLockOn?'小键盘：数字层':'小键盘：导航层');
        renderPc();updatePc();
        return;
      }
      pick();
    };
    if(el.getAttribute('data-num-toggle')==='1'){
      el.ondblclick=e=>{
        e.preventDefault();
        e.stopPropagation();
        toggle(pc,'numlk',false);
        renderPc();updatePc();
      };
    }
  });
}
function btnHtml(k){
  if(k.gap)return `<span class="gap${k.u?' '+k.u:''}"></span>`;
  if(k.dead)return `<span class="k dead${k.u?' '+k.u:''}">${k.l||''}</span>`;
  const isMod=!!k.mod;
  const on=isMod?pc.mods.includes(k.k):pc.keys.includes(k.k);
  const dim=!isMod&&!on&&!canToggleKey(pc,k.k,false);
  const led=k.numLed?' num-led':'';
  const cls=['k',k.u||'',isMod?'mod':'',on?'on':'',dim?'dim':'',led].filter(Boolean).join(' ');
  const extra=k.numToggle?' data-num-toggle="1"':'';
  return `<button type="button" class="${cls}" data-${isMod?'mod':'key'}="${k.k}"${extra}>${k.l}</button>`;
}
function rowHtml(keys){return `<div class="kr">${keys.map(btnHtml).join('')}</div>`}

function renderPc(){
  const g=platform?1:0;
  const L=(k,l,u,mod)=>({k,l:l||KEY_LAB[k]||k,u,mod:!!mod});
  const G=(u)=>({gap:1,u:u||''});
  // 媒体条
  const mkMedia=(k,l)=>{
    const on=pc.keys.includes(k);
    const dim=!on&&!canToggleKey(pc,k,false);
    return `<button type="button" class="k${on?' on':''}${dim?' dim':''}" data-key="${k}">${l}</button>`;
  };
  $('pcMedia').innerHTML='<div class="hint">媒体 / 系统</div><div class="kr">'+PC_MEDIA.map(x=>mkMedia(x.k,x.l)).join('')+'</div>';
  bindPcKeys($('pcMedia'));

  // 108 键：主区 + 导航 + 小键盘
  const main=[
    [L('esc','Esc','u125'),G('u1'),L('f1','F1'),L('f2','F2'),L('f3','F3'),L('f4','F4'),G(),L('f5','F5'),L('f6','F6'),L('f7','F7'),L('f8','F8'),G(),L('f9','F9'),L('f10','F10'),L('f11','F11'),L('f12','F12')],
    [L('`','`'),L('1','1'),L('2','2'),L('3','3'),L('4','4'),L('5','5'),L('6','6'),L('7','7'),L('8','8'),L('9','9'),L('0','0'),L('-','-'),L('=','='),L('bksp','⌫','u2')],
    [L('tab','Tab','u15'),L('q','Q'),L('w','W'),L('e','E'),L('r','R'),L('t','T'),L('y','Y'),L('u','U'),L('i','I'),L('o','O'),L('p','P'),L('[','['),L(']',']'),L('\\','\\','u15')],
    [L('caps','Caps','u175'),L('a','A'),L('s','S'),L('d','D'),L('f','F'),L('g','G'),L('h','H'),L('j','J'),L('k','K'),L('l','L'),L(';',';'),L("'", "'"),L('enter','Enter','u225')],
    [L('shift','⇧','u225',1),L('z','Z'),L('x','X'),L('c','C'),L('v','V'),L('b','B'),L('n','N'),L('m','M'),L(',',','),L('.','.'),L('/', '/'),L('rshift','⇧','u275',1)],
    [L('ctrl',g?'Ctrl':'⌃','u125',1),L('gui',g?'Win':'⌘','u125',1),L('alt',g?'Alt':'⌥','u125',1),L('space','Space','u6'),L('ralt',g?'AltGr':'⌥','u125',1),L('rgui',g?'Win':'⌘','u125',1),L('menu','Menu','u125'),L('rctrl',g?'Ctrl':'⌃','u125',1)]
  ];
  const nav=[
    [L('prtsc','Prt'),L('scrlk','Scr'),L('pause','Pse')],
    [L('ins','Ins'),L('home','Home'),L('pgup','PgUp')],
    [L('del','Del'),L('end','End'),L('pgdn','PgDn')],
    [{gap:1}],
    [G(),L('up','↑'),G()],
    [L('left','←'),L('down','↓'),L('right','→')]
  ];
  // NumLk ON：数字层；OFF：导航层（与实体键盘一致）
  const numLkBtn=Object.assign(L('numlk',numLockOn?'Num*':'Num'),{numToggle:1,numLed:numLockOn});
  const numKeys=numLockOn?[
    numLkBtn,L('num_slash','/'),L('num_asterisk','*'),L('num_minus','-'),
    L('num7','7'),L('num8','8'),L('num9','9'),L('num_plus','+','span2r'),
    L('num4','4'),L('num5','5'),L('num6','6'),
    L('num1','1'),L('num2','2'),L('num3','3'),L('num_enter','Ent','span2r'),
    L('num0','0','span2c'),L('num_period','.')
  ]:[
    numLkBtn,L('num_slash','/'),L('num_asterisk','*'),L('num_minus','-'),
    L('home','Home'),L('up','↑'),L('pgup','PgUp'),L('num_plus','+','span2r'),
    L('left','←'),{dead:1,l:''},L('right','→'),
    L('end','End'),L('down','↓'),L('pgdn','PgDn'),L('num_enter','Ent','span2r'),
    L('ins','Ins','span2c'),L('del','Del')
  ];
  const numHtml=numKeys.map(btnHtml).join('');

  $('pcKb').innerHTML =
    `<div class="fullkb">`+
      `<div class="fullkb-main">${main.map(rowHtml).join('')}</div>`+
      `<div class="fullkb-nav">${nav.map(rowHtml).join('')}</div>`+
      `<div class="fullkb-num">${numHtml}</div>`+
    `</div>`;
  bindPcKeys($('pcKb'));
  updatePc();
}
$('pcClear').onclick=()=>{pc=empty();renderPc()};


function updateAdv(){
  const p=parts(adv,true);
  $('advLive').textContent=p.length?p.join(' + '):'（未选择）';
  $('advChips').innerHTML=p.map(x=>`<span>${x}</span>`).join('')||'<span style="opacity:.45;border-color:var(--border);background:transparent;color:var(--muted)">点 ADV 键组成触发组合</span>';
  $('addMap').disabled=!valid(adv);
}
function renderAdv(){
  $('advKb').innerHTML=ADV.map(row=>'<div class="cr">'+row.map(k=>{
    const isMod=ADV_MODS.has(k.p);
    const on=isMod?adv.mods.includes(k.p):adv.keys.includes(k.p);
    const dim=!isMod&&!on&&!canToggleKey(adv,k.p,false);
    return `<button type="button" class="ck${isMod?' mod':''}${on?' on':''}${dim?' dim':''}" data-${isMod?'mod':'key'}="${k.p}">${k.l}</button>`;
  }).join('')+'</div>').join('');
  $('advKb').querySelectorAll('[data-mod]').forEach(el=>el.onclick=()=>{toggle(adv,el.dataset.mod,true);renderAdv();updateAdv()});
  $('advKb').querySelectorAll('[data-key]').forEach(el=>el.onclick=()=>{toggle(adv,el.dataset.key,false);renderAdv();updateAdv()});
  updateAdv();
}
$('advClear').onclick=()=>{adv=empty();renderAdv()};

function openAdv(){
  if(!valid(pc)){toast('请先选好电脑端组合',1);return}
  adv=empty();
  $('modalPc').textContent=text(pc,false);
  renderAdv();
  $('advMask').classList.add('show');
}
function closeAdv(){$('advMask').classList.remove('show')}
$('nextAdv').onclick=openAdv;
$('modalClose').onclick=closeAdv;
$('advMask').onclick=e=>{if(e.target===$('advMask'))closeAdv()};

function addMapping(){
  if(!valid(pc)||!valid(adv)){toast('两侧组合不完整',1);return}
  const a=clone(adv), p=clone(pc);
  const list=curRemaps();
  if(list.some(r=>sig(r.adv)===sig(a))){toast('ADV 触发组合与已有映射完全相同',1);return}
  list.push({adv:a,pc:p});
  closeAdv();
  pc=empty();adv=empty();
  renderPc();renderTable();
  toast('已加入'+(platform?' Windows':' Mac')+'映射表（记得保存）');
}
$('addMap').onclick=addMapping;

function renderTable(){
  const list=curRemaps();
  const tag=platform?'Windows':'Mac';
  if(!list.length){
    $('mapBody').innerHTML=`<tr><td colspan="5" class="hint" style="text-align:center">${tag}：暂无映射 — 未映射键屏蔽</td></tr>`;
    return;
  }
  $('mapBody').innerHTML=list.map((r,i)=>`<tr>
    <td>${i+1}</td>
    <td><b>${text(r.adv,true)}</b></td>
    <td class="arrow">→</td>
    <td style="color:var(--accent)">${text(r.pc,false)}</td>
    <td><button type="button" class="btn btn-g del" data-i="${i}">删除</button></td>
  </tr>`).join('');
  $('mapBody').querySelectorAll('.del').forEach(el=>el.onclick=()=>{curRemaps().splice(Number(el.dataset.i),1);renderTable()});
}
$('clearAllBtn').onclick=()=>{const list=curRemaps();if(!list.length||confirm('清空当前平台映射？')){if(platform)remapsWin=[];else remapsMac=[];renderTable();toast('已清空当前平台（记得保存）')}};

function applyStatus(d,keepPlat){
  advPlatform=d.platform|0;
  if(!keepPlat)platform=advPlatform;
  renderPlat();
  // 已取消全键透传：始终仅映射键
  $('ip').textContent=d.ip||'—';$('ble').textContent=d.ble?'已连接':'未连接';
  const sta=!!d.sta;
  $('wifiDot').className='dot'+(sta?' on':'');$('bleDot').className='dot'+(d.ble?' on':'');
  remapsMac=(d.remaps_mac||[]).map(mapRow);
  remapsWin=(d.remaps_win||[]).map(mapRow);
  if(!d.remaps_mac&&d.remaps){const one=(d.remaps||[]).map(mapRow);if((d.platform|0)===1)remapsWin=one;else remapsMac=one}
  renderPc();renderTable();
}
async function loadStatus(keepPlat){const d=await(await fetch('/api/status')).json();applyStatus(d,!!keepPlat)}
$('saveBtn').onclick=async()=>{
  const b=$('saveBtn');b.disabled=1;b.textContent='保存中…';
  const body={platform,passthrough:false,remaps_mac:remapsMac,remaps_win:remapsWin};
  try{
    const j=await(await fetch('/api/save',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(body)})).json();
    toast(j.ok?(j.msg||'已保存'):(j.msg||'失败'),!j.ok);
    if(j.ip)$('ip').textContent=j.ip;
    if(j.ok){advPlatform=platform;renderPlat();loadStatus(true)}
  }catch(e){toast('保存失败',1)}
  b.disabled=0;syncDock();
};
syncDock();
// 轮询 ADV 平台（Opt+P / 自动探测），不打断当前正在编辑的另一套映射
setInterval(async()=>{
  try{
    const d=await(await fetch('/api/status',{cache:'no-store'})).json();
    const p=d.platform|0;
    if(p!==advPlatform){advPlatform=p;renderPlat()}
    if(d.ip)$('ip').textContent=d.ip;
    $('ble').textContent=d.ble?'已连接':'未连接';
    $('wifiDot').className='dot'+(d.sta?' on':'');
    $('bleDot').className='dot'+(d.ble?' on':'');
  }catch(e){}
},2500);
$('otaPickBtn').onclick=()=>$('otaFile').click();
$('otaFile').onchange=()=>{const f=$('otaFile').files[0];$('otaFileName').textContent=f?f.name:'未选择文件'};
$('otaBtn').onclick=async()=>{const f=$('otaFile').files[0];if(!f){toast('选 .bin',1);return}const b=$('otaBtn');b.disabled=1;b.textContent='上传中…';$('otaMsg').textContent='写入中…';
  try{const fd=new FormData();fd.append('firmware',f);const t=await(await fetch('/api/ota',{method:'POST',body:fd})).text();let j;try{j=JSON.parse(t)}catch(e){j={ok:0,msg:t}}
    if(j.ok){$('otaMsg').textContent='成功，重启中…';toast('OTA 成功')}else{$('otaMsg').textContent=j.msg||'失败';toast('OTA 失败',1)}}catch(e){toast('失败',1)}b.disabled=0;b.textContent='开始升级'};
loadStatus(0).catch(()=>toast('无法读取状态',1));
</script></body></html>
)HTML";

static void handleRoot() {
  server.sendHeader("Cache-Control", "no-store");
  server.send_P(200, "text/html; charset=utf-8", PAGE_HTML);
}

static void handleStatus() {
  auto appendRemaps = [&](String& json, uint8_t plat) {
    bool first = true;
    for (uint8_t i = 0; i < remapCount(plat); i++) {
      const RemapEntry* e = remapAt(plat, i);
      if (!e) continue;
      if (!first) json += ",";
      first = false;
      json += "{\"adv\":";
      appendComboJson(json, e->adv, true);
      json += ",\"pc\":";
      appendComboJson(json, e->pc, false);
      json += "}";
    }
  };

  String json = "{";
  json += "\"ip\":\"" + jsonEscape(String(ipStr)) + "\",";
  json += "\"sta\":" + String(WiFi.status() == WL_CONNECTED ? "true" : "false") + ",";
  json += "\"ble\":" + String(bleConn ? "true" : "false") + ",";
  json += "\"platform\":" + String(platform) + ",";
  json += "\"wifi_name\":\"" + jsonEscape(String(ssidShown)) + "\",";
  json += "\"passthrough\":" + String(remapsPassThrough() ? "true" : "false") + ",";
  json += "\"remaps_mac\":[";
  appendRemaps(json, 0);
  json += "],\"remaps_win\":[";
  appendRemaps(json, 1);
  json += "]}";
  server.send(200, "application/json", json);
}

static void handleSave() {
  String body = server.arg("plain");
  DynamicJsonDocument doc(12288);
  if (deserializeJson(doc, body)) {
    server.send(400, "application/json", "{\"ok\":false,\"msg\":\"bad json\"}");
    return;
  }
  uint8_t plat = (uint8_t)(doc["platform"] | 0);
  if (plat > 1) plat = 0;
  platform = plat;
  prefs.putUChar("plat", platform);
  remapsSetPassThrough(false);  // 产品仅保留「映射键」模式

  auto loadArr = [&](const char* field, uint8_t p) {
    remapsClearPlatform(p);
    JsonArray arr = doc[field].as<JsonArray>();
    if (arr.isNull()) return;
    for (JsonObject o : arr) {
      KeyCombo adv{}, pc{};
      if (!parseComboJson(o["adv"].as<JsonObject>(), adv, true)) continue;
      if (!parseComboJson(o["pc"].as<JsonObject>(), pc, false)) continue;
      remapAdd(p, adv, pc);
    }
  };
  if (!doc["remaps_mac"].isNull() || !doc["remaps_win"].isNull()) {
    loadArr("remaps_mac", 0);
    loadArr("remaps_win", 1);
  } else if (!doc["remaps"].isNull()) {
    loadArr("remaps", platform);
  }
  remapsSaveBoth();
  dirty = true;
  server.send(200, "application/json",
              "{\"ok\":true,\"msg\":\"已保存 Mac+Win\",\"ip\":\"" + jsonEscape(String(ipStr)) + "\"}");
  Serial.println("[wifi] save remaps only");
}

static void handleOtaFinish() {
  if (otaOK) {
    server.send(200, "application/json", "{\"ok\":true,\"msg\":\"OTA ok, rebooting\"}");
    delay(500);
    ESP.restart();
  } else {
    server.send(500, "application/json", "{\"ok\":false,\"msg\":\"OTA failed\"}");
  }
}

static void handleOtaUpload() {
  HTTPUpload& upload = server.upload();
  if (upload.status == UPLOAD_FILE_START) {
    otaOK = false;
    Serial.printf("[ota] start %s\n", upload.filename.c_str());
    if (!Update.begin(UPDATE_SIZE_UNKNOWN)) Update.printError(Serial);
  } else if (upload.status == UPLOAD_FILE_WRITE) {
    if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) Update.printError(Serial);
  } else if (upload.status == UPLOAD_FILE_END) {
    if (Update.end(true)) {
      Serial.printf("[ota] success %u bytes\n", upload.totalSize);
      otaOK = true;
    } else {
      Update.printError(Serial);
      otaOK = false;
    }
  } else if (upload.status == UPLOAD_FILE_ABORTED) {
    Update.abort();
    otaOK = false;
  }
}

static void startSta(const String& ssid, const String& pass) {
  WiFi.softAPdisconnect(true);
  WiFi.disconnect(true, false);
  delay(40);
  WiFi.mode(WIFI_STA);
  delay(40);
  strncpy(ssidShown, ssid.c_str(), sizeof(ssidShown) - 1);
  ssidShown[sizeof(ssidShown) - 1] = 0;
  WiFi.begin(ssid.c_str(), pass.c_str());
  staTrying = true;
  lastWifiTry = millis();
  snprintf(ipStr, sizeof(ipStr), "...");
  dirty = true;
}

int wifiScanNetworks(WifiScanHit* out, int maxOut) {
  if (!out || maxOut <= 0) return 0;
  WiFi.softAPdisconnect(true);
  if (WiFi.getMode() != WIFI_STA && WiFi.getMode() != WIFI_AP_STA) {
    WiFi.mode(WIFI_STA);
    delay(80);
  }
  if (WiFi.status() != WL_CONNECTED) {
    WiFi.disconnect(false);
    delay(50);
  }
  WiFi.scanDelete();
  esp_coex_preference_set(ESP_COEX_PREFER_WIFI);
  int n = WiFi.scanNetworks(/*async=*/false, /*hidden=*/false);
  if (n == WIFI_SCAN_FAILED) {
    delay(200);
    n = WiFi.scanNetworks(false, false);
  }
  int count = 0;
  if (n > 0) {
    for (int i = 0; i < n && count < maxOut; i++) {
      String name = WiFi.SSID(i);
      if (!name.length()) continue;
      // de-dupe by SSID keep strongest
      int found = -1;
      for (int j = 0; j < count; j++) {
        if (name.equals(out[j].ssid)) { found = j; break; }
      }
      int32_t rssi = WiFi.RSSI(i);
      if (found >= 0) {
        if (rssi > out[found].rssi) out[found].rssi = rssi;
      } else {
        strncpy(out[count].ssid, name.c_str(), 32);
        out[count].ssid[32] = 0;
        out[count].rssi = rssi;
        count++;
      }
    }
    // sort by rssi desc
    for (int i = 0; i + 1 < count; i++) {
      for (int j = i + 1; j < count; j++) {
        if (out[j].rssi > out[i].rssi) {
          WifiScanHit t = out[i];
          out[i] = out[j];
          out[j] = t;
        }
      }
    }
    WiFi.scanDelete();
  }
  esp_coex_preference_set(ESP_COEX_PREFER_BALANCE);
  Serial.printf("[wifi] device scan -> %d unique (raw=%d)\n", count, n);
  return count;
}

void wifiWebDiagScan() {
  WifiScanHit hits[24];
  int n = wifiScanNetworks(hits, 24);
  for (int i = 0; i < n; i++) {
    Serial.printf("[wifi][diag]  %2d  rssi=%4d  %s\n", i, (int)hits[i].rssi, hits[i].ssid);
  }
}

void wifiWebBegin() {
  remapsLoad();
  remapsSetPassThrough(false);  // 取消全键透传
  WiFi.persistent(false);
  WiFi.softAPdisconnect(true);
  WiFi.disconnect(true, true);
  delay(60);
  WiFi.mode(WIFI_STA);

  wifi_country_t country = {};
  strncpy(country.cc, "CN", sizeof(country.cc));
  country.schan = 1;
  country.nchan = 13;
  country.max_tx_power = 84;
  country.policy = WIFI_COUNTRY_POLICY_MANUAL;
  esp_wifi_set_country(&country);

  String ssid = prefs.getString("wifi_ssid", "");
  String pass = prefs.getString("wifi_pass", "");
  if (ssid.length()) {
    startSta(ssid, pass);
    Serial.printf("[wifi] STA try saved '%s'\n", ssid.c_str());
  } else {
    snprintf(ipStr, sizeof(ipStr), "—");
    ssidShown[0] = 0;
    Serial.println("[wifi] no saved SSID — use Opt+W on device");
  }

  server.on("/", handleRoot);
  server.on("/api/status", handleStatus);
  server.on("/api/save", HTTP_POST, handleSave);
  server.on("/api/ota", HTTP_POST, handleOtaFinish, handleOtaUpload);
  server.begin();
  wifiStarted = true;
  lastWifiTry = millis();
  Serial.println("[wifi] web+ota on :80 (STA only, no SoftAP)");
}

void wifiWebLoop() {
  if (!wifiStarted) return;
  server.handleClient();

  if (WiFi.status() == WL_CONNECTED) {
    String ip = WiFi.localIP().toString();
    if (strcmp(ipStr, ip.c_str()) != 0) {
      snprintf(ipStr, sizeof(ipStr), "%s", ip.c_str());
      dirty = true;
      Serial.printf("[wifi] STA up %s\n", ipStr);
    }
    staTrying = false;
  } else if (staTrying && millis() - lastWifiTry > 20000) {
    staTrying = false;
    snprintf(ipStr, sizeof(ipStr), "fail");
    dirty = true;
    Serial.println("[wifi] STA fail — Opt+W to reconfigure");
  }
}

void wifiWebResetNetwork() {
  prefs.remove("wifi_ssid");
  prefs.remove("wifi_pass");
  WiFi.softAPdisconnect(true);
  WiFi.disconnect(true, true);
  delay(40);
  WiFi.mode(WIFI_STA);
  staTrying = false;
  ssidShown[0] = 0;
  snprintf(ipStr, sizeof(ipStr), "—");
  dirty = true;
  Serial.println("[wifi] creds cleared (no SoftAP)");
}

static char stashSsid[33] = "";
static char stashPass[65] = "";
static bool stashValid = false;

void wifiWebStashCurrent() {
  String s = prefs.getString("wifi_ssid", "");
  String p = prefs.getString("wifi_pass", "");
  if (s.length()) {
    strncpy(stashSsid, s.c_str(), sizeof(stashSsid) - 1);
    stashSsid[sizeof(stashSsid) - 1] = 0;
    strncpy(stashPass, p.c_str(), sizeof(stashPass) - 1);
    stashPass[sizeof(stashPass) - 1] = 0;
    stashValid = true;
    Serial.printf("[wifi] stash '%s' (keep link)\n", stashSsid);
  } else {
    stashValid = false;
    stashSsid[0] = 0;
    stashPass[0] = 0;
    Serial.println("[wifi] stash empty");
  }
}

bool wifiWebHasStash() { return stashValid; }
const char* wifiWebStashSsid() { return stashSsid; }

void wifiWebClearStash() {
  stashValid = false;
  stashSsid[0] = 0;
  stashPass[0] = 0;
}

bool wifiWebRestoreStash() {
  if (stashValid) {
    prefs.putString("wifi_ssid", stashSsid);
    prefs.putString("wifi_pass", stashPass);
    startSta(String(stashSsid), String(stashPass));
    Serial.printf("[wifi] restore stash '%s'\n", stashSsid);
    return true;
  }
  // 无旧网：清掉失败尝试留下的凭据
  wifiWebResetNetwork();
  Serial.println("[wifi] restore: no stash, cleared");
  return false;
}

bool wifiWebConnectHome(const char* ssid, const char* pass) {
  if (!ssid || !ssid[0]) return false;
  prefs.putString("wifi_ssid", ssid);
  prefs.putString("wifi_pass", pass ? pass : "");
  startSta(String(ssid), String(pass ? pass : ""));
  Serial.printf("[wifi] STA try '%s'\n", ssid);
  return true;
}

bool wifiWebConnected() { return WiFi.status() == WL_CONNECTED; }
const char* wifiWebIpString() { return ipStr; }
const char* wifiWebSsid() { return ssidShown; }
