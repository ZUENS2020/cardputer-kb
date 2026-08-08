# Cardputer KB

Turn an M5Stack **Cardputer ADV** into a web-configurable **BLE HID keyboard**: combo remaps, on-device WiFi setup, OTA.

BLE name: **Cardputer KB**

[中文说明](#中文) · [English](#english)

---

## English

### Quick start

1. Flash firmware (below)
2. Pair **Cardputer KB** over Bluetooth
3. **WiFi is configured on the device** (no SoftAP / no web WiFi tab):
   - Auto-enters setup when no saved WiFi; or press **Opt+W**
   - Scan → `;` / `.` to move → **Enter** to select
   - Type password on the Cardputer (open network: Enter) → **Enter** to connect
   - `` ` `` (Esc) cancel / back (failed connect restores previous WiFi)
4. When connected, screen shows `Web x.x.x.x` — open it for **Keys / OTA**
5. Add remaps → **Save & apply** (Mac + Windows saved together). Header chip toggles **中文 / EN**.

Serial (115200):

```text
wifi YourSSID YourPassword
wifi clear
wifi ui
wifi scan
```

### Local shortcuts

| Combo | Action |
|------|------|
| **Opt+P** | Toggle Mac / Windows map table |
| **Opt+R** | Clear BLE bonds and re-advertise |
| **Opt+W** | Open on-device WiFi setup (keeps current link; stash/restore on fail) |
| **Aa+`** | Return to Launcher |

### Build / flash

```bash
cd <repo>
pio run -e cardputer-adv -t upload
```

Always use `-e cardputer-adv`.

### Notes

- WiFi is **2.4 GHz only**
- Do not disable WiFi modem sleep while BLE is active
- Source: https://github.com/ZUENS2020/cardputer-kb

### Share on M5 community

Typical path used by other Cardputer projects:

1. Post in [community.m5stack.com](https://community.m5stack.com/) (Projects / Cardputer) with GitHub + features
2. Optionally upload a `.bin` to **M5Burner** so users can one-click flash
3. Link firmware from GitHub Releases for Launcher / PlatformIO users

---

## 中文

### 快速开始

1. 烧录固件（见下方）
2. 电脑蓝牙搜索并配对 **Cardputer KB**
3. **配网在设备上完成**（无 SoftAP / 无网页 WiFi 页）：
   - 无已存 WiFi 时开机自动进入配网；或按 **Opt+W**
   - 等待扫描 → `;` / `.` 上下选网 → **Enter**
   - 用本机键盘输入密码（开放网直接 Enter）→ **Enter** 连接
   - `` ` ``（Esc）返回 / 取消（连网失败会回退上一套 WiFi）
4. 连上后屏显 `Web x.x.x.x`，浏览器打开该地址配置**键盘映射 / OTA**
5. 「键盘」页添加映射 → 底部 **保存并应用**（Mac / Windows 两套一起保存）。右上角可切换 **中文 / EN**。

也可 USB 串口 115200：

```text
wifi 你家SSID 你家密码
wifi clear
wifi ui
wifi scan
```

### 本地保留键

| 组合 | 作用 |
|------|------|
| **Opt+P** | 切换 Mac / Windows 映射表 |
| **Opt+R** | 清除 BLE 绑定并重新广播 |
| **Opt+W** | 进入本机配网（不立刻断网；失败回退） |
| **Aa+`** | 回到 Launcher |

### 构建 / 烧录

```bash
cd <本仓库>
pio run -e cardputer-adv -t upload
```

务必带 `-e cardputer-adv`。

### 说明

- WiFi 仅 **2.4GHz**
- 网页不再提供 WiFi 标签；连网只走本机键盘或串口
- BLE 与 WiFi 共存时勿关闭 WiFi modem sleep
- 仓库：https://github.com/ZUENS2020/cardputer-kb
