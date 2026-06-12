// Cardputer ADV -> Typeless 语音输入 + Claude Code 控制台 (BLE HID)
//
// 电脑把本机当蓝牙键盘。Cardputer 按键映射：
//   Ctrl        -> 发听写热键 (默认 Ctrl+Alt+\) = Typeless 听写开/关 (toggle)
//   Alt         -> 选择模式开关 (本地)。开时方向键变 Shift+方向
//   Opt         -> 发 Shift+Tab = 切换 Claude Code 模式
//   方向键      -> 转发 ↑↓←→ (选择模式下加 Shift)
//   Esc(` 键)   -> 转发 Esc (REC 状态下还会同步退出本地 REC 指示)
//   Space/Del   -> 转发 Space/Backspace
//   Enter 键    -> 发 Ctrl+Enter
//   Fn 单独点按 -> 发 Enter
//   Tab         -> Typeless 另一种输入 (和 Ctrl 同样处理: 单独按下/2s 间隔/带状态)
//                  平台专属组合 (Mac: Ctrl+Alt+'，Win: RightAlt+Space)
//   Aa(Shift) 层 (按住 Aa + 键):
//     Aa + P    -> 切换平台 Mac/Windows (存 NVS)
//     Aa + R    -> 重新配对 (清 bond + 重新广播)
//     Aa + `    -> 返回 bmorcelli Launcher
//
// 串口命令: status / platform [mac|win] / hotkey <n> / send / keymap / reset / repair
//
// 详见 README.md

#include <M5Cardputer.h>
// M5Cardputer 的 Keyboard_def.h 把这些名字定义成宏 (HID 内部码)，会和
// ESP32-BLE-Keyboard 的同名常量冲突。先 undef，让 BLE 库的常量生效。
#undef KEY_LEFT_CTRL
#undef KEY_LEFT_SHIFT
#undef KEY_LEFT_ALT
#undef KEY_BACKSPACE
#undef KEY_TAB
#include <BleKeyboard.h>
#include <NimBLEDevice.h>      // 直接拿电量特征做 notify (T-vK 只 setValue 不 notify)
#include <Preferences.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>

// ---- BLE HID ----
BleKeyboard bleKeyboard("Cardputer Voice", "M5Stack", 100);

// ---- 配置持久化 ----
Preferences prefs;

// ---- 听写热键预设：按平台分两套 (Mac / Windows) ----
// BLE HID 无法可靠探测主机系统，所以用一个记忆在 NVS 的平台开关 (Fn+P 切换)。
struct Hotkey {
  const char* name;
  uint8_t mods[2];
  uint8_t nmod;
  char    key;
};
static const Hotkey HK_MAC[] = {
  {"Ctrl+Alt+\\", {KEY_LEFT_CTRL, KEY_LEFT_ALT}, 2, '\\'},  // 默认 (Control+Option+Backslash)
  {"Ctrl+Alt+.",  {KEY_LEFT_CTRL, KEY_LEFT_ALT}, 2, '.'},   // (Control+Option+Period)
  {"Alt+Cmd+\\",  {KEY_LEFT_ALT,  KEY_LEFT_GUI}, 2, '\\'},  // (Option+Command+Backslash)
};
static const Hotkey HK_WIN[] = {
  {"RightAlt",     {KEY_RIGHT_ALT}, 1, 0},                     // 默认: 单独右 Alt (Typeless 绑定)
  {"Ctrl+Alt+\\",  {KEY_LEFT_CTRL, KEY_LEFT_ALT},   2, '\\'},  // 备选
  {"Ctrl+Shift+\\",{KEY_LEFT_CTRL, KEY_LEFT_SHIFT}, 2, '\\'},  // 避开部分键盘 AltGr(=Ctrl+Alt) 冲突
};
static const uint8_t HK_MAC_COUNT = sizeof(HK_MAC) / sizeof(HK_MAC[0]);
static const uint8_t HK_WIN_COUNT = sizeof(HK_WIN) / sizeof(HK_WIN[0]);

// Tab 键发的平台专属组合键
static const Hotkey TAB_MAC = {"Ctrl+Alt+'", {KEY_LEFT_CTRL, KEY_LEFT_ALT}, 2, '\''};  // Control+Option+'
static const Hotkey TAB_WIN = {"RAlt+Space",  {KEY_RIGHT_ALT}, 1, ' '};                // Right Alt+Space

uint8_t platform    = 0;   // 0 = Mac, 1 = Windows
uint8_t hotkeyIndex = 0;

static const Hotkey* activeHK()      { return platform ? HK_WIN : HK_MAC; }
static uint8_t       activeHKCount() { return platform ? HK_WIN_COUNT : HK_MAC_COUNT; }

// ---- 方向键字符映射 (实机用串口 `keymap` 确认；默认社区常见映射) ----
static char ARROW_UP    = ';';
static char ARROW_DOWN  = '.';
static char ARROW_LEFT  = ',';
static char ARROW_RIGHT = '/';

// ---- 运行态 ----
bool recState   = false;   // 听写假设状态 (单向发键无法读真值)
bool recState2  = false;   // Tab = Typeless 另一种输入的假设状态
bool selectMode = false;   // 选择模式
bool bleConn    = false;
int  batLevel   = -1;      // 平滑后用于显示/上报的电量 %
float battEma   = -1.0f;   // 电量 EMA (LiPo 电压随负载抖，需平滑)
bool charging   = false;
bool dirty      = true;    // UI 需重绘
uint32_t repairMsgUntil = 0;   // 重新配对提示横幅显示截止时间
NimBLECharacteristic* battChar = nullptr;   // BAS 电量特征 (0x2A19)

// ---- UI sprite (8bit 省内存, 单缓冲) ----
M5Canvas cv(&M5Cardputer.Display);

// ---- 串口 ----
String  cmdBuf;
uint32_t keymapUntil = 0;

// ---- 边沿检测状态 ----
bool prevCtrl = false, prevAlt = false, prevOpt = false;
bool prevTab = false, prevEsc = false;
bool prevFn = false, fnSolo = false;   // Fn 单独点按检测
bool prevShiftBacktick = false, prevShiftP = false, prevShiftR = false;  // Aa(Shift) 层
uint8_t lastFwKey = 0; int lastFwMod = 0; uint32_t fwNextRepeat = 0;
uint32_t lastDictMs = 0, lastSelMs = 0, lastOptMs = 0, lastTabMs = 0;   // 去抖冷却

// ===================== 工具 =====================

// BLE HID 组合键要稳：每个按键报文之间留间隔，整体按住一会儿再松开，
// 否则报文挨太近会被 BLE 丢掉或被主机识别不全 (Typeless 偶尔收不到)。
// key==0 表示纯修饰键 (如单独 Right Alt)，只发修饰键不带主键。
static void sendHotkeyDef(const Hotkey& h) {
  if (!bleKeyboard.isConnected()) return;
  for (uint8_t i = 0; i < h.nmod; i++) { bleKeyboard.press(h.mods[i]); delay(15); }
  if (h.key) bleKeyboard.press(h.key);
  delay(45);                  // 按住整组，确保主机稳定识别
  bleKeyboard.releaseAll();
  delay(15);                  // 给松开报文留出时间
}

static void sendHotkey() { sendHotkeyDef(activeHK()[hotkeyIndex]); }

static void sendKey(int mod, uint8_t key) {
  if (!bleKeyboard.isConnected()) return;
  if (mod) { bleKeyboard.press((uint8_t)mod); delay(15); }
  bleKeyboard.press(key);
  delay(25);
  bleKeyboard.releaseAll();
  delay(10);
}

static void sendShiftTab() {
  sendKey(KEY_LEFT_SHIFT, KEY_TAB);
}

// 返回 bmorcelli Launcher (无 Launcher 时为安全空操作)
static void returnToLauncher() {
  auto l = esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_TEST, nullptr);
  if (!l) l = esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_FACTORY, nullptr);
  if (!l || l == esp_ota_get_running_partition()) return;
  esp_ota_set_boot_partition(l);
  delay(60);
  esp_restart();
}

static void saveHotkey() {
  prefs.putUChar("hk", hotkeyIndex);
}

// 重新配对：断开当前连接 + 清掉所有 bond + 重新广播。
// 之后需在主机蓝牙里删除旧的 "Cardputer Voice" 再重新配对。
static void doRePair() {
  NimBLEServer* s = NimBLEDevice::getServer();
  if (s) {
    for (uint16_t id : s->getPeerDevices()) s->disconnect(id);
  }
  delay(120);
  NimBLEDevice::deleteAllBonds();
  battChar = nullptr;                 // 重新连接后重新定位电量特征
  NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
  if (adv && !adv->isAdvertising()) adv->start();
  bleConn = false;
  repairMsgUntil = millis() + 3000;
  dirty = true;
  Serial.println("[repair] bonds cleared, re-advertising. Remove 'Cardputer Voice' on host and pair again.");
}

static void updateBattery();   // 见 loop 前定义

// ===================== UI =====================

static void drawUI() {
  cv.fillSprite(TFT_BLACK);

  // 顶栏: BLE 状态 + 电池
  cv.setTextSize(1);
  cv.setTextColor(bleConn ? TFT_GREEN : TFT_ORANGE, TFT_BLACK);
  cv.setTextDatum(TL_DATUM);
  cv.drawString(bleConn ? "BLE: Connected" : "BLE: Advertising...", 4, 4);
  {
    char bat[12];
    if (batLevel < 0) snprintf(bat, sizeof(bat), "--%%");
    else              snprintf(bat, sizeof(bat), "%s%d%%", charging ? "+" : "", batLevel);
    uint16_t col = charging ? TFT_GREEN
                 : (batLevel >= 0 && batLevel <= 15) ? TFT_RED
                 : TFT_WHITE;
    cv.setTextColor(col, TFT_BLACK);
    cv.setTextDatum(TR_DATUM);
    cv.drawString(bat, 236, 4);
  }
  if (recState2) {                       // Tab = Typeless 另一种输入，开启时顶栏中间标注
    cv.setTextColor(TFT_ORANGE, TFT_BLACK);
    cv.setTextDatum(TC_DATUM);
    cv.drawString("TYPE2:ON", 120, 4);
  }
  cv.drawFastHLine(0, 18, 240, 0x4208);

  // 中部大字: 听写状态
  cv.setTextDatum(MC_DATUM);
  cv.setTextSize(4);
  if (recState) {
    cv.setTextColor(TFT_RED, TFT_BLACK);
    cv.drawString("REC", 120, 58);
  } else {
    cv.setTextColor(0x7BEF, TFT_BLACK);
    cv.drawString("IDLE", 120, 58);
  }

  // 选择模式徽标
  if (selectMode) {
    cv.setTextSize(2);
    cv.setTextColor(TFT_YELLOW, TFT_BLACK);
    cv.drawString("SELECT", 120, 95);
  }

  // 底栏: 平台 + 当前听写键，再加按键速查
  cv.setTextSize(1);
  cv.setTextDatum(BL_DATUM);
  cv.setTextColor(platform ? TFT_CYAN : TFT_GREEN, TFT_BLACK);
  cv.drawString(String(platform ? "WIN" : "MAC") + "  mic:" + activeHK()[hotkeyIndex].name, 4, 120);
  cv.setTextColor(0x8410, TFT_BLACK);
  cv.drawString("Ctrl=mic Opt=mode Alt=sel Aa+P=OS", 4, 132);

  // 重新配对横幅 (覆盖在最上层)
  if (repairMsgUntil) {
    cv.fillRect(0, 44, 240, 48, 0x000F);
    cv.setTextDatum(MC_DATUM);
    cv.setTextColor(TFT_YELLOW, 0x000F);
    cv.setTextSize(2);
    cv.drawString("RE-PAIRING", 120, 60);
    cv.setTextSize(1);
    cv.drawString("remove on host & pair again", 120, 82);
  }

  cv.pushSprite(0, 0);
}

// ===================== 串口控制台 =====================

static void printStatus() {
  Serial.printf("[status] BLE=%s platform=%s rec=%s type2=%s select=%s hotkey=%s(%d) batt=%d%%\n",
                bleConn ? "connected" : "advertising",
                platform ? "Windows" : "Mac",
                recState ? "REC" : "IDLE",
                recState2 ? "on" : "off",
                selectMode ? "on" : "off",
                activeHK()[hotkeyIndex].name, hotkeyIndex, batLevel);
}

static void handleCommand(String line) {
  line.trim();
  if (line.length() == 0) return;

  if (line == "status") {
    printStatus();
  } else if (line.startsWith("platform")) {
    String a = line.substring(8); a.trim();
    if (a == "mac")      platform = 0;
    else if (a == "win") platform = 1;
    else                 platform ^= 1;          // 无参数则切换
    if (hotkeyIndex >= activeHKCount()) hotkeyIndex = 0;
    prefs.putUChar("plat", platform);
    saveHotkey();
    dirty = true;
    Serial.printf("[platform] -> %s\n", platform ? "Windows" : "Mac");
  } else if (line.startsWith("hotkey")) {
    int idx = line.substring(6).toInt();
    if (idx >= 0 && idx < activeHKCount()) {
      hotkeyIndex = idx;
      saveHotkey();
      dirty = true;
      Serial.printf("[hotkey] -> %s\n", activeHK()[hotkeyIndex].name);
    } else {
      Serial.printf("[hotkey] usage: hotkey <0..%d> (%s)\n", activeHKCount() - 1, platform ? "Win" : "Mac");
      for (uint8_t i = 0; i < activeHKCount(); i++) Serial.printf("  %d = %s\n", i, activeHK()[i].name);
    }
  } else if (line == "send") {
    sendHotkey();
    recState = !recState;
    dirty = true;
    Serial.printf("[send] hotkey sent, assumed rec=%s\n", recState ? "REC" : "IDLE");
  } else if (line == "keymap") {
    keymapUntil = millis() + 10000;
    Serial.println("[keymap] press keys on Cardputer for 10s; printing keysState...");
  } else if (line == "reset") {
    recState = false;
    recState2 = false;
    selectMode = false;
    dirty = true;
    Serial.println("[reset] state -> IDLE, type2 off, select off");
  } else if (line == "repair") {
    doRePair();
  } else {
    Serial.println("[?] cmds: status | platform [mac|win] | hotkey <n> | send | keymap | reset | repair");
  }
}

static void pollSerial() {
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\n' || c == '\r') {
      if (cmdBuf.length()) { handleCommand(cmdBuf); cmdBuf = ""; }
    } else if (cmdBuf.length() < 64) {
      cmdBuf += c;
    }
  }
}

// ===================== 键盘逻辑 =====================

// 判断 word 里是否含某字符
static bool hasChar(const std::vector<char>& w, char target) {
  for (char c : w) if (c == target) return true;
  return false;
}

static void handleKeyboard() {
  auto ks = M5Cardputer.Keyboard.keysState();

  // keymap 调试: 打印当前按下的键
  if (keymapUntil && millis() < keymapUntil) {
    if (M5Cardputer.Keyboard.isChange() && M5Cardputer.Keyboard.isPressed()) {
      Serial.print("[keymap] word=[");
      for (char c : ks.word) Serial.printf("'%c'(%d) ", c, c);
      Serial.printf("] ctrl=%d alt=%d opt=%d fn=%d shift=%d tab=%d enter=%d space=%d del=%d\n",
                    ks.ctrl, ks.alt, ks.opt, ks.fn, ks.shift, ks.tab, ks.enter, ks.space, ks.del);
    }
  } else if (keymapUntil) {
    keymapUntil = 0;
    Serial.println("[keymap] done");
  }

  // --- 本地控制键：检测该键"按下沿"(每次物理按下必触发一次)，
  //     仅在按下那一刻没有其他键同时按着时才生效，避免组合误碰。 ---
  bool otherKey = !ks.word.empty() || ks.tab || ks.enter || ks.space || ks.del;

  // Ctrl 按下 -> 听写 (2s 间隔防快按失步)
  if (ks.ctrl && !prevCtrl) {
    if (!ks.alt && !ks.opt && !ks.fn && !ks.shift && !otherKey && millis() - lastDictMs > 500) {
      sendHotkey(); recState = !recState; lastDictMs = millis(); dirty = true;
    }
  }
  // Tab 按下 -> Typeless 另一种输入 (与 Ctrl 一致: 单独按下 + 2s 间隔)
  if (ks.tab && !prevTab) {
    bool tabOther = !ks.word.empty() || ks.enter || ks.space || ks.del;  // tab 以外的其他键
    if (!ks.ctrl && !ks.alt && !ks.opt && !ks.fn && !ks.shift && !tabOther && millis() - lastTabMs > 500) {
      sendHotkeyDef(platform ? TAB_WIN : TAB_MAC);
      recState2 = !recState2; lastTabMs = millis(); dirty = true;
    }
  }
  // Alt 按下 -> 选择模式
  if (ks.alt && !prevAlt) {
    if (!ks.ctrl && !ks.opt && !ks.fn && !ks.shift && !otherKey && millis() - lastSelMs > 250) {
      selectMode = !selectMode; lastSelMs = millis(); dirty = true;
    }
  }
  // Opt 按下 -> Shift+Tab
  if (ks.opt && !prevOpt) {
    if (!ks.ctrl && !ks.alt && !ks.fn && !ks.shift && !otherKey && millis() - lastOptMs > 250) {
      sendShiftTab(); lastOptMs = millis();
    }
  }

  // Fn 单独点按(松开时、按下期间无其他键) -> 发 Enter；Fn+其他键仍是层功能
  if (ks.fn && !prevFn) fnSolo = true;
  if (ks.fn && (!ks.word.empty() || ks.ctrl || ks.alt || ks.opt || ks.shift ||
                ks.enter || ks.space || ks.del || ks.tab)) fnSolo = false;
  if (!ks.fn && prevFn) {
    if (fnSolo) sendKey(0, KEY_RETURN);
    fnSolo = false;
  }
  prevFn = ks.fn;

  bool backtick = hasChar(ks.word, '`') || hasChar(ks.word, '~');

  // REC 状态下按 Esc -> 同步退出本地 REC 指示 (仅非 Shift 层；Esc 照常转发)
  if (!ks.shift && backtick && !prevEsc && recState) { recState = false; dirty = true; }
  prevEsc = backtick;

  // Aa(Shift) 层: 组合功能 (不转发普通键)
  if (ks.shift) {
    // Shift + ` -> 返回 Launcher
    if (backtick && !prevShiftBacktick) returnToLauncher();
    prevShiftBacktick = backtick;

    // Shift + P -> 切换平台 (Mac <-> Windows)，存 NVS
    bool sP = hasChar(ks.word, 'p') || hasChar(ks.word, 'P');
    if (sP && !prevShiftP) {
      platform ^= 1;
      if (hotkeyIndex >= activeHKCount()) hotkeyIndex = 0;
      prefs.putUChar("plat", platform);
      saveHotkey();
      dirty = true;
      Serial.printf("[platform] -> %s\n", platform ? "Windows" : "Mac");
    }
    prevShiftP = sP;

    // Shift + R -> 重新配对 (清 bond + 重新广播)
    bool sR = hasChar(ks.word, 'r') || hasChar(ks.word, 'R');
    if (sR && !prevShiftR) doRePair();
    prevShiftR = sR;

    // Shift 层不做普通转发
    lastFwKey = 0; lastFwMod = 0;
  } else {
    prevShiftBacktick = false;
    prevShiftP = false;
    prevShiftR = false;

    // --- 转发键判定 ---
    uint8_t curKey = 0; int curMod = 0; bool curIsArrow = false; bool curRepeat = false;
    if (hasChar(ks.word, ARROW_UP))         { curKey = KEY_UP_ARROW;    curIsArrow = true; }
    else if (hasChar(ks.word, ARROW_DOWN))  { curKey = KEY_DOWN_ARROW;  curIsArrow = true; }
    else if (hasChar(ks.word, ARROW_LEFT))  { curKey = KEY_LEFT_ARROW;  curIsArrow = true; }
    else if (hasChar(ks.word, ARROW_RIGHT)) { curKey = KEY_RIGHT_ARROW; curIsArrow = true; }
    else if (backtick)                      { curKey = KEY_ESC; }
    else if (ks.space)                      { curKey = ' '; }
    else if (ks.enter)                      { curKey = KEY_RETURN; curMod = KEY_LEFT_CTRL; }  // Enter 键 = Ctrl+Enter
    else if (ks.del)                        { curKey = KEY_BACKSPACE; curRepeat = true; }     // 退格：按住连删

    if (curKey && curIsArrow && selectMode) curMod = KEY_LEFT_SHIFT;
    curRepeat = curRepeat || curIsArrow;     // 方向键与退格支持长按重复

    uint32_t now = millis();
    if (curKey) {
      bool isNew = (curKey != lastFwKey || curMod != lastFwMod);
      if (isNew) {
        sendKey(curMod, curKey);
        lastFwKey = curKey; lastFwMod = curMod;
        fwNextRepeat = now + 350;            // 初次按下后的重复延迟
      } else if (curRepeat && now >= fwNextRepeat) {
        sendKey(curMod, curKey);             // 方向键/退格长按重复
        fwNextRepeat = now + 90;
      }
    } else {
      lastFwKey = 0; lastFwMod = 0;
    }
  }

  prevCtrl = ks.ctrl;
  prevAlt  = ks.alt;
  prevOpt  = ks.opt;
  prevTab  = ks.tab;
}

// ===================== Arduino =====================

void setup() {
  auto cfg = M5.config();
  M5Cardputer.begin(cfg, true);
  M5Cardputer.Display.setRotation(1);

  cv.setColorDepth(8);                 // 省内存 (240x135x1B ≈ 32KB)
  cv.createSprite(240, 135);

  prefs.begin("typeless", false);
  platform    = prefs.getUChar("plat", 0) ? 1 : 0;
  hotkeyIndex = prefs.getUChar("hk", 0);
  if (hotkeyIndex >= activeHKCount()) hotkeyIndex = 0;

  Serial.begin(115200);
  Serial.println("\n[boot] Cardputer Voice (BLE HID) -> Typeless / Claude Code");
  Serial.printf("[boot] platform=%s hotkey=%s  cmds: status|platform [mac|win]|hotkey <n>|send|keymap|reset\n",
                platform ? "Windows" : "Mac", activeHK()[hotkeyIndex].name);

  bleKeyboard.begin();
  updateBattery();        // 开机先读一次，避免首屏显示 --
  drawUI();
}

// T-vK 的 setBatteryLevel 只 setValue 不 notify，主机一直读到初始值。
// 直接拿到 BAS(0x180F) 的电量特征(0x2A19)，setValue + notify，主机才会更新。
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

// ADV 用 pmic_m5pm1：getBatteryLevel() 是电压线性映射，负载下会抖。
// 用 EMA 平滑 + 忽略错误读数 + 跟踪充电状态，再上报给 BLE 电量服务。
static void updateBattery() {
  int raw = M5.Power.getBatteryLevel();
  if (raw >= 0 && raw <= 100) {
    if (battEma < 0) battEma = raw;               // 首次直接取值
    else             battEma = battEma * 0.8f + raw * 0.2f;
    int lv = (int)lroundf(battEma);
    if (lv != batLevel) { batLevel = lv; dirty = true; }
  }
  bool ch = ((int)M5.Power.isCharging() == 1);    // 1 = is_charging
  if (ch != charging) { charging = ch; dirty = true; }
  if (bleConn && batLevel >= 0) {
    bleKeyboard.setBatteryLevel((uint8_t)batLevel);   // 同步 T-vK 内部值
    pushBatteryBle((uint8_t)batLevel);                // 真正 notify 主机
  }
}

void loop() {
  M5Cardputer.update();
  pollSerial();
  handleKeyboard();

  // BLE 连接状态 (1s) + 电量平滑/上报 (2s)
  static uint32_t tConn = 0, tBatt = 0;
  if (millis() - tConn > 1000) {
    tConn = millis();
    bool c = bleKeyboard.isConnected();
    if (c != bleConn) { bleConn = c; dirty = true; }
  }
  if (millis() - tBatt > 2000) {
    tBatt = millis();
    updateBattery();
  }
  if (repairMsgUntil && millis() > repairMsgUntil) { repairMsgUntil = 0; dirty = true; }

  if (dirty) { drawUI(); dirty = false; }

  delay(8);
}
