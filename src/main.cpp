#include <Arduino.h>

#include "app_config.h"
#include "discord_client.h"
#include "image_store.h"
#include "net.h"
#include "orchestrator.h"
#include "sd_card.h"
#include "status.h"
#include "status_display.h"
#include "wifi_manager.h"

static StatusDisplay display;
static DiscordClient discord;
static AppConfig config;
static WifiManager wifi;
static Orchestrator orchestrator;

static bool booted = false;

void setup() {
  Serial.begin(115200);
  delay(200);

  display.begin();
  Status::begin(&display, &discord);
  Status::stage(DeviceState::Boot, "DockerGo " DOCKERGO_VERSION);

  if (!sdBegin()) {
    Status::error("No SD card");
    return;  // loop() will idle; nothing to do without config
  }
  Status::info("SD mounted");
  ImageStore::begin();

  String err;
  if (!ConfigLoader::loadFromSd("/config.json", config, err)) {
    Status::error("config: " + err);
    return;
  }
  Status::info(config.deviceName + " sites:" + config.sites.size());

  discord.begin(config.discordWebhook, config.deviceName);
  wifi.begin();
  orchestrator.begin(&config, &wifi);
  booted = true;
}

void loop() {
  if (!booted || config.sites.empty()) {
    delay(3000);
    return;
  }
  orchestrator.runCycle();
  Status::info("Sleep 60s");
  delay(60000);
}
