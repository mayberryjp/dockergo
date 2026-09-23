#pragma once
#include <Arduino.h>

#include "status_display.h"

class DiscordClient;

// Central status reporter. Fans out to the LCD (verbose) and Discord (milestones).
//   stage()  -> LCD header  + Discord
//   info()   -> LCD log only
//   event()  -> LCD log     + Discord
//   error()  -> LCD (error) + Discord
namespace Status {
void begin(StatusDisplay* display, DiscordClient* discord);
void stage(DeviceState state, const String& label);
void info(const String& line);
void event(const String& line);
void error(const String& line);
void progress(const String& label, float pct);
void clearProgress();
}  // namespace Status
