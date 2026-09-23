#include "discord_client.h"

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>

#include "net.h"

void DiscordClient::begin(const String& webhookUrl, const String& username) {
  _webhook = webhookUrl;
  _username = username.length() ? username : String("DockerGo");
}

void DiscordClient::send(const String& content) {
  if (!enabled() || WiFi.status() != WL_CONNECTED) return;

  WiFiClientSecure client;
  configureTls(client);
  client.setTimeout(4000);

  HTTPClient http;
  if (!http.begin(client, _webhook)) return;
  http.setConnectTimeout(4000);
  http.setTimeout(4000);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("User-Agent", DOCKERGO_UA);

  JsonDocument doc;
  doc["username"] = _username;
  doc["content"] = content;
  String body;
  serializeJson(doc, body);

  http.POST(body);
  http.end();
}
