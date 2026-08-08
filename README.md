# Cardputer KB

把 M5Stack **Cardputer ADV** 变成可网页配置的 **BLE HID 键盘**：自定义组合键重映射、本机键盘配网、OTA 升级。

蓝牙广播名：**Cardputer KB**

## 快速开始

1. 烧录固件（见下方）
2. 电脑蓝牙搜索并配对 **Cardputer KB**
3. **配网在设备上完成**（无 SoftAP / 无网页 WiFi 页）：
   - 无已存 WiFi 时开机自动进入配网；或按 **Opt+W**
   - 等待扫描 → `;` / `.` 上下选网 → **Enter**
   - 用本机键盘输入密码（开放网直接 Enter）→ **Enter** 连接
   - `` ` ``（Esc）返回 / 取消
4. 连上后屏显 `Web x.x.x.x`，浏览器打开该地址配置**键盘映射 / OTA**
5. 「键盘」页添加映射 → 底部 **保存并应用**（Mac / Windows 两套一起保存）

也可 USB 串口 115200：

```text
wifi 你家SSID 你家密码
wifi clear
wifi ui
wifi scan
```

## 本地保留键

| 组合 | 作用 |
|------|------|
| **Opt+P** | 切换 Mac / Windows 映射表 |
| **Opt+R** | 清除 BLE 绑定并重新广播 |
| **Opt+W** | 清除 WiFi 凭据并进入本机配网 |
| **Aa+`** | 回到 Launcher |

## 构建 / 烧录

```bash
cd <本仓库>
pio run -e cardputer-adv -t upload
```

务必带 `-e cardputer-adv`。

## 说明

- WiFi 仅 **2.4GHz**
- 网页不再提供 WiFi 标签；连网只走本机键盘或串口
- BLE 与 WiFi 共存时勿关闭 WiFi modem sleep
