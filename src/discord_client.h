#pragma once
#include <Arduino.h>

// Best-effort Discord webhook notifier. Never blocks the main flow for long:
// posts are skipped when Wi-Fi is down and use short timeouts otherwise.
class DiscordClient {
 public:
  void begin(const String& webhookUrl, const String& username);
  bool enabled() const { return _webhook.length() > 0; }
  void send(const String& content);

 private:
  String _webhook;
  String _username;
};
