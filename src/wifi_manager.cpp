#include "wifi_manager.h"

#include <WiFi.h>

#include <algorithm>

#include "status.h"

void WifiManager::begin() {
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(false);
  WiFi.disconnect(true);
}

const std::vector<ScanEntry>& WifiManager::scan() {
  _last.clear();
  Status::info("Scanning Wi-Fi...");
  int n = WiFi.scanNetworks(/*async=*/false, /*show_hidden=*/false);
  for (int i = 0; i < n; ++i) {
    _last.push_back({WiFi.SSID(i), WiFi.RSSI(i)});
  }
  WiFi.scanDelete();
  std::sort(_last.begin(), _last.end(),
            [](const ScanEntry& a, const ScanEntry& b) { return a.rssi > b.rssi; });
  Status::info(String("Found ") + _last.size() + " APs");
  return _last;
}

bool WifiManager::isVisible(const String& ssid) const {
  for (const auto& e : _last)
    if (e.ssid == ssid) return true;
  return false;
}

int32_t WifiManager::rssiOf(const String& ssid) const {
  for (const auto& e : _last)
    if (e.ssid == ssid) return e.rssi;
  return 0;
}

bool WifiManager::connect(const SiteConfig& site, uint32_t timeoutMs) {
  Status::info(String("Join ") + site.ssid);
  WiFi.disconnect(true);
  delay(50);
  WiFi.begin(site.ssid.c_str(), site.password.c_str());

  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && (millis() - start) < timeoutMs) {
    float pct = float(millis() - start) / float(timeoutMs);
    Status::progress("link", pct);
    delay(200);
  }
  Status::clearProgress();

  if (WiFi.status() != WL_CONNECTED) {
    Status::info(String("Join failed: ") + site.ssid);
    return false;
  }
  Status::info(String("IP ") + WiFi.localIP().toString());
  return true;
}

void WifiManager::disconnect() {
  WiFi.disconnect(true);
  delay(50);
}

bool WifiManager::connected() const { return WiFi.status() == WL_CONNECTED; }

String WifiManager::ip() const { return WiFi.localIP().toString(); }
