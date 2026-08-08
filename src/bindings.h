#pragma once

#include <stddef.h>
#include <stdint.h>

// 物理键编码：1..127 = ASCII；0x80+ = 功能键
enum PhysKey : uint8_t {
  PK_CTRL  = 0x80,
  PK_ALT   = 0x81,
  PK_OPT   = 0x82,
  PK_FN    = 0x83,
  PK_TAB   = 0x84,
  PK_ENTER = 0x85,
  PK_SPACE = 0x86,
  PK_DEL   = 0x87,
  PK_ESC   = 0x88,
  PK_UP    = 0x89,
  PK_DOWN  = 0x8A,
  PK_LEFT  = 0x8B,
  PK_RIGHT = 0x8C,
  PK_SHIFT = 0x8D,  // Aa
};

// 组合：有修饰键时可带多枚主键；无修饰键时只能 1 枚主键
struct KeyCombo {
  uint8_t nmod;
  uint8_t mods[4];
  uint8_t nkey;
  uint8_t keys[6];
};

struct RemapEntry {
  uint8_t used;
  KeyCombo adv;  // ADV 触发（物理键码 / PK_*）
  KeyCombo pc;   // 发给电脑的 HID（修饰键 + 键码）
};

static constexpr uint8_t MAX_REMAPS = 32;

void remapsLoad();
void remapsSavePlatform(uint8_t platform);
void remapsSaveBoth();
void remapsClearPlatform(uint8_t platform);

bool remapsPassThrough();
void remapsSetPassThrough(bool on);

// 根据当前 BLE 主机特征探测 Mac(0)/Windows(1)；不落盘记忆
uint8_t platformDetectHost();
bool platformApplyDetected(uint8_t* platformInOut);

uint8_t remapCount(uint8_t platform);
const RemapEntry* remapAt(uint8_t platform, uint8_t index);
bool remapAdd(uint8_t platform, const KeyCombo& adv, const KeyCombo& pc);
void remapRemoveAt(uint8_t platform, uint8_t index);
bool remapAdvConflict(uint8_t platform, const KeyCombo& adv, int ignoreIndex);

// 当前 ADV 按键状态（供组合匹配）
struct AdvPress {
  bool ctrl, alt, opt, fn, shift;
  bool tab, enter, space, del, esc;
  bool ascii[128];
};

bool comboMatchAdv(const KeyCombo& adv, const AdvPress& s);
bool comboEqual(const KeyCombo& a, const KeyCombo& b);
bool physUsedInAnyTrigger(uint8_t platform, uint8_t phys);

void comboToName(const KeyCombo& c, bool advSide, char* out, size_t outLen);
const char* physKeyName(uint8_t phys);
uint8_t physKeyFromName(const char* name);
uint8_t hidModFromName(const char* name);
const char* hidModName(uint8_t mod);
uint8_t hidKeyFromName(const char* name);
void hidKeyToName(uint8_t key, char* out, size_t outLen);

// 媒体键编码进 keys[]：0xA1..0xAF（不与 ASCII / F 键冲突）
bool hidIsMedia(uint8_t key);
bool hidMediaReportBytes(uint8_t key, uint8_t out[2]);
