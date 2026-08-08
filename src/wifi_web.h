#pragma once

#include <Arduino.h>

struct WifiScanHit {
  char ssid[33];
  int32_t rssi;
};

void wifiWebBegin();
void wifiWebLoop();
bool wifiWebConnected();
const char* wifiWebIpString();
const char* wifiWebSsid();

// 清凭据并断开（不再开 SoftAP）
void wifiWebResetNetwork();
bool wifiWebConnectHome(const char* ssid, const char* pass);

// Opt+W 配网：先暂存当前网，失败/取消再回退
void wifiWebStashCurrent();
bool wifiWebHasStash();
const char* wifiWebStashSsid();
bool wifiWebRestoreStash();  // 回退并重连；无暂存则断开。返回是否恢复了旧网
void wifiWebClearStash();

// 设备端配网：纯 STA 同步扫网
int wifiScanNetworks(WifiScanHit* out, int maxOut);
void wifiWebDiagScan();
