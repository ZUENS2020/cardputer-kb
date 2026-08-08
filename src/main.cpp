// Cardputer ADV -> 全键盘 BLE HID + 网页组合键重映射 + WiFi/OTA
//
// 重映射：ADV 组合 → 电脑组合（网页配置）。默认无预设。
// 未映射键：可选透传或屏蔽。
// 本地保留：Opt+P 切平台 / Opt+R 重配对 / Opt+W 设备配网 / Aa+` 回 Launcher
//
// 配网：本机键盘扫网+输密码（无 SoftAP）。网页仅键盘映射 / OTA（需已连 WiFi）。

#include <M5Cardputer.h>
#undef KEY_LEFT_CTRL
#undef KEY_LEFT_SHIFT
#undef KEY_LEFT_ALT
#undef KEY_BACKSPACE
#undef KEY_TAB
#include <BleKeyboard.h>
#include <NimBLEDevice.h>
#include <Preferences.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>

#include "bindings.h"
#include "wifi_web.h"

BleKeyboard bleKeyboard("Cardputer KB", "M5Stack", 100);
Preferences prefs;

uint8_t platform = 0;

bool bleConn = false;
int batLevel = -1;
float battEma = -1.0f;
bool charging = false;
bool dirty = true;
uint32_t repairMsgUntil = 0;
NimBLECharacteristic* battChar = nullptr;

M5Canvas cv(&M5Cardputer.Display);

String cmdBuf;
uint32_t keymapUntil = 0;

// —— 设备端 WiFi 配网 UI ——
enum class WifiUi : uint8_t { Off, Scanning, List, Pass, Connecting, Result };
static WifiUi wifiUi = WifiUi::Off;
static constexpr int WIFI_HIT_MAX = 16;
static WifiScanHit wifiHits[WIFI_HIT_MAX];
static int wifiHitCount = 0;
static int wifiSel = 0;
static int wifiListTop = 0;
static char wifiPassBuf[65] = "";
static char wifiPickSsid[33] = "";
static char wifiStatusLine[40] = "";
static char wifiStatusSub[48] = "";
static bool wifiNeedScan = false;
static bool wifiTriedConnect = false;
static bool wifiResultOk = false;
static uint32_t wifiConnDeadline = 0;
static bool wPrevUp = false, wPrevDown = false, wPrevEnter = false, wPrevEsc = false, wPrevDel = false,
            wPrevSpace = false;
static bool wPrevCh[128] = {};

// 透传按键状态
static bool prevCtrl = false, prevAlt = false, prevOpt = false;
static bool prevTab = false, prevEnter = false, prevSpace = false, prevDel = false;
static bool prevFn = false;
static bool prevEsc = false;
static bool prevShiftBacktick = false;
static bool prevOptP = false, prevOptR = false, prevOptW = false;
static bool heldAscii[128] = {};
static bool remapHeld[MAX_REMAPS] = {};
static KeyCombo remapHeldPc[MAX_REMAPS] = {};

static void pressComboPc(const KeyCombo& h) {
  if (!bleKeyboard.isConnected()) return;
  for (uint8_t i = 0; i < h.nmod; i++) {
    bleKeyboard.press(h.mods[i]);
  }
  for (uint8_t i = 0; i < h.nkey; i++) {
    uint8_t k = h.keys[i];
    if (hidIsMedia(k)) {
      uint8_t m[2] = {0, 0};
      if (hidMediaReportBytes(k, m)) bleKeyboard.press(m);
    } else {
      bleKeyboard.press(k);
    }
  }
}

static void releaseComboPc(const KeyCombo& h) {
  if (!bleKeyboard.isConnected()) return;
  for (int i = (int)h.nkey - 1; i >= 0; i--) {
    uint8_t k = h.keys[i];
    if (hidIsMedia(k)) {
      uint8_t m[2] = {0, 0};
      if (hidMediaReportBytes(k, m)) bleKeyboard.release(m);
    } else {
      bleKeyboard.release(k);
    }
  }
  for (int i = (int)h.nmod - 1; i >= 0; i--) {
    bleKeyboard.release(h.mods[i]);
  }
}

static void returnToLauncher() {
  auto l = esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_TEST, nullptr);
  if (!l) l = esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_FACTORY, nullptr);
  if (!l || l == esp_ota_get_running_partition()) return;
  esp_ota_set_boot_partition(l);
  delay(60);
  esp_restart();
}

static void doRePair() {
  NimBLEServer* s = NimBLEDevice::getServer();
  if (s) {
    for (uint16_t id : s->getPeerDevices()) s->disconnect(id);
  }
  delay(120);
  NimBLEDevice::deleteAllBonds();
  battChar = nullptr;
  NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
  if (adv && !adv->isAdvertising()) adv->start();
  bleConn = false;
  repairMsgUntil = millis() + 3000;
  dirty = true;
  Serial.println("[repair] bonds cleared, re-advertising");
}

static void updateBattery();

static void wifiUiEnter() {
  wifiWebStashCurrent();  // 暂存旧网，不立刻断开
  wifiTriedConnect = false;
  wifiResultOk = false;
  wifiUi = WifiUi::Scanning;
  wifiNeedScan = true;
  wifiHitCount = 0;
  wifiSel = 0;
  wifiListTop = 0;
  wifiPassBuf[0] = 0;
  wifiPickSsid[0] = 0;
  wifiStatusSub[0] = 0;
  snprintf(wifiStatusLine, sizeof(wifiStatusLine), "Scanning...");
  dirty = true;
}

static void wifiUiShowResult(bool ok, const char* title, const char* sub) {
  wifiUi = WifiUi::Result;
  wifiResultOk = ok;
  snprintf(wifiStatusLine, sizeof(wifiStatusLine), "%s", title ? title : "");
  snprintf(wifiStatusSub, sizeof(wifiStatusSub), "%s", sub ? sub : "");
  dirty = true;
}

static void wifiUiRollback(const char* title) {
  bool restored = wifiWebRestoreStash();
  if (restored) {
    char sub[48];
    snprintf(sub, sizeof(sub), "Restored %s", wifiWebStashSsid());
    wifiUiShowResult(false, title ? title : "Failed", sub);
  } else {
    wifiUiShowResult(false, title ? title : "Failed", "No prior WiFi");
  }
}

static void wifiUiExit() {
  wifiUi = WifiUi::Off;
  wifiNeedScan = false;
  wifiTriedConnect = false;
  wifiStatusSub[0] = 0;
  dirty = true;
}

static void wifiUiCancel() {
  // Esc：若已尝试连新网或当前已掉线，回退旧网
  if (wifiTriedConnect || !wifiWebConnected()) {
    if (wifiWebHasStash() || wifiTriedConnect) {
      wifiUiRollback("Cancelled");
      return;
    }
  }
  wifiUiExit();
}

static void drawWifiUI() {
  cv.fillSprite(TFT_BLACK);
  cv.setTextDatum(TL_DATUM);
  cv.setTextSize(1);
  cv.setTextColor(TFT_CYAN, TFT_BLACK);
  cv.drawString("WiFi setup", 6, 4);
  cv.setTextColor(0x8410, TFT_BLACK);
  cv.drawString("Esc=back  Ent=ok", 130, 4);
  cv.drawFastHLine(0, 16, 240, 0x4208);

  if (wifiUi == WifiUi::Scanning) {
    cv.setTextColor(TFT_WHITE, TFT_BLACK);
    cv.setTextSize(2);
    cv.drawString("Scanning...", 6, 50);
    cv.setTextSize(1);
    cv.setTextColor(0xAD55, TFT_BLACK);
    cv.drawString("2.4GHz only", 6, 90);
  } else if (wifiUi == WifiUi::List) {
    cv.setTextColor(0xAD55, TFT_BLACK);
    cv.drawString(";/. move  Enter select", 6, 20);
    const int visible = 5;
    if (wifiSel < wifiListTop) wifiListTop = wifiSel;
    if (wifiSel >= wifiListTop + visible) wifiListTop = wifiSel - visible + 1;
    for (int i = 0; i < visible; i++) {
      int idx = wifiListTop + i;
      if (idx >= wifiHitCount) break;
      int y = 34 + i * 18;
      bool on = (idx == wifiSel);
      if (on) cv.fillRect(0, y - 2, 240, 17, 0x1A2F);
      cv.setTextColor(on ? TFT_GREEN : TFT_WHITE, on ? 0x1A2F : TFT_BLACK);
      char line[40];
      snprintf(line, sizeof(line), "%d %s", (int)wifiHits[idx].rssi, wifiHits[idx].ssid);
      if (strlen(line) > 36) {
        line[33] = '.';
        line[34] = '.';
        line[35] = '.';
        line[36] = 0;
      }
      cv.drawString(line, 6, y);
    }
  } else if (wifiUi == WifiUi::Pass) {
    cv.setTextColor(TFT_WHITE, TFT_BLACK);
    cv.drawString(wifiPickSsid, 6, 22);
    cv.setTextColor(0xAD55, TFT_BLACK);
    cv.drawString("Password (empty=open):", 6, 40);
    cv.setTextColor(TFT_GREEN, TFT_BLACK);
    cv.setTextSize(2);
    char stars[65];
    size_t n = strlen(wifiPassBuf);
    for (size_t i = 0; i < n && i < 20; i++) stars[i] = '*';
    stars[n > 20 ? 20 : n] = 0;
    cv.drawString(n ? stars : "_", 6, 62);
    cv.setTextSize(1);
    cv.setTextColor(0x8410, TFT_BLACK);
    cv.drawString("type on keyboard", 6, 100);
  } else if (wifiUi == WifiUi::Connecting) {
    cv.setTextColor(TFT_WHITE, TFT_BLACK);
    cv.setTextSize(2);
    cv.drawString("Connecting...", 6, 44);
    cv.setTextSize(1);
    cv.setTextColor(TFT_CYAN, TFT_BLACK);
    cv.drawString(wifiPickSsid, 6, 80);
  } else if (wifiUi == WifiUi::Result) {
    cv.setTextColor(wifiResultOk ? TFT_GREEN : TFT_YELLOW, TFT_BLACK);
    cv.setTextSize(2);
    cv.drawString(wifiStatusLine, 6, 36);
    cv.setTextSize(1);
    cv.setTextColor(TFT_WHITE, TFT_BLACK);
    if (wifiStatusSub[0]) cv.drawString(wifiStatusSub, 6, 68);
    cv.setTextColor(0xAD55, TFT_BLACK);
    cv.drawString("Enter / Esc close", 6, 100);
  }
  cv.pushSprite(0, 0);
}

static void drawUI() {
  if (wifiUi != WifiUi::Off) {
    drawWifiUI();
    return;
  }

  cv.fillSprite(TFT_BLACK);
  cv.setTextDatum(TL_DATUM);
  cv.setTextSize(1);

  cv.setTextColor(bleConn ? TFT_GREEN : 0xAD55, TFT_BLACK);
  cv.drawString(bleConn ? "BLE OK" : "BLE ...", 6, 6);
  {
    char bat[12];
    if (batLevel < 0) snprintf(bat, sizeof(bat), "--%%");
    else snprintf(bat, sizeof(bat), "%s%d%%", charging ? "+" : "", batLevel);
    cv.setTextColor(charging ? TFT_GREEN : (batLevel >= 0 && batLevel <= 15) ? TFT_RED : TFT_WHITE, TFT_BLACK);
    cv.setTextDatum(TR_DATUM);
    cv.drawString(bat, 234, 6);
  }
  cv.setTextDatum(TL_DATUM);
  cv.drawFastHLine(0, 20, 240, 0x4208);

  cv.setTextColor(TFT_WHITE, TFT_BLACK);
  cv.setTextSize(2);
  cv.drawString(platform ? "Windows" : "Mac", 6, 36);
  cv.setTextSize(1);
  cv.setTextColor(0xAD55, TFT_BLACK);
  cv.drawString("Mode: mapped keys only", 6, 62);
  cv.setTextColor(TFT_CYAN, TFT_BLACK);
  cv.drawString(String("Remaps: ") + remapCount(platform), 6, 80);

  cv.drawFastHLine(0, 96, 240, 0x4208);
  if (wifiWebConnected()) {
    cv.setTextColor(TFT_GREEN, TFT_BLACK);
    cv.drawString(String("Web ") + wifiWebIpString(), 6, 102);
    cv.setTextColor(0x8410, TFT_BLACK);
    cv.drawString("Opt+P/R/W  Aa+` launcher", 6, 118);
  } else {
    cv.setTextColor(TFT_YELLOW, TFT_BLACK);
    const char* ss = wifiWebSsid();
    if (ss && ss[0]) cv.drawString(String("WiFi ") + ss + "...", 6, 102);
    else cv.drawString("WiFi off — Opt+W setup", 6, 102);
    cv.setTextColor(0x8410, TFT_BLACK);
    cv.drawString("keyboard to join 2.4G", 6, 118);
  }

  if (repairMsgUntil) {
    cv.fillRect(0, 40, 240, 50, 0x000F);
    cv.setTextColor(TFT_WHITE, 0x000F);
    cv.setTextSize(2);
    cv.drawString("RE-PAIR", 6, 52);
    cv.setTextSize(1);
    cv.drawString("remove device on host", 6, 76);
  }
  cv.pushSprite(0, 0);
}

static uint32_t platDetectAt = 0;
static uint8_t platDetectPass = 0;
static bool platManualOverride = false;

static void schedulePlatformDetect() {
  platManualOverride = false;
  platDetectPass = 0;
  platDetectAt = millis() + 450;  // 等主机订阅 Battery 等特征
}

static void runPlatformDetect() {
  if (platManualOverride || !bleConn || !platDetectAt) return;
  if (millis() < platDetectAt) return;
  if (platformApplyDetected(&platform)) {
    dirty = true;
    Serial.printf("[platform] auto -> %s\n", platform ? "Windows" : "Mac");
  }
  platDetectPass++;
  // 多次探测：Battery 订阅可能稍晚才到
  if (platDetectPass < 3) platDetectAt = millis() + 600;
  else platDetectAt = 0;
}

static void printStatus() {
  Serial.printf("[status] BLE=%s platform=%s remaps=%u wifi=%s ip=%s ssid=%s batt=%d%%\n",
                bleConn ? "connected" : "advertising",
                platform ? "Windows" : "Mac",
                remapCount(platform),
                wifiWebConnected() ? "up" : "down",
                wifiWebIpString(),
                wifiWebSsid(),
                batLevel);
}

static void handleCommand(String line) {
  line.trim();
  if (!line.length()) return;
  if (line == "status") printStatus();
  else if (line.startsWith("platform")) {
    String a = line.substring(8);
    a.trim();
    if (a == "mac") platform = 0;
    else if (a == "win") platform = 1;
    else platform ^= 1;
    prefs.putUChar("plat", platform);
    platManualOverride = true;
    platDetectAt = 0;
    dirty = true;
    Serial.printf("[platform] manual -> %s\n", platform ? "Windows" : "Mac");
  } else if (line == "keymap") {
    keymapUntil = millis() + 10000;
    Serial.println("[keymap] 10s...");
  } else if (line == "repair") {
    doRePair();
  } else if (line == "clear") {
    remapsClearPlatform(platform);
    remapsSavePlatform(platform);
    dirty = true;
    Serial.println("[clear] remaps cleared");
  } else if (line.startsWith("wifi")) {
    String rest = line.substring(4);
    rest.trim();
    if (!rest.length() || rest == "status") {
      Serial.printf("[wifi] mode=%s ip=%s ssid=%s\n",
                    wifiWebConnected() ? "STA" : "down",
                    wifiWebIpString(),
                    prefs.getString("wifi_ssid", "").c_str());
    } else if (rest == "clear" || rest == "reset") {
      wifiWebResetNetwork();
      Serial.println("[wifi] cleared — Opt+W on device to setup");
    } else if (rest == "scan") {
      wifiWebDiagScan();
    } else if (rest == "ui") {
      wifiUiEnter();
      Serial.println("[wifi] open device setup UI");
    } else {
      int sp = rest.indexOf(' ');
      String ssid = (sp < 0) ? rest : rest.substring(0, sp);
      String pass = (sp < 0) ? "" : rest.substring(sp + 1);
      ssid.trim();
      pass.trim();
      if (!ssid.length()) {
        Serial.println("[wifi] usage: wifi <ssid> [password] | wifi clear | wifi ui");
      } else if (wifiWebConnectHome(ssid.c_str(), pass.c_str())) {
        Serial.printf("[wifi] connecting '%s'...\n", ssid.c_str());
      }
    }
  } else {
    Serial.println("[?] status|platform|keymap|repair|clear|wifi");
  }
}

static void pollSerial() {
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\n' || c == '\r') {
      if (cmdBuf.length()) {
        handleCommand(cmdBuf);
        cmdBuf = "";
      }
    } else if (cmdBuf.length() < 64) {
      cmdBuf += c;
    }
  }
}

static bool hasChar(const std::vector<char>& w, char target) {
  for (char c : w)
    if (c == target) return true;
  return false;
}

static char normChar(char c) {
  if (c >= 'A' && c <= 'Z') return c - 'A' + 'a';
  return c;
}

static void handleWifiKeyboard() {
  auto ks = M5Cardputer.Keyboard.keysState();
  bool up = hasChar(ks.word, ';');
  bool down = hasChar(ks.word, '.');
  bool enter = ks.enter;
  bool esc = !ks.shift && (hasChar(ks.word, '`') || hasChar(ks.word, '~'));

  auto edge = [](bool now, bool& prev) {
    bool e = now && !prev;
    prev = now;
    return e;
  };

  if (wifiUi == WifiUi::List) {
    if (edge(up, wPrevUp) && wifiHitCount > 0) {
      wifiSel = (wifiSel + wifiHitCount - 1) % wifiHitCount;
      dirty = true;
    }
    if (edge(down, wPrevDown) && wifiHitCount > 0) {
      wifiSel = (wifiSel + 1) % wifiHitCount;
      dirty = true;
    }
    if (edge(enter, wPrevEnter) && wifiHitCount > 0) {
      strncpy(wifiPickSsid, wifiHits[wifiSel].ssid, sizeof(wifiPickSsid) - 1);
      wifiPickSsid[sizeof(wifiPickSsid) - 1] = 0;
      wifiPassBuf[0] = 0;
      wifiUi = WifiUi::Pass;
      dirty = true;
    }
    if (edge(esc, wPrevEsc)) wifiUiCancel();
    wPrevDel = ks.del;
  } else if (wifiUi == WifiUi::Pass) {
    if (edge(esc, wPrevEsc)) {
      wifiUi = WifiUi::List;
      dirty = true;
    } else if (edge(enter, wPrevEnter)) {
      wifiTriedConnect = true;
      wifiUi = WifiUi::Connecting;
      wifiStatusSub[0] = 0;
      snprintf(wifiStatusLine, sizeof(wifiStatusLine), "Connecting...");
      wifiConnDeadline = millis() + 18000;
      wifiWebConnectHome(wifiPickSsid, wifiPassBuf);
      dirty = true;
    } else if (edge(ks.del, wPrevDel)) {
      size_t n = strlen(wifiPassBuf);
      if (n) {
        wifiPassBuf[n - 1] = 0;
        dirty = true;
      }
    } else if (edge(ks.space, wPrevSpace)) {
      size_t n = strlen(wifiPassBuf);
      if (n + 1 < sizeof(wifiPassBuf)) {
        wifiPassBuf[n] = ' ';
        wifiPassBuf[n + 1] = 0;
        dirty = true;
      }
    } else {
      // 追加可打印字符（边沿）
      for (char c : ks.word) {
        if (c == ';' || c == '.' || c == '`' || c == '~') continue;
        uint8_t u = (uint8_t)c;
        if (u < 32 || u >= 127) continue;
        if (!wPrevCh[u]) {
          size_t n = strlen(wifiPassBuf);
          if (n + 1 < sizeof(wifiPassBuf)) {
            wifiPassBuf[n] = c;
            wifiPassBuf[n + 1] = 0;
            dirty = true;
          }
        }
        wPrevCh[u] = true;
      }
      for (int i = 32; i < 127; i++) {
        bool still = false;
        for (char c : ks.word)
          if ((uint8_t)c == i) {
            still = true;
            break;
          }
        if (!still) wPrevCh[i] = false;
      }
    }
    wPrevUp = up;
    wPrevDown = down;
  } else if (wifiUi == WifiUi::Result) {
    if (edge(enter, wPrevEnter) || edge(esc, wPrevEsc)) wifiUiExit();
    wPrevUp = up;
    wPrevDown = down;
    wPrevDel = ks.del;
  } else {
    // Scanning / Connecting：Esc 取消（连接中会回退）
    if (edge(esc, wPrevEsc)) {
      wifiUiCancel();
    }
    wPrevUp = up;
    wPrevDown = down;
    wPrevEnter = enter;
    wPrevDel = ks.del;
  }
}

// 宏 / 透传：组合触发优先；触发里用到的物理键不透传
static void handleKeyboard() {
  if (wifiUi != WifiUi::Off) {
    handleWifiKeyboard();
    return;
  }

  auto ks = M5Cardputer.Keyboard.keysState();

  if (keymapUntil && millis() < keymapUntil) {
    if (M5Cardputer.Keyboard.isChange() && M5Cardputer.Keyboard.isPressed()) {
      Serial.print("[keymap] word=[");
      for (char c : ks.word) Serial.printf("'%c'(%d) ", c, c);
      Serial.printf("] ctrl=%d alt=%d opt=%d fn=%d shift=%d tab=%d enter=%d space=%d del=%d\n",
                    ks.ctrl, ks.alt, ks.opt, ks.fn, ks.shift, ks.tab, ks.enter, ks.space, ks.del);
    }
  } else if (keymapUntil) {
    keymapUntil = 0;
  }

  bool backtick = hasChar(ks.word, '`') || hasChar(ks.word, '~');

  // —— 本地保留：Aa+` 回 Launcher ——
  if (ks.shift) {
    if (backtick && !prevShiftBacktick) returnToLauncher();
    prevShiftBacktick = backtick;
  } else {
    prevShiftBacktick = false;
  }

  // —— 本地保留：Opt+P 平台 / Opt+R 重配对 / Opt+W 设备配网 ——
  if (ks.opt) {
    bool oP = hasChar(ks.word, 'p') || hasChar(ks.word, 'P');
    if (oP && !prevOptP) {
      platform ^= 1;
      prefs.putUChar("plat", platform);
      platManualOverride = true;
      platDetectAt = 0;
      dirty = true;
      Serial.printf("[platform] manual -> %s\n", platform ? "Windows" : "Mac");
    }
    prevOptP = oP;

    bool oR = hasChar(ks.word, 'r') || hasChar(ks.word, 'R');
    if (oR && !prevOptR) doRePair();
    prevOptR = oR;

    bool oW = hasChar(ks.word, 'w') || hasChar(ks.word, 'W');
    if (oW && !prevOptW) {
      wifiUiEnter();  // 暂存旧网，不立刻断开
    }
    prevOptW = oW;
  } else {
    prevOptP = prevOptR = prevOptW = false;
  }

  AdvPress st{};
  st.ctrl = ks.ctrl;
  st.alt = ks.alt;
  st.opt = ks.opt;
  st.fn = ks.fn;
  st.shift = ks.shift;
  st.tab = ks.tab;
  st.enter = ks.enter;
  st.space = ks.space;
  st.del = ks.del;
  st.esc = !ks.shift && backtick;
  for (char c : ks.word) {
    char n = normChar(c);
    if (n == '`' || n == '~') continue;
    // 保留组合里的字母不参与映射/透传边沿
    if (ks.opt && (n == 'p' || n == 'r' || n == 'w')) continue;
    if ((uint8_t)n < 128) st.ascii[(uint8_t)n] = true;
  }

  // —— 组合映射：状态跟随 ADV（按下保持 / 松开释放，支持长按）——
  uint8_t nmap = remapCount(platform);
  for (uint8_t i = 0; i < nmap; i++) {
    const RemapEntry* r = remapAt(platform, i);
    if (!r) continue;
    bool down = comboMatchAdv(r->adv, st);
    if (down && !remapHeld[i]) {
      pressComboPc(r->pc);
      remapHeldPc[i] = r->pc;
      remapHeld[i] = true;
    } else if (!down && remapHeld[i]) {
      releaseComboPc(remapHeldPc[i]);
      remapHeld[i] = false;
    }
  }
  for (uint8_t i = nmap; i < MAX_REMAPS; i++) {
    if (remapHeld[i]) {
      releaseComboPc(remapHeldPc[i]);
      remapHeld[i] = false;
    }
  }

  auto reserved = [&](uint8_t phys) { return physUsedInAnyTrigger(platform, phys); };

  auto pressIf = [&](bool want, bool& held, uint8_t hid) {
    if (want && !held) {
      if (bleKeyboard.isConnected()) bleKeyboard.press(hid);
      held = true;
    } else if (!want && held) {
      if (bleKeyboard.isConnected()) bleKeyboard.release(hid);
      held = false;
    }
  };

  auto passOrRelease = [&](bool down, bool& held, uint8_t hid, uint8_t phys) {
    if (reserved(phys)) {
      if (held) {
        if (bleKeyboard.isConnected()) bleKeyboard.release(hid);
        held = false;
      }
      return;
    }
    if (remapsPassThrough()) pressIf(down, held, hid);
    else if (held) {
      if (bleKeyboard.isConnected()) bleKeyboard.release(hid);
      held = false;
    }
  };

  static bool heldShift = false, heldCtrl = false, heldAlt = false, heldOpt = false;
  static bool heldTab = false, heldEnter = false, heldSpace = false, heldDel = false, heldEsc = false;

  // Aa：仅透传模式且未占用时发给电脑
  if (remapsPassThrough() && !reserved(PK_SHIFT)) pressIf(ks.shift, heldShift, KEY_LEFT_SHIFT);
  else if (heldShift) {
    if (bleKeyboard.isConnected()) bleKeyboard.release(KEY_LEFT_SHIFT);
    heldShift = false;
  }

  passOrRelease(ks.ctrl, heldCtrl, KEY_LEFT_CTRL, PK_CTRL);
  passOrRelease(ks.alt, heldAlt, KEY_LEFT_ALT, PK_ALT);
  passOrRelease(ks.opt, heldOpt, KEY_LEFT_GUI, PK_OPT);
  passOrRelease(ks.tab, heldTab, KEY_TAB, PK_TAB);
  passOrRelease(ks.enter, heldEnter, KEY_RETURN, PK_ENTER);
  passOrRelease(ks.space, heldSpace, ' ', PK_SPACE);
  passOrRelease(ks.del, heldDel, KEY_BACKSPACE, PK_DEL);
  passOrRelease(st.esc, heldEsc, KEY_ESC, PK_ESC);

  prevFn = ks.fn;
  prevCtrl = ks.ctrl;
  prevAlt = ks.alt;
  prevOpt = ks.opt;
  prevTab = ks.tab;
  prevEnter = ks.enter;
  prevSpace = ks.space;
  prevDel = ks.del;
  prevEsc = st.esc;

  for (int i = 1; i < 128; i++) {
    bool down = st.ascii[i];
    if (!down && !heldAscii[i]) continue;
    if (reserved((uint8_t)i)) {
      if (heldAscii[i]) {
        if (bleKeyboard.isConnected()) bleKeyboard.release((uint8_t)i);
        heldAscii[i] = false;
      }
      continue;
    }
    if (remapsPassThrough()) pressIf(down, heldAscii[i], (uint8_t)i);
    else if (heldAscii[i]) {
      if (bleKeyboard.isConnected()) bleKeyboard.release((uint8_t)i);
      heldAscii[i] = false;
    }
  }
}

void setup() {
  auto cfg = M5.config();
  M5Cardputer.begin(cfg, true);
  M5Cardputer.Display.setRotation(1);
  cv.setColorDepth(8);
  cv.createSprite(240, 135);

  prefs.begin("typeless", false);
  platform = prefs.getUChar("plat", 0) ? 1 : 0;

  Serial.begin(115200);
  Serial.println("\n[boot] Cardputer full BLE keyboard + remap + OTA");

  remapsLoad();
  // ESP32-S3：必须先起 BLE，再起 WiFi，否则 coex_enable abort
  bleKeyboard.begin();
  delay(150);
  wifiWebBegin();
  updateBattery();
  if (!prefs.getString("wifi_ssid", "").length()) {
    wifiUiEnter();
  }
  drawUI();
}

static void pushBatteryBle(uint8_t level) {
  if (!battChar) {
    NimBLEServer* s = NimBLEDevice::getServer();
    if (!s) return;
    NimBLEService* bas = s->getServiceByUUID(NimBLEUUID((uint16_t)0x180f));
    if (!bas) return;
    battChar = bas->getCharacteristic(NimBLEUUID((uint16_t)0x2a19));
    if (!battChar) return;
  }
  battChar->setValue(&level, 1);
  battChar->notify();
}

static void updateBattery() {
  int raw = M5.Power.getBatteryLevel();
  if (raw >= 0 && raw <= 100) {
    if (battEma < 0) battEma = raw;
    else battEma = battEma * 0.8f + raw * 0.2f;
    int lv = (int)lroundf(battEma);
    if (lv != batLevel) {
      batLevel = lv;
      dirty = true;
    }
  }
  bool ch = ((int)M5.Power.isCharging() == 1);
  if (ch != charging) {
    charging = ch;
    dirty = true;
  }
  if (bleConn && batLevel >= 0) {
    bleKeyboard.setBatteryLevel((uint8_t)batLevel);
    pushBatteryBle((uint8_t)batLevel);
  }
}

void loop() {
  M5Cardputer.update();
  wifiWebLoop();
  pollSerial();

  if (wifiUi == WifiUi::Scanning && wifiNeedScan) {
    wifiNeedScan = false;
    drawUI();
    wifiHitCount = wifiScanNetworks(wifiHits, WIFI_HIT_MAX);
    wifiSel = 0;
    wifiListTop = 0;
    if (wifiHitCount > 0) {
      wifiUi = WifiUi::List;
      dirty = true;
    } else {
      // 扫不到网：不断开则保持旧网；若已掉线则回退
      if (!wifiWebConnected() && wifiWebHasStash()) {
        wifiUiRollback("Scan fail");
      } else {
        wifiUiShowResult(false, "Scan fail",
                         wifiWebConnected() ? "Kept current WiFi" : "No networks");
      }
    }
  }
  if (wifiUi == WifiUi::Connecting) {
    if (wifiWebConnected()) {
      wifiWebClearStash();
      char sub[48];
      snprintf(sub, sizeof(sub), "%s  %s", wifiPickSsid, wifiWebIpString());
      wifiUiShowResult(true, "Connected", sub);
    } else if (millis() > wifiConnDeadline) {
      wifiUiRollback("Connect fail");
    }
  }

  handleKeyboard();
  runPlatformDetect();

  static uint32_t tConn = 0, tBatt = 0;
  if (millis() - tConn > 500) {
    tConn = millis();
    bool c = bleKeyboard.isConnected();
    if (c != bleConn) {
      bleConn = c;
      dirty = true;
      if (c) {
        schedulePlatformDetect();
      } else {
        platDetectAt = 0;
        platManualOverride = false;
        memset(heldAscii, 0, sizeof(heldAscii));
        memset(remapHeld, 0, sizeof(remapHeld));
      }
    }
  }
  if (millis() - tBatt > 2000) {
    tBatt = millis();
    updateBattery();
  }
  if (repairMsgUntil && millis() > repairMsgUntil) {
    repairMsgUntil = 0;
    dirty = true;
  }
  if (dirty) {
    drawUI();
    dirty = false;
  }
  delay(6);
}
