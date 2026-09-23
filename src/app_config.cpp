#include "app_config.h"

#include <ArduinoJson.h>
#include <SD_MMC.h>

const SiteConfig* AppConfig::homeSite() const {
  for (const auto& s : sites)
    if (s.home) return &s;
  return sites.empty() ? nullptr : &sites.front();
}

String AppConfig::platformFor(const SiteConfig& s) const {
  return s.platform.length() ? s.platform : imagePlatform;
}

namespace ConfigLoader {

bool loadFromSd(const char* path, AppConfig& out, String& err) {
  File f = SD_MMC.open(path, FILE_READ);
  if (!f) {
    err = String("cannot open ") + path;
    return false;
  }

  JsonDocument doc;
  DeserializationError jerr = deserializeJson(doc, f);
  f.close();
  if (jerr) {
    err = String("json parse: ") + jerr.c_str();
    return false;
  }

  out = AppConfig{};
  if (doc["device_name"].is<const char*>()) out.deviceName = doc["device_name"].as<String>();
  if (doc["image_platform"].is<const char*>()) out.imagePlatform = doc["image_platform"].as<String>();
  if (doc["discord_webhook"].is<const char*>()) out.discordWebhook = doc["discord_webhook"].as<String>();

  JsonArray sites = doc["sites"].as<JsonArray>();
  if (sites.isNull() || sites.size() == 0) {
    err = "config has no sites";
    return false;
  }

  for (JsonObject s : sites) {
    SiteConfig site;
    site.name = s["name"].as<String>();
    site.home = s["home"] | false;
    site.ssid = s["ssid"].as<String>();
    site.password = s["password"].as<String>();
    site.dockerApi = s["docker_api"].as<String>();
    site.summaryUrl = s["summary_url"].as<String>();
    if (s["platform"].is<const char*>()) site.platform = s["platform"].as<String>();

    if (site.ssid.length() == 0) {
      err = String("site '") + site.name + "' missing ssid";
      return false;
    }
    out.sites.push_back(site);
  }

  return true;
}

}  // namespace ConfigLoader
