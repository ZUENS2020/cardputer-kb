# Cardputer KB

M5Stack **Cardputer ADV** 全键盘 BLE HID：网页配置组合键重映射、WiFi、OTA。

## 功能

- **BLE 无线键盘**：连上电脑后当 HID 键盘用
- **网页重映射**：ADV 组合 → 电脑组合（Mac / Windows 两套一起保存）
- **自动探测平台**：蓝牙连上后按 Battery / 连接特征猜测 Mac 或 Windows
- **WiFi + OTA**：STA 连家里网，或 AP 配网；网页可扫网、改映射、刷固件
- **未映射键**：可透传或屏蔽

默认**没有** Typeless 预设映射，全部由网页自己配。

## 本地保留键

| 组合 | 作用 |
|---|---|
| **Opt+P** | 临时切换平台 Mac ↔ Windows（存 NVS，本次会话手动覆盖） |
| **Opt+R** | BLE 重配对：清 bond、断开、重新广播 |
| **Opt+W** | 清除 WiFi 凭据并强制开配置 AP |
| **Aa+`** | 返回 bmorcelli Launcher |

## 配网

1. 未连上 WiFi 时自动开 AP：`Cardputer-KB` / `typeless123`
2. 手机连上后打开 [http://192.168.4.1](http://192.168.4.1)
3. 「WiFi」页扫描并加入家里网络；连上后 AP 关闭，用局域网 IP 访问
4. 「键盘」页编辑 Mac / Windows 重映射并保存
5. 「OTA」页上传固件（`.bin`）

重置网络：设备上按 **Opt+W**，再连 `Cardputer-KB`。

## 构建 / 烧录

```bash
pio run -e cardputer-adv -t upload
```

Launcher 兼容分区：

```bash
pio run -e cardputer-adv-launcher -t upload
```

串口：`115200`。USB 口会在 `cu.usbmodem1101 ↔ 1201` 之间跳，动态取端口再烧。

## 硬件

- Cardputer ADV（ESP32-S3FN8，无 PSRAM）
- 仅 BLE（无 Classic 蓝牙）
- 分区：`default_8MB.csv`（双 OTA）

## License

MIT
