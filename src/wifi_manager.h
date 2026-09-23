#pragma once
#include <Arduino.h>

#include <vector>

#include "app_config.h"

struct ScanEntry {
  String ssid;
  int32_t rssi;
};

class WifiManager {
 public:
  void begin();
  // Scan for APs; result is cached and sorted by RSSI (strongest first).
  const std::vector<ScanEntry>& scan();
  bool isVisible(const String& ssid) const;
  int32_t rssiOf(const String& ssid) const;  // 0 if not visible

  bool connect(const SiteConfig& site, uint32_t timeoutMs = 20000);
  void disconnect();
  bool connected() const;
  String ip() const;

 private:
  std::vector<ScanEntry> _last;
};
