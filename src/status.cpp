#include "status.h"

#include "discord_client.h"

namespace {
StatusDisplay* g_display = nullptr;
DiscordClient* g_discord = nullptr;

void toSerial(const char* level, const String& s) {
  Serial.printf("[%s] %s\n", level, s.c_str());
}
}  // namespace

namespace Status {

void begin(StatusDisplay* display, DiscordClient* discord) {
  g_display = display;
  g_discord = discord;
}

void stage(DeviceState state, const String& label) {
  toSerial("STAGE", label);
  if (g_display) {
    g_display->setState(state, label);
    g_display->clearProgress();
    g_display->log(label);
  }
  if (g_discord) g_discord->send("**" + label + "**");
}

void info(const String& line) {
  toSerial("INFO", line);
  if (g_display) g_display->log(line);
}

void event(const String& line) {
  toSerial("EVENT", line);
  if (g_display) g_display->log(line);
  if (g_discord) g_discord->send(line);
}

void error(const String& line) {
  toSerial("ERROR", line);
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
