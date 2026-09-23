#pragma once
#include <Arduino.h>

#include <vector>

struct SiteConfig {
  String name;
  bool home = false;
  String ssid;
  String password;
  String dockerApi;   // e.g. http://192.168.1.10:2375
  String summaryUrl;  // e.g. http://host:10000/summary
  String platform;    // optional per-site override of image_platform
};

struct AppConfig {
  String deviceName = "dockergo";
  String imagePlatform = "linux/amd64";
  String discordWebhook;
  std::vector<SiteConfig> sites;

  // Site flagged home:true, else the first site, else nullptr.
  const SiteConfig* homeSite() const;
  // Effective platform for a site (per-site override or global default).
  String platformFor(const SiteConfig& s) const;
};

namespace ConfigLoader {
// Reads and validates a JSON config from the SD card. Returns false on error
// with a human-readable reason in `err`.
bool loadFromSd(const char* path, AppConfig& out, String& err);
}  // namespace ConfigLoader
