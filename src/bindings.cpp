#include "bindings.h"

#include <BleKeyboard.h>
#include <NimBLEDevice.h>
#include <Preferences.h>
#include <stdio.h>
#include <string.h>
#include <vector>

extern Preferences prefs;

static RemapEntry gMac[MAX_REMAPS];
static RemapEntry gWin[MAX_REMAPS];
static bool gPassThrough = false;

static RemapEntry* table(uint8_t platform) { return platform ? gWin : gMac; }

bool remapsPassThrough() { return gPassThrough; }

void remapsSetPassThrough(bool on) {
  gPassThrough = on;
  prefs.putBool("passthru", on);
}

void remapsClearPlatform(uint8_t platform) {
  memset(table(platform), 0, sizeof(RemapEntry) * MAX_REMAPS);
}

void remapsLoad() {
  remapsClearPlatform(0);
  remapsClearPlatform(1);
  gPassThrough = prefs.getBool("passthru", false);
  // 新 schema：mapv2*，旧 typeless/rm* 一律忽略
  for (uint8_t p = 0; p < 2; p++) {
    char key[12];
    snprintf(key, sizeof(key), "mapv2%d", p);
    if (!prefs.isKey(key)) continue;
    size_t len = prefs.getBytesLength(key);
    if (len == 0 || len > sizeof(RemapEntry) * MAX_REMAPS) continue;
    prefs.getBytes(key, table(p), len);
  }
}

void remapsSavePlatform(uint8_t platform) {
  char key[12];
  snprintf(key, sizeof(key), "mapv2%d", platform);
  prefs.putBytes(key, table(platform), sizeof(RemapEntry) * MAX_REMAPS);
}

void remapsSaveBoth() {
  remapsSavePlatform(0);
  remapsSavePlatform(1);
}

uint8_t platformDetectHost() {
  // 打分：>0 偏 Windows，<=0 偏 Mac。不写 NVS。
  NimBLEServer* s = NimBLEDevice::getServer();
  if (!s) return 0;
  if (s->getPeerDevices().empty()) return 0;

  NimBLEConnInfo info = s->getPeerInfo((size_t)0);
  int score = 0;

  // Apple 主机几乎总会订阅 Battery Level 通知
  NimBLEService* bas = s->getServiceByUUID(NimBLEUUID((uint16_t)0x180F));
  if (bas) {
    NimBLECharacteristic* bat = bas->getCharacteristic(NimBLEUUID((uint16_t)0x2A19));
    if (bat && bat->getSubscribedCount() > 0) score -= 4;
  }

  // 连接间隔单位 1.25ms：Apple 常见 12–24（15–30ms），Windows 常更大
  uint16_t itvl = info.getConnInterval();
  if (itvl > 0 && itvl <= 16) score -= 2;
  else if (itvl >= 30) score += 2;

  // Apple 常协商更大 MTU
  uint16_t mtu = info.getMTU();
  if (mtu >= 100) score -= 1;
  else if (mtu > 0 && mtu <= 23) score += 1;

  // 公有地址更常见于部分 Windows 适配器；随机/RPA 更常见于 Apple
  uint8_t at = info.getAddress().getType();
  if (at == BLE_ADDR_PUBLIC) score += 2;
  else if (at == BLE_ADDR_RANDOM || at == BLE_ADDR_RANDOM_ID) score -= 1;

  uint8_t plat = (score > 0) ? 1 : 0;
  Serial.printf("[platform] detect score=%d itvl=%u mtu=%u addrType=%u -> %s\n",
                score, itvl, mtu, at, plat ? "Windows" : "Mac");
  return plat;
}

bool platformApplyDetected(uint8_t* platformInOut) {
  if (!platformInOut) return false;
  uint8_t d = platformDetectHost();
  if (d == *platformInOut) return false;
  *platformInOut = d;
  prefs.putUChar("plat", d);  // 仅作断电后默认显示，下次连上仍会重探测
  return true;
}

uint8_t remapCount(uint8_t platform) {
  RemapEntry* t = table(platform);
  uint8_t n = 0;
  for (uint8_t i = 0; i < MAX_REMAPS; i++) if (t[i].used) n++;
  return n;
}

const RemapEntry* remapAt(uint8_t platform, uint8_t index) {
  RemapEntry* t = table(platform);
  uint8_t n = 0;
  for (uint8_t i = 0; i < MAX_REMAPS; i++) {
    if (!t[i].used) continue;
    if (n == index) return &t[i];
    n++;
  }
  return nullptr;
}

static void sortBytes(uint8_t* a, uint8_t n) {
  for (uint8_t i = 1; i < n; i++) {
    uint8_t v = a[i], j = i;
    while (j > 0 && a[j - 1] > v) {
      a[j] = a[j - 1];
      j--;
    }
    a[j] = v;
  }
}

bool comboEqual(const KeyCombo& a, const KeyCombo& b) {
  if (a.nmod != b.nmod || a.nkey != b.nkey) return false;
  uint8_t am[4], bm[4], ak[6], bk[6];
  memcpy(am, a.mods, 4);
  memcpy(bm, b.mods, 4);
  memcpy(ak, a.keys, 6);
  memcpy(bk, b.keys, 6);
  sortBytes(am, a.nmod);
  sortBytes(bm, b.nmod);
  sortBytes(ak, a.nkey);
  sortBytes(bk, b.nkey);
  return memcmp(am, bm, a.nmod) == 0 && memcmp(ak, bk, a.nkey) == 0;
}

bool remapAdvConflict(uint8_t platform, const KeyCombo& adv, int ignoreIndex) {
  RemapEntry* t = table(platform);
  int idx = 0;
  for (uint8_t i = 0; i < MAX_REMAPS; i++) {
    if (!t[i].used) continue;
    if (idx != ignoreIndex && comboEqual(t[i].adv, adv)) return true;
    idx++;
  }
  return false;
}

bool remapAdd(uint8_t platform, const KeyCombo& adv, const KeyCombo& pc) {
  if (remapAdvConflict(platform, adv, -1)) return false;
  RemapEntry* t = table(platform);
  for (uint8_t i = 0; i < MAX_REMAPS; i++) {
    if (!t[i].used) {
      t[i].used = 1;
      t[i].adv = adv;
      t[i].pc = pc;
      return true;
    }
  }
  return false;
}

void remapRemoveAt(uint8_t platform, uint8_t index) {
  RemapEntry* t = table(platform);
  uint8_t n = 0;
  for (uint8_t i = 0; i < MAX_REMAPS; i++) {
    if (!t[i].used) continue;
    if (n == index) {
      memset(&t[i], 0, sizeof(RemapEntry));
      return;
    }
    n++;
  }
}

static bool physInCombo(const KeyCombo& c, uint8_t phys) {
  for (uint8_t i = 0; i < c.nmod; i++) if (c.mods[i] == phys) return true;
  for (uint8_t i = 0; i < c.nkey; i++) if (c.keys[i] == phys) return true;
  return false;
}

bool physUsedInAnyTrigger(uint8_t platform, uint8_t phys) {
  RemapEntry* t = table(platform);
  for (uint8_t i = 0; i < MAX_REMAPS; i++) {
    if (t[i].used && physInCombo(t[i].adv, phys)) return true;
  }
  return false;
}

static bool advPhysDown(uint8_t phys, const AdvPress& s) {
  switch (phys) {
    case PK_CTRL: return s.ctrl;
    case PK_ALT: return s.alt;
    case PK_OPT: return s.opt;
    case PK_FN: return s.fn;
    case PK_SHIFT: return s.shift;
    case PK_TAB: return s.tab;
    case PK_ENTER: return s.enter;
    case PK_SPACE: return s.space;
    case PK_DEL: return s.del;
    case PK_ESC: return s.esc;
    default:
      if (phys < 128) return s.ascii[phys];
      return false;
  }
}

bool comboMatchAdv(const KeyCombo& adv, const AdvPress& s) {
  if (adv.nmod == 0 && adv.nkey == 0) return false;
  for (uint8_t i = 0; i < adv.nmod; i++) {
    if (!advPhysDown(adv.mods[i], s)) return false;
  }
  for (uint8_t i = 0; i < adv.nkey; i++) {
    if (!advPhysDown(adv.keys[i], s)) return false;
  }
  return true;
}

const char* physKeyName(uint8_t phys) {
  switch (phys) {
    case PK_CTRL: return "ctrl";
    case PK_ALT: return "alt";
    case PK_OPT: return "opt";
    case PK_FN: return "fn";
    case PK_SHIFT: return "shift";
    case PK_TAB: return "tab";
    case PK_ENTER: return "enter";
    case PK_SPACE: return "space";
    case PK_DEL: return "del";
    case PK_ESC: return "esc";
    case PK_UP: return "up";
    case PK_DOWN: return "down";
    case PK_LEFT: return "left";
    case PK_RIGHT: return "right";
    default: break;
  }
  static char buf[4];
  if (phys >= 32 && phys < 127) {
    buf[0] = (char)phys;
    buf[1] = 0;
    return buf;
  }
  return "?";
}

uint8_t physKeyFromName(const char* name) {
  if (!name || !name[0]) return 0;
  if (!strcmp(name, "ctrl")) return PK_CTRL;
  if (!strcmp(name, "alt")) return PK_ALT;
  if (!strcmp(name, "opt")) return PK_OPT;
  if (!strcmp(name, "fn")) return PK_FN;
  if (!strcmp(name, "shift") || !strcmp(name, "aa")) return PK_SHIFT;
  if (!strcmp(name, "tab")) return PK_TAB;
  if (!strcmp(name, "enter")) return PK_ENTER;
  if (!strcmp(name, "space")) return PK_SPACE;
  if (!strcmp(name, "del")) return PK_DEL;
  if (!strcmp(name, "esc")) return PK_ESC;
  if (!strcmp(name, "up")) return PK_UP;
  if (!strcmp(name, "down")) return PK_DOWN;
  if (!strcmp(name, "left")) return PK_LEFT;
  if (!strcmp(name, "right")) return PK_RIGHT;
  if (strlen(name) == 1) {
    char c = name[0];
    if (c >= 'A' && c <= 'Z') c = c - 'A' + 'a';
    return (uint8_t)c;
  }
  return 0;
}

uint8_t hidModFromName(const char* name) {
  if (!name) return 0;
  if (!strcmp(name, "ctrl")) return KEY_LEFT_CTRL;
  if (!strcmp(name, "shift")) return KEY_LEFT_SHIFT;
  if (!strcmp(name, "alt")) return KEY_LEFT_ALT;
  if (!strcmp(name, "gui")) return KEY_LEFT_GUI;
  if (!strcmp(name, "ralt")) return KEY_RIGHT_ALT;
  return 0;
}

const char* hidModName(uint8_t mod) {
  switch (mod) {
    case KEY_LEFT_CTRL: return "ctrl";
    case KEY_LEFT_SHIFT: return "shift";
    case KEY_LEFT_ALT: return "alt";
    case KEY_LEFT_GUI: return "gui";
    case KEY_RIGHT_ALT: return "ralt";
    default: return "";
  }
}

uint8_t hidKeyFromName(const char* name) {
  if (!name || !name[0]) return 0;
  if (!strcmp(name, "enter")) return KEY_RETURN;
  if (!strcmp(name, "tab")) return KEY_TAB;
  if (!strcmp(name, "esc")) return KEY_ESC;
  if (!strcmp(name, "bksp")) return KEY_BACKSPACE;
  if (!strcmp(name, "up")) return KEY_UP_ARROW;
  if (!strcmp(name, "down")) return KEY_DOWN_ARROW;
  if (!strcmp(name, "left")) return KEY_LEFT_ARROW;
  if (!strcmp(name, "right")) return KEY_RIGHT_ARROW;
  if (!strcmp(name, "space")) return ' ';
  // F1–F12
  if (name[0] == 'f' || name[0] == 'F') {
    if (!strcmp(name, "f1") || !strcmp(name, "F1")) return KEY_F1;
    if (!strcmp(name, "f2") || !strcmp(name, "F2")) return KEY_F2;
    if (!strcmp(name, "f3") || !strcmp(name, "F3")) return KEY_F3;
    if (!strcmp(name, "f4") || !strcmp(name, "F4")) return KEY_F4;
    if (!strcmp(name, "f5") || !strcmp(name, "F5")) return KEY_F5;
    if (!strcmp(name, "f6") || !strcmp(name, "F6")) return KEY_F6;
    if (!strcmp(name, "f7") || !strcmp(name, "F7")) return KEY_F7;
    if (!strcmp(name, "f8") || !strcmp(name, "F8")) return KEY_F8;
    if (!strcmp(name, "f9") || !strcmp(name, "F9")) return KEY_F9;
    if (!strcmp(name, "f10") || !strcmp(name, "F10")) return KEY_F10;
    if (!strcmp(name, "f11") || !strcmp(name, "F11")) return KEY_F11;
    if (!strcmp(name, "f12") || !strcmp(name, "F12")) return KEY_F12;
  }
  // 媒体 / 消费类（笔记本 Fn 层常见动作）
  if (!strcmp(name, "vol_up") || !strcmp(name, "vol+")) return 0xA1;
  if (!strcmp(name, "vol_down") || !strcmp(name, "vol-")) return 0xA2;
  if (!strcmp(name, "mute")) return 0xA3;
  if (!strcmp(name, "play") || !strcmp(name, "play_pause")) return 0xA4;
  if (!strcmp(name, "next")) return 0xA5;
  if (!strcmp(name, "prev")) return 0xA6;
  if (!strcmp(name, "stop")) return 0xA7;
  if (!strcmp(name, "www_home") || !strcmp(name, "home")) return 0xA8;
  if (!strcmp(name, "calc")) return 0xA9;
  if (!strcmp(name, "search")) return 0xAA;
  if (!strcmp(name, "back")) return 0xAB;
  if (!strcmp(name, "email")) return 0xAC;
  if (strlen(name) == 1) return (uint8_t)name[0];
  return 0;
}

bool hidIsMedia(uint8_t key) {
  return key >= 0xA1 && key <= 0xAC;
}

bool hidMediaReportBytes(uint8_t key, uint8_t out[2]) {
  if (!out || !hidIsMedia(key)) return false;
  // 与 BleKeyboard.h MediaKeyReport 字节一致
  static const uint8_t kReports[][2] = {
    {32, 0},   // A1 vol_up
    {64, 0},   // A2 vol_down
    {16, 0},   // A3 mute
    {8, 0},    // A4 play_pause
    {1, 0},    // A5 next
    {2, 0},    // A6 prev
    {4, 0},    // A7 stop
    {128, 0},  // A8 www_home
    {0, 2},    // A9 calc
    {0, 8},    // AA search
    {0, 32},   // AB back
    {0, 128},  // AC email
  };
  const uint8_t* r = kReports[key - 0xA1];
  out[0] = r[0];
  out[1] = r[1];
  return true;
}

void hidKeyToName(uint8_t key, char* out, size_t outLen) {
  if (!out || !outLen) return;
  if (key == KEY_RETURN) snprintf(out, outLen, "enter");
  else if (key == KEY_TAB) snprintf(out, outLen, "tab");
  else if (key == KEY_ESC) snprintf(out, outLen, "esc");
  else if (key == KEY_BACKSPACE) snprintf(out, outLen, "bksp");
  else if (key == KEY_UP_ARROW) snprintf(out, outLen, "up");
  else if (key == KEY_DOWN_ARROW) snprintf(out, outLen, "down");
  else if (key == KEY_LEFT_ARROW) snprintf(out, outLen, "left");
  else if (key == KEY_RIGHT_ARROW) snprintf(out, outLen, "right");
  else if (key == ' ') snprintf(out, outLen, "space");
  else if (key == KEY_F1) snprintf(out, outLen, "f1");
  else if (key == KEY_F2) snprintf(out, outLen, "f2");
  else if (key == KEY_F3) snprintf(out, outLen, "f3");
  else if (key == KEY_F4) snprintf(out, outLen, "f4");
  else if (key == KEY_F5) snprintf(out, outLen, "f5");
  else if (key == KEY_F6) snprintf(out, outLen, "f6");
  else if (key == KEY_F7) snprintf(out, outLen, "f7");
  else if (key == KEY_F8) snprintf(out, outLen, "f8");
  else if (key == KEY_F9) snprintf(out, outLen, "f9");
  else if (key == KEY_F10) snprintf(out, outLen, "f10");
  else if (key == KEY_F11) snprintf(out, outLen, "f11");
  else if (key == KEY_F12) snprintf(out, outLen, "f12");
  else if (key == 0xA1) snprintf(out, outLen, "vol_up");
  else if (key == 0xA2) snprintf(out, outLen, "vol_down");
  else if (key == 0xA3) snprintf(out, outLen, "mute");
  else if (key == 0xA4) snprintf(out, outLen, "play");
  else if (key == 0xA5) snprintf(out, outLen, "next");
  else if (key == 0xA6) snprintf(out, outLen, "prev");
  else if (key == 0xA7) snprintf(out, outLen, "stop");
  else if (key == 0xA8) snprintf(out, outLen, "www_home");
  else if (key == 0xA9) snprintf(out, outLen, "calc");
  else if (key == 0xAA) snprintf(out, outLen, "search");
  else if (key == 0xAB) snprintf(out, outLen, "back");
  else if (key == 0xAC) snprintf(out, outLen, "email");
  else if (key >= 32 && key < 127) snprintf(out, outLen, "%c", (char)key);
  else snprintf(out, outLen, "");
}

void comboToName(const KeyCombo& c, bool advSide, char* out, size_t outLen) {
  out[0] = '\0';
  size_t pos = 0;
  auto append = [&](const char* s) {
    if (!s || !s[0] || pos + 1 >= outLen) return;
    if (pos) {
      snprintf(out + pos, outLen - pos, "+");
      pos = strlen(out);
    }
    snprintf(out + pos, outLen - pos, "%s", s);
    pos = strlen(out);
  };
  for (uint8_t i = 0; i < c.nmod; i++) {
    if (advSide) append(physKeyName(c.mods[i]));
    else {
      const char* n = hidModName(c.mods[i]);
      if (n[0]) append(n);
    }
  }
  for (uint8_t i = 0; i < c.nkey; i++) {
    if (advSide) append(physKeyName(c.keys[i]));
    else {
      char kn[16];
      hidKeyToName(c.keys[i], kn, sizeof(kn));
      append(kn);
    }
  }
  if (!out[0]) snprintf(out, outLen, "—");
}
