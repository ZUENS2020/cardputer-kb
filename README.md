# Cardputer Voice — 用 Cardputer ADV 调用电脑上的 Typeless 语音输入

把 M5Stack **Cardputer ADV** 变成一个 **BLE 无线键盘遥控器**：

1. **触发电脑上的 Typeless 语音听写** —— 按一下 `Ctrl` 开始、再按一下停止，对着电脑说话即可语音输入。
2. **遥控 Claude Code** —— 方向键 / Esc / 空格 / Backspace 直接转发，Enter 键发 `Ctrl+Enter`，单点 Fn 发 Enter；`Opt` 发 `Shift+Tab` 切模式。
3. **选择模式** —— 按 `Alt` 进入后，方向键变成 `Shift+方向` 选中文本，再配合 Typeless 编辑模式语音改写。

语音用的是**电脑自己的麦克风**（Cardputer 只当遥控器）。Typeless 在 Arduino 下无法把 Cardputer 当 USB 麦克风，
所以走这条更可靠的路线。

---

## 按键一览

| Cardputer 键 | 作用 |
|---|---|
| **Ctrl** | 发听写热键（按平台不同）= 开/关听写（点按切换；单独按下 + 2s 间隔防失步） |
| **Tab** | Typeless **另一种输入**，与 Ctrl 同样处理（单独按下 + 2s 间隔 + 状态）。Mac=`Ctrl+Alt+'`，Win=`Right Alt+Space` |
| **Alt** | 选择模式开/关（本地）。开时方向键 → `Shift+方向` |
| **Opt** | 发 `Shift+Tab` = 切换 Claude Code 模式 |
| **方向键** | 转发 ↑↓←→（长按重复；选择模式下加 Shift） |
| **`** (Esc 位) | 转发 `Esc`；若在 REC 中则同步退出本地 REC 指示 |
| **Space / Del** | 转发 空格 / Backspace（长按连删） |
| **Enter 键** | 发 `Ctrl+Enter` |
| **A 键** | 全选（Mac=`⌘A`，Win=`Ctrl+A`） |
| **Fn（单独点按）** | 发 `Enter` |
| **Aa + P**（Aa=Shift） | 切换平台 **Mac ↔ Windows**（存 NVS） |
| **Aa + R** | **重新配对**：清除所有 bond + 断开 + 重新广播 |
| **Aa + `** | 返回 bmorcelli Launcher |

> 听写热键固定用各平台默认预设（Mac `⌃⌥\` / Win `Right Alt`）；如需更换用串口 `hotkey <n>`。

> 屏幕显示：BLE 连接状态、电池、听写状态（IDLE/REC）、选择模式（SELECT）、当前平台（MAC/WIN）+ 听写热键。

### 多平台适配（Mac / Windows）

BLE HID 协议无法可靠探测主机系统，所以用一个**记忆在 NVS 的平台开关**（`Aa+P` 切换，或串口 `platform mac|win`）。
设一次就一直生效；在 Mac 和 PC 间换时按一下 `Aa+P` 即可。各平台的听写组合键预设：

| 平台 | 默认听写键 | 其它可选（串口 `hotkey <n>`） |
|---|---|---|
| **Mac** | `⌃⌥\` | `1=⌃⌥.`、`2=⌥⌘\` |
| **Windows** | `Right Alt` | `1=Ctrl+Alt+\`、`2=Ctrl+Shift+\` |

**Tab（Typeless 第二输入）的平台组合**：Mac=`Ctrl+Alt+'`，Windows=`Right Alt+Space`（在 Typeless 里把第二输入绑成对应组合）。

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

**先选平台**：在 Cardputer 上按 `Aa+P` 把屏幕底部切到 `MAC` 或 `WIN`（设一次即记住）。

### macOS

1. **设置 Typeless 听写热键**：把热键从 `fn` 改录成 **`⌃⌥\`（Control+Option+反斜杠）**，
   模式设为**点按切换（tap / toggle）**。
   - 为什么不用 `fn` / `F13`：`fn` 是键盘硬件键，HID 发不出来；F13 在 Mac 键盘上没有物理键，没法绑定。
   - 想换预设用串口 `hotkey <n>` 切换：`0=⌃⌥\`、`1=⌃⌥.`、`2=⌥⌘\`。
2. **蓝牙配对**：系统设置 → 蓝牙 → 配对 **“Cardputer Voice”**。

### Windows

1. **Typeless 听写热键**：默认是**单独 `Right Alt`**，在 Typeless 里把听写热键绑成 Right Alt、模式设为点按切换。
   想用组合键改用串口 `hotkey 1`(=Ctrl+Alt+\) 或 `hotkey 2`(=Ctrl+Shift+\) 并在 Typeless 里改绑。
2. **蓝牙配对**：设置 → 蓝牙和其他设备 → 添加设备 → 配对 **“Cardputer Voice”**。

### 通用用法
光标放进任意文本框 / Claude Code 输入框 →
- 按 `Ctrl` 开始听写，说话，再按 `Ctrl` 停止；
- 方向键 / Esc 操作界面；`Opt` 切 Claude Code 模式；**Enter 键发 Ctrl+Enter**，**单点 Fn 发 Enter**；
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
| `repair` | 重新配对：清 bond + 断开 + 重新广播（等同 `Aa+R`） |

> **重新配对怎么用**：按 `Aa+R`（屏幕显示 `RE-PAIRING`）→ 到主机蓝牙设置里**删除/忽略 “Cardputer Voice”** → 重新搜索配对。
> 换电脑、配对卡住、或换固件后主机 GATT 缓存出问题时用它。

> pyserial 连接前先设 `dtr=True; rts=True`，否则可能把板子推进 ROM 下载模式。

---

## 已知限制

- **听写状态是“假设值”**：单向发键无法读 Typeless 真实状态，若失步用串口 `reset` 或多按一次 `Ctrl` 对齐。
- **Ctrl/Alt/Opt/Fn/Aa(Shift) 被本地占用**，不作为修饰键转发（按需求无需 Ctrl+方向等组合）。
- **构建依赖**：`ESP32-BLE-Keyboard` 需配 `NimBLE-Arduino` **1.4.x**（已在 `platformio.ini` 锁定）；
  若升级到 NimBLE 2.x 会编译失败。
