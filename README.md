# Cardputer Voice — 用 Cardputer ADV 调用电脑上的 Typeless 语音输入

把 M5Stack **Cardputer ADV** 变成一个 **BLE 无线键盘遥控器**：

1. **触发电脑上的 Typeless 语音听写** —— 按一下 `Ctrl` 开始、再按一下停止，对着电脑说话即可语音输入。
2. **遥控 Claude Code** —— 方向键 / Esc / 空格 / 回车 / Backspace / Tab 直接转发；`Opt` 发 `Shift+Tab` 切模式。
3. **选择模式** —— 按 `Alt` 进入后，方向键变成 `Shift+方向` 选中文本，再配合 Typeless 编辑模式语音改写。

语音用的是**电脑自己的麦克风**（Cardputer 只当遥控器）。Typeless 在 Arduino 下无法把 Cardputer 当 USB 麦克风，
所以走这条更可靠的路线。

---

## 按键一览

| Cardputer 键 | 作用 |
|---|---|
| **Ctrl** | 发听写热键（按平台不同）= 开/关听写（点按切换；2s 最小间隔防失步） |
| **Alt** | 选择模式开/关（本地）。开时方向键 → `Shift+方向` |
| **Opt** | 发 `Shift+Tab` = 切换 Claude Code 模式 |
| **方向键** | 转发 ↑↓←→（长按重复；选择模式下加 Shift） |
| **`** (Esc 位) | 转发 `Esc` |
| **Space / Enter / Del / Tab** | 转发 空格 / 回车 / Backspace（长按连删）/ Tab |
| **Fn + Enter** | 发 `Ctrl+Enter` |
| **Fn + P** | 切换平台 **Mac ↔ Windows**（存 NVS，各平台用不同听写组合键） |
| **Fn + ↑ / ↓** | 切换当前平台的听写热键预设（存 NVS，重启保留） |
| **Fn + `** | 返回 bmorcelli Launcher |

> 屏幕显示：BLE 连接状态、电池、听写状态（IDLE/REC）、选择模式（SELECT）、当前平台（MAC/WIN）+ 听写热键。

### 多平台适配（Mac / Windows）

BLE HID 协议无法可靠探测主机系统，所以用一个**记忆在 NVS 的平台开关**（`Fn+P` 切换，或串口 `platform mac|win`）。
设一次就一直生效；在 Mac 和 PC 间换时按一下 `Fn+P` 即可。各平台的听写组合键预设：

| 平台 | 预设（`Fn+↑/↓` 循环） |
|---|---|
| **Mac** | `⌃⌥\`（默认，Typeless）/ `⌃⌥.` / `⌥⌘\` |
| **Windows** | `Win+H`（系统内置语音输入，默认）/ `Ctrl+Alt+\` / `Ctrl+Alt+.` |

---

## 编译与烧录

```bash
# 主固件
pio run -e cardputer-adv -t upload      # ⚠️ 永远带 -e
pio device monitor -b 115200

# Launcher 兼容版（可被 bmorcelli/Launcher 安装）
pio run -e cardputer-adv-launcher
# 产出 .pio/build/cardputer-adv-launcher/firmware.bin
```

USB 口复位后会在 `/dev/cu.usbmodem1101 ↔ 1201` 间跳；脚本里动态取端口：
`P=$(ls /dev/cu.usbmodem* | head -1)`。

> ⚠️ **烧录后屏幕没反应？按一下机身复位键 / 拔插一次 USB（冷启动）即可运行 app。**
> ESP32-S3 用原生 USB-Serial/JTAG，`pio upload` 结束的自动复位（RTS pin）在某些情况下
> （尤其**经 USB Hub** 连接时）会让芯片落进 ROM 下载模式（`boot:0x3 DOWNLOAD`）而不进 app。
> 冷启动时 GPIO0 被内部上拉为高 → 正常进 app。直连电脑 USB 口通常不会有这个问题。
> 串口同理：pyserial 打开端口前必须 `dtr=True; rts=True`，否则也会把板子推进下载模式（见下）。

---

## 电脑端一次性设置

**先选平台**：在 Cardputer 上按 `Fn+P` 把屏幕底部切到 `MAC` 或 `WIN`（设一次即记住）。

### macOS

1. **设置 Typeless 听写热键**：把热键从 `fn` 改录成 **`⌃⌥\`（Control+Option+反斜杠）**，
   模式设为**点按切换（tap / toggle）**。
   - 为什么不用 `fn` / `F13`：`fn` 是键盘硬件键，HID 发不出来；F13 在 Mac 键盘上没有物理键，没法绑定。
   - 想换预设可在 Cardputer 上 `Fn+↑/↓` 或串口 `hotkey <n>` 切换：`0=⌃⌥\`、`1=⌃⌥.`、`2=⌥⌘\`。
2. **蓝牙配对**：系统设置 → 蓝牙 → 配对 **“Cardputer Voice”**。

### Windows

1. **语音输入**：默认预设是 **`Win+H`**（Windows 内置语音输入，无需装软件，按一下打开/说话）。
   若用第三方听写工具，`Fn+↑/↓` 切到 `Ctrl+Alt+\` / `Ctrl+Alt+.` 并在该工具里绑定同样的热键。
2. **蓝牙配对**：设置 → 蓝牙和其他设备 → 添加设备 → 配对 **“Cardputer Voice”**。

### 通用用法
光标放进任意文本框 / Claude Code 输入框 →
- 按 `Ctrl` 开始听写，说话，再按 `Ctrl` 停止；
- 方向键 / Esc / Enter 操作界面；`Opt` 切 Claude Code 模式；`Fn+Enter` 发 Ctrl+Enter；
- `Alt` 进选择模式，用方向键选中文本后再 `Ctrl` 语音改写。

---

## 串口调试命令（115200）

| 命令 | 作用 |
|---|---|
| `status` | 打印 BLE / 平台 / 听写 / 选择 / 热键 / 电量 |
| `platform [mac\|win]` | 切换平台（无参数则翻转），存 NVS |
| `hotkey <0\|1\|2>` | 切换当前平台的听写热键预设并保存 |
| `send` | 手动发一次听写热键（翻转假设状态） |
| `keymap` | 10 秒内打印你在 Cardputer 上按下的键（用于确认方向键字符） |
| `reset` | 假设状态归 IDLE、关闭选择模式 |

> pyserial 连接前先设 `dtr=True; rts=True`，否则可能把板子推进 ROM 下载模式。

---

## 实机要确认的一点：方向键字符

不同批次 Cardputer 键盘的方向键上报字符可能不同。默认映射：`;`=↑ `.`=↓ `,`=← `/`=→。
烧录后串口执行 `keymap`，按一下每个方向键看打印的 `word=[...]`，若与默认不符，改 `src/main.cpp` 顶部的
`ARROW_UP/DOWN/LEFT/RIGHT` 重新烧录即可。

---

## 已知限制

- **听写状态是“假设值”**：单向发键无法读 Typeless 真实状态，若失步用串口 `reset` 或多按一次 `Ctrl` 对齐。
- **Ctrl/Alt/Opt/Fn 被本地占用**，不作为修饰键转发（按需求无需 Ctrl+方向等组合）。
- **构建依赖**：`ESP32-BLE-Keyboard` 需配 `NimBLE-Arduino` **1.4.x**（已在 `platformio.ini` 锁定）；
  若升级到 NimBLE 2.x 会编译失败。
