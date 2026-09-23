#pragma once
#include "app_config.h"
#include "wifi_manager.h"

// Drives one full update cycle: download at home, then apply at any visible
// remote sites (see SPEC §5).
class Orchestrator {
 public:
  void begin(AppConfig* cfg, WifiManager* wifi);
  void runCycle();

 private:
  AppConfig* _cfg = nullptr;
  WifiManager* _wifi = nullptr;

  void doHome(const SiteConfig& home);
  void doRemote(const SiteConfig& site);
};
