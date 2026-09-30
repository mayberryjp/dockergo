#include "status.h"

#include <SD_MMC.h>
#include <esp_system.h>

#include "discord_client.h"

namespace {
StatusDisplay* g_display = nullptr;
DiscordClient* g_discord = nullptr;
bool g_sdLog = false;

// Last-reset cause. Logged in the boot marker because the SD log records only
// Status:: lines, never the panic handler, so a crash leaves no trace here on
// its own -- this is the only way to tell brownout vs watchdog vs panic apart.
const char* resetReasonStr() {
  switch (esp_reset_reason()) {
    case ESP_RST_POWERON: return "POWERON";
    case ESP_RST_EXT: return "EXT";
    case ESP_RST_SW: return "SW";
    case ESP_RST_PANIC: return "PANIC";
    case ESP_RST_INT_WDT: return "INT_WDT";
    case ESP_RST_TASK_WDT: return "TASK_WDT";
    case ESP_RST_WDT: return "WDT";
    case ESP_RST_BROWNOUT: return "BROWNOUT";
    case ESP_RST_DEEPSLEEP: return "DEEPSLEEP";
    case ESP_RST_SDIO: return "SDIO";
    default: return "UNKNOWN";
  }
}

void toSerial(const char* level, const String& s) {
  Serial.printf("[%s] %s\n", level, s.c_str());
}

// Persist each status line (+ heap trend) to SD so an unattended freeze can be
// diagnosed afterward by pulling the card and reading the tail of the log.
void toSd(const char* level, const String& s) {
  if (!g_sdLog) return;
  fs::File f = SD_MMC.open("/dockergo.log", FILE_APPEND);
  if (!f) return;
  f.printf("%lu %s %s | heap=%u max=%u\n", (unsigned long)millis(), level, s.c_str(),
           (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxAllocHeap());
  f.close();
}
}  // namespace

namespace Status {

void begin(StatusDisplay* display, DiscordClient* discord) {
  g_display = display;
  g_discord = discord;
}

void beginSdLog() {
  // Append across boots so an unattended crash isn't erased by the next power-up;
  // the boot marker delimits each session in the growing log.
  fs::File f = SD_MMC.open("/dockergo.log", FILE_APPEND);
  if (f) {
    f.printf("--- boot %lu heap=%u rst=%s ---\n", (unsigned long)millis(),
             (unsigned)ESP.getFreeHeap(), resetReasonStr());
    f.close();
  }
  g_sdLog = true;
}

void stage(DeviceState state, const String& label) {
  toSerial("STAGE", label);
  toSd("STAGE", label);
  if (g_display) {
    g_display->setState(state, label);
    g_display->clearProgress();
    g_display->log(label);
  }
  if (g_discord) g_discord->send("**" + label + "**");
}

void info(const String& line) {
  toSerial("INFO", line);
  toSd("INFO", line);
  if (g_display) g_display->log(line);
}

void event(const String& line) {
  toSerial("EVENT", line);
  toSd("EVENT", line);
  if (g_display) g_display->log(line);
  if (g_discord) g_discord->send(line);
}

void error(const String& line) {
  toSerial("ERROR", line);
  toSd("ERROR", line);
  if (g_display) {
    g_display->setState(DeviceState::Error, line);
    g_display->log("ERR: " + line);
  }
  if (g_discord) g_discord->send(":warning: " + line);
}

void progress(const String& label, float pct) {
  if (g_display) g_display->progress(label, pct);
}

void clearProgress() {
  if (g_display) g_display->clearProgress();
}

}  // namespace Status
