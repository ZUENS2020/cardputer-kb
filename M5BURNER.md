# M5Burner 发布信息 / Publish Info

烧录文件：`cardputer-typeless-v1.0-merged.bin`（合并镜像，烧录地址 **0x0**）
Firmware file: `cardputer-typeless-v1.0-merged.bin` (merged image, flash at **0x0**)

设备 / Device: **M5Stack Cardputer ADV (ESP32-S3 / StampS3A)**
名称 / Name: **Cardputer Voice**
版本 / Version: **1.0**

---

## 简介（中文）

把 Cardputer ADV 变成一个 **BLE 无线键盘遥控器**：一键触发电脑上的 **Typeless** 语音听写，并能遥控 **Claude Code**。
语音走电脑自己的麦克风，Cardputer 只当遥控器，自动适配 Mac / Windows。

**按键**
- `Ctrl` 听写开/关 · `Tab` Typeless 第二输入
- `A` 全选(Mac ⌘A / Win Ctrl+A) · `Enter` 发 Ctrl+Enter · `Fn` 单点发 Enter
- `Opt` 发 Shift+Tab(切 Claude Code 模式) · `Alt` 选择模式(方向键变 Shift+方向)
- 方向键 / Esc / 空格 / 退格 直接转发
- `Aa+P` 切 Mac/Windows · `Aa+R` 重新配对 · `Aa+\`` 返回 Launcher

**用法**：蓝牙配对 “Cardputer Voice” → 在 Typeless 里把听写热键绑成对应平台默认键(Mac `⌃⌥\`、Win `Right Alt`)、模式设为点按切换 → 光标放文本框即可语音输入。烧录后按一下复位键启动。

源码 / Source: https://github.com/ZUENS2020/cardputer-typeless

---

## Description (English)

Turn the Cardputer ADV into a **BLE wireless keyboard remote**: one-tap trigger for **Typeless** voice dictation on your computer, plus wireless control of **Claude Code**. Audio uses the computer's own mic; the Cardputer is just the remote. Auto-adapts to Mac / Windows.

**Keys**
- `Ctrl` dictation on/off · `Tab` Typeless 2nd input
- `A` Select All (Mac ⌘A / Win Ctrl+A) · `Enter` sends Ctrl+Enter · `Fn` tap = Enter
- `Opt` Shift+Tab (Claude Code mode) · `Alt` selection mode (arrows → Shift+arrows)
- Arrows / Esc / Space / Backspace forwarded
- `Aa+P` switch Mac/Windows · `Aa+R` re-pair · `Aa+\`` launcher

**Setup**: pair Bluetooth "Cardputer Voice" → set the Typeless hotkey to the per-platform default (Mac `⌃⌥\`, Win `Right Alt`), tap-toggle mode → put the cursor in any text field and dictate. Press reset after flashing to boot.

Source: https://github.com/ZUENS2020/cardputer-typeless

---

## 发布步骤 / How to publish on M5Burner

1. 打开 M5Burner，用 M5Stack 账号登录 / Open M5Burner, log in with an M5Stack account.
2. 进入 “Burner Share” / Publish，新建固件 / Create a new firmware entry.
3. 上传 `cardputer-typeless-v1.0-merged.bin`，烧录地址填 `0x0` / Upload the merged bin, set flash address `0x0`.
4. 设备类别选 Cardputer / StampS3；填上面的名称、版本、说明 / Pick the Cardputer category; fill name/version/description above.
5. 加封面图后提交发布 / Add a cover image and submit.
