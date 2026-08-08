#pragma once

void wifiWebBegin();
void wifiWebLoop();
bool wifiWebConnected();
const char* wifiWebIpString();
bool wifiWebApMode();
const char* wifiWebApSsid();
void wifiWebResetNetwork();  // 清 WiFi 凭据并强制开 AP
