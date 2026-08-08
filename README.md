# Cardputer KB

把 M5Stack **Cardputer ADV** 变成可网页配置的 **BLE HID 键盘**：自定义组合键重映射、WiFi 配网、OTA 升级。

蓝牙广播名：**Cardputer KB**

## 快速开始

1. 烧录固件（见下方）
2. 电脑蓝牙搜索并配对 **Cardputer KB**
3. 未连上家里 WiFi 时，设备会开配置热点：
   - SSID：`Cardputer-KB`
   - 密码：`typeless123`
   - 打开 [http://192.168.4.1](http://192.168.4.1)
4. 在网页「WiFi」页扫描并加入网络（**只能从扫描列表选，不能手输 SSID**）
5. 连上后热点关闭；屏幕显示局域网 IP，用 `http://<IP>` 继续配置
6. 「键盘」页添加映射 → 底部 **保存并应用**（Mac / Windows 两套一起保存）

重置 WiFi：设备上按 **Opt+W**，再连回 `Cardputer-KB`。

## 本地保留键

这些组合不参与重映射，固件本地处理：

| 组合 | 作用 |
|---|---|
| **Opt+P** | 手动切换 Mac ↔ Windows，写入 NVS；并在本轮 BLE 连接内禁止自动探测覆盖 |
| **Opt+R** | 清除 BLE bond、断开、重新广播（主机需忘掉旧设备后重配对） |
| **Opt+W** | 删除已存 WiFi 凭据，强制开配置 AP |
| **Aa+`** | 返回 bmorcelli Launcher（需用 launcher 分区烧录才有入口） |

屏幕在已连 WiFi 时底部会提示：`Opt+P/R/W  Aa+\` launcher`。

## 网页重映射

默认映射表为空（无任何出厂预设）。

流程：

1. 顶部切换 **编辑 Mac** / **编辑 Windows**（两套表；保存时一起写入）
2. 选择 **电脑将收到** 的组合 → **下一步：选 ADV**
3. 在弹层里选 **ADV 上的触发组合** → **加入映射表**
4. 底部 **保存并应用**

规则：

- 无修饰键：只能选 **1** 个主键
- 有修饰键：主键个数不限（也可只发修饰键）；HID 侧修饰最多 4、主键最多 6
- 冲突判定：仅当 **整段 ADV 触发组合完全相同** 才冲突
- 模式：
  - **仅映射键**：未出现在任何触发里的物理键不发出
  - **全键透传**：未映射的键按原样发给电脑

蓝牙连上后会按主机特征（Battery 订阅、连接间隔、MTU、地址类型）**自动猜测** Mac / Windows，并切换当前使用的映射表。猜错用 **Opt+P**，或在网页改完后保存。

## OTA

「OTA」页上传 PlatformIO 产出的 `firmware.bin`（例如 `.pio/build/cardputer-adv/firmware.bin`）。建议在已连 STA 的稳定网络下升级。

## 构建 / 烧录

```bash
# 标准双 OTA 分区（推荐，支持网页 OTA）
pio run -e cardputer-adv -t upload

# bmorcelli Launcher 兼容分区（Aa+` 回 Launcher）
pio run -e cardputer-adv-launcher -t upload
```

- 串口波特率 `115200`
- ESP32-S3 原生 USB 复位后端口常在 `cu.usbmodem1101` ↔ `1201` 间跳；不确定时：

```bash
P=$(ls /dev/cu.usbmodem* | head -1)
pio run -e cardputer-adv -t upload --upload-port "$P"
```

### 串口命令

| 命令 | 作用 |
|---|---|
| `status` | BLE / 平台 / 映射数 / WiFi / 电量 |
| `platform` / `platform mac` / `platform win` | 手动切平台 |
| `keymap` | 10 秒内串口打印物理按键调试信息 |
| `repair` | 同 Opt+R |
| `clear` | 清空**当前平台**映射并保存 |

## 硬件与限制

- 板子：Cardputer ADV（ESP32-S3FN8，**无 PSRAM**）
- 只有 BLE，没有 Classic 蓝牙
- 分区：`default_8MB.csv`（双 OTA，各约 3.3MB）；Launcher 版用 `partitions_launcher.csv`
- BLE 与 WiFi 共存：启动顺序为先 BLE 再 WiFi；不要对 STA 关 WiFi sleep（会与 BLE 冲突）

## 仓库

https://github.com/ZUENS2020/cardputer-kb
