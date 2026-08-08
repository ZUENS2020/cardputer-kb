// Cardputer ADV -> 全键盘 BLE HID + 网页组合键重映射 + WiFi/OTA
//
// 重映射：ADV 组合 → 电脑组合（网页配置）。默认无预设。
// 未映射键：可选透传或屏蔽。
// 本地保留：Opt+P 切平台 / Opt+R 重配对 / Opt+W 重置 WiFi+AP / Aa+` 回 Launcher
//
// 网页：http://<IP>  → WiFi / 键盘重映射 / OTA

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

BleKeyboard bleKeyboard("Cardputer Voice", "M5Stack", 100);
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

// 透传按键状态
static bool prevCtrl = false, prevAlt = false, prevOpt = false;
static bool prevTab = false, prevEnter = false, prevSpace = false, prevDel = false;
static bool prevFn = false;
static bool prevEsc = false;
static bool prevShiftBacktick = false;
static bool prevOptP = false, prevOptR = false, prevOptW = false;
static bool heldAscii[128] = {};
static bool remapLatch[MAX_REMAPS] = {};

static void sendComboPc(const KeyCombo& h) {
  if (!bleKeyboard.isConnected()) return;
  for (uint8_t i = 0; i < h.nmod; i++) {
    bleKeyboard.press(h.mods[i]);
    delay(10);
  }
  for (uint8_t i = 0; i < h.nkey; i++) {
    bleKeyboard.press(h.keys[i]);
    delay(10);
  }
  delay(35);
  bleKeyboard.releaseAll();
  delay(10);
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

static void drawUI() {
  cv.fillSprite(TFT_BLACK);
  cv.setTextDatum(TL_DATUM);
  cv.setTextSize(1);

  // 顶栏（不用橙色，避免被当成告警）
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
  cv.drawString(remapsPassThrough() ? "Mode: full pass-through" : "Mode: mapped keys only", 6, 62);
  cv.setTextColor(TFT_CYAN, TFT_BLACK);
  cv.drawString(String("Remaps: ") + remapCount(platform), 6, 80);

  cv.drawFastHLine(0, 96, 240, 0x4208);
  if (wifiWebConnected()) {
    cv.setTextColor(TFT_GREEN, TFT_BLACK);
    cv.drawString(String("Web ") + wifiWebIpString(), 6, 102);
    cv.setTextColor(0x8410, TFT_BLACK);
    cv.drawString("Opt+P/R/W  Aa+` launcher", 6, 118);
  } else if (wifiWebApMode()) {
    cv.setTextColor(TFT_CYAN, TFT_BLACK);
    cv.drawString(String("AP ") + wifiWebApSsid(), 6, 102);
    cv.drawString(String("http://") + wifiWebIpString(), 6, 116);
  } else {
    cv.setTextColor(0xAD55, TFT_BLACK);
    cv.drawString("WiFi connecting...", 6, 102);
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
  Serial.printf("[status] BLE=%s platform=%s remaps=%u wifi=%s ip=%s batt=%d%%\n",
                bleConn ? "connected" : "advertising",
                platform ? "Windows" : "Mac",
                remapCount(platform),
                wifiWebConnected() ? "up" : (wifiWebApMode() ? "ap" : "down"),
                wifiWebIpString(), batLevel);
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
  } else {
    Serial.println("[?] status|platform|keymap|repair|clear");
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

// 宏 / 透传：组合触发优先；触发里用到的物理键不透传
static void handleKeyboard() {
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

  // —— 本地保留：Opt+P 平台 / Opt+R 重配对 / Opt+W 重置 WiFi+AP ——
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
      wifiWebResetNetwork();
      dirty = true;
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

  // —— 组合映射：边沿触发 ——
  uint8_t nmap = remapCount(platform);
  for (uint8_t i = 0; i < nmap; i++) {
    const RemapEntry* r = remapAt(platform, i);
    if (!r) continue;
    bool down = comboMatchAdv(r->adv, st);
    if (down && !remapLatch[i]) {
      sendComboPc(r->pc);
      remapLatch[i] = true;
    }
    if (!down) remapLatch[i] = false;
  }
  for (uint8_t i = nmap; i < MAX_REMAPS; i++) remapLatch[i] = false;

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
        memset(remapLatch, 0, sizeof(remapLatch));
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
