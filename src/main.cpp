// Cardputer ADV -> Typeless 语音输入 + Claude Code 控制台 (BLE HID)
//
// 电脑把本机当蓝牙键盘。Cardputer 按键映射：
//   Ctrl        -> 发听写热键 (默认 Ctrl+Alt+\) = Typeless 听写开/关 (toggle)
//   Alt         -> 选择模式开关 (本地)。开时方向键变 Shift+方向
//   Opt         -> 发 Shift+Tab = 切换 Claude Code 模式
//   方向键      -> 转发 ↑↓←→ (选择模式下加 Shift)
//   Esc(` 键)   -> 转发 Esc
//   Space/Enter/Del/Tab -> 转发 Space/Enter/Backspace/Tab
//   Fn + Enter   -> Ctrl+Enter
//   Fn 层 (按住 Fn):
//     Fn + P    -> 切换平台 Mac/Windows (存 NVS，各平台用不同听写组合键)
//     Fn + ↑/↓  -> 切换当前平台的听写热键预设 (存 NVS)
//     Fn + `    -> 返回 bmorcelli Launcher
//
// 串口命令: status / platform [mac|win] / hotkey <n> / send / keymap / reset
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
  {"Win+H",       {KEY_LEFT_GUI}, 1, 'h'},                  // Windows 内置语音输入
  {"Ctrl+Alt+\\", {KEY_LEFT_CTRL, KEY_LEFT_ALT}, 2, '\\'},  // 第三方听写 (同 Mac 默认)
  {"Ctrl+Alt+.",  {KEY_LEFT_CTRL, KEY_LEFT_ALT}, 2, '.'},
};
static const uint8_t HK_MAC_COUNT = sizeof(HK_MAC) / sizeof(HK_MAC[0]);
static const uint8_t HK_WIN_COUNT = sizeof(HK_WIN) / sizeof(HK_WIN[0]);

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
bool selectMode = false;   // 选择模式
bool bleConn    = false;
int  batLevel   = -1;
bool dirty      = true;    // UI 需重绘

// ---- UI sprite (8bit 省内存, 单缓冲) ----
M5Canvas cv(&M5Cardputer.Display);

// ---- 串口 ----
String  cmdBuf;
uint32_t keymapUntil = 0;

// ---- 边沿检测状态 ----
bool prevCtrl = false, prevAlt = false, prevOpt = false;
bool prevFnBacktick = false;
bool prevFnEnter = false;
bool prevFnP = false;
char prevFnArrow = 0;
uint8_t lastFwKey = 0; int lastFwMod = 0; uint32_t fwNextRepeat = 0;
uint32_t lastDictMs = 0, lastSelMs = 0, lastOptMs = 0;   // 去抖冷却

// ===================== 工具 =====================

// BLE HID 组合键要稳：每个按键报文之间留间隔，整体按住一会儿再松开，
// 否则报文挨太近会被 BLE 丢掉或被主机识别不全 (Typeless 偶尔收不到)。
static void sendHotkey() {
  if (!bleKeyboard.isConnected()) return;
  const Hotkey& h = activeHK()[hotkeyIndex];
  for (uint8_t i = 0; i < h.nmod; i++) { bleKeyboard.press(h.mods[i]); delay(15); }
  bleKeyboard.press(h.key);
  delay(45);                  // 按住整组，确保主机稳定识别
  bleKeyboard.releaseAll();
  delay(15);                  // 给松开报文留出时间
}

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

// ===================== UI =====================

static void drawUI() {
  cv.fillSprite(TFT_BLACK);

  // 顶栏: BLE 状态 + 电池
  cv.setTextSize(1);
  cv.setTextColor(bleConn ? TFT_GREEN : TFT_ORANGE, TFT_BLACK);
  cv.setTextDatum(TL_DATUM);
  cv.drawString(bleConn ? "BLE: Connected" : "BLE: Advertising...", 4, 4);
  if (batLevel >= 0) {
    char bat[8];
    snprintf(bat, sizeof(bat), "%d%%", batLevel);
    cv.setTextColor(TFT_WHITE, TFT_BLACK);
    cv.setTextDatum(TR_DATUM);
    cv.drawString(bat, 236, 4);
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
  cv.drawString("Ctrl=mic Opt=mode Alt=sel Fn+P=OS", 4, 132);

  cv.pushSprite(0, 0);
}

// ===================== 串口控制台 =====================

static void printStatus() {
  Serial.printf("[status] BLE=%s platform=%s rec=%s select=%s hotkey=%s(%d) batt=%d%%\n",
                bleConn ? "connected" : "advertising",
                platform ? "Windows" : "Mac",
                recState ? "REC" : "IDLE",
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
    selectMode = false;
    dirty = true;
    Serial.println("[reset] state -> IDLE, select off");
  } else {
    Serial.println("[?] cmds: status | platform [mac|win] | hotkey <n> | send | keymap | reset");
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
    if (!ks.alt && !ks.opt && !ks.fn && !ks.shift && !otherKey && millis() - lastDictMs > 2000) {
      sendHotkey(); recState = !recState; lastDictMs = millis(); dirty = true;
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

  bool backtick = hasChar(ks.word, '`') || hasChar(ks.word, '~');

  // Fn 层: 配置 / Launcher (不转发普通键)
  if (ks.fn) {
    // Fn + ` -> 返回 Launcher
    bool fnBacktick = backtick;
    if (fnBacktick && !prevFnBacktick) returnToLauncher();
    prevFnBacktick = fnBacktick;

    // Fn + Enter -> Ctrl+Enter
    if (ks.enter && !prevFnEnter) sendKey(KEY_LEFT_CTRL, KEY_RETURN);
    prevFnEnter = ks.enter;

    // Fn + P -> 切换平台 (Mac <-> Windows)，存 NVS
    bool fnP = hasChar(ks.word, 'p') || hasChar(ks.word, 'P');
    if (fnP && !prevFnP) {
      platform ^= 1;
      if (hotkeyIndex >= activeHKCount()) hotkeyIndex = 0;
      prefs.putUChar("plat", platform);
      saveHotkey();
      dirty = true;
      Serial.printf("[platform] -> %s\n", platform ? "Windows" : "Mac");
    }
    prevFnP = fnP;

    // Fn + 上/下 -> 切换听写热键预设
    char fnArrow = 0;
    if (hasChar(ks.word, ARROW_UP))   fnArrow = 'U';
    else if (hasChar(ks.word, ARROW_DOWN)) fnArrow = 'D';
    if (fnArrow && fnArrow != prevFnArrow) {
      uint8_t n = activeHKCount();
      if (fnArrow == 'U') hotkeyIndex = (hotkeyIndex + n - 1) % n;
      else                hotkeyIndex = (hotkeyIndex + 1) % n;
      saveHotkey();
      dirty = true;
      Serial.printf("[hotkey] -> %s\n", activeHK()[hotkeyIndex].name);
    }
    prevFnArrow = fnArrow;

    // Fn 按下时不做普通转发，清空转发态
    lastFwKey = 0; lastFwMod = 0;
  } else {
    prevFnBacktick = false;
    prevFnEnter = false;
    prevFnP = false;
    prevFnArrow = 0;

    // --- 转发键判定 ---
    uint8_t curKey = 0; int curMod = 0; bool curIsArrow = false; bool curRepeat = false;
    if (hasChar(ks.word, ARROW_UP))         { curKey = KEY_UP_ARROW;    curIsArrow = true; }
    else if (hasChar(ks.word, ARROW_DOWN))  { curKey = KEY_DOWN_ARROW;  curIsArrow = true; }
    else if (hasChar(ks.word, ARROW_LEFT))  { curKey = KEY_LEFT_ARROW;  curIsArrow = true; }
    else if (hasChar(ks.word, ARROW_RIGHT)) { curKey = KEY_RIGHT_ARROW; curIsArrow = true; }
    else if (backtick)                      { curKey = KEY_ESC; }
    else if (ks.space)                      { curKey = ' '; }
    else if (ks.enter)                      { curKey = KEY_RETURN; }
    else if (ks.del)                        { curKey = KEY_BACKSPACE; curRepeat = true; }  // 退格：按住连删
    else if (ks.tab)                        { curKey = KEY_TAB; }

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
  drawUI();
}

void loop() {
  M5Cardputer.update();
  pollSerial();
  handleKeyboard();

  // BLE 连接状态 / 电池 / 电量上报 (1s 节流)
  static uint32_t tBatt = 0;
  if (millis() - tBatt > 1000) {
    tBatt = millis();
    bool c = bleKeyboard.isConnected();
    if (c != bleConn) { bleConn = c; dirty = true; }
    int b = M5.Power.getBatteryLevel();
    if (b != batLevel) { batLevel = b; dirty = true; }
    if (bleConn && batLevel >= 0) bleKeyboard.setBatteryLevel((uint8_t)batLevel);
  }

  if (dirty) { drawUI(); dirty = false; }

  delay(8);
}
