#include "summary_client.h"

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>

#include "net.h"

namespace SummaryClient {

SummaryResult fetch(const String& url) {
  SummaryResult r;

  WiFiClient plain;
  WiFiClientSecure secure;
  HTTPClient http;
  const bool https = url.startsWith("https");
  bool started = https ? (configureTls(secure), http.begin(secure, url))
                       : http.begin(plain, url);
  if (!started) {
    r.error = "begin failed";
    return r;
  }
  http.addHeader("User-Agent", DOCKERGO_UA);
  http.setConnectTimeout(8000);
  http.setTimeout(8000);

  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    r.error = String("HTTP ") + code;
    http.end();
    return r;
  }

  JsonDocument doc;
  DeserializationError jerr = deserializeJson(doc, http.getStream());
  http.end();
  if (jerr) {
    r.error = String("json: ") + jerr.c_str();
    return r;
  }

  JsonObject du = doc["checks"]["docker_updater"].as<JsonObject>();
  r.pendingUpdates = du["pending_updates"] | 0;
  for (JsonVariant v : du["pending_images"].as<JsonArray>()) {
    String img = v.as<String>();
    if (img.length()) r.pendingImages.push_back(img);
  }
  r.ok = true;
  return r;
}

}  // namespace SummaryClient
