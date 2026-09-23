#pragma once
#include <WiFiClientSecure.h>

#define DOCKERGO_VERSION "0.1.0"
#define DOCKERGO_UA "dockergo/" DOCKERGO_VERSION " (/u/homelabids)"

// Central place to configure TLS trust for outbound HTTPS (registries, Discord).
//
// Default: skip certificate-chain validation (setInsecure). For registry pulls,
// content integrity is independently guaranteed because every blob is verified
// against its sha256 digest from the manifest (see registry_client). To harden
// transport trust, attach the ESP32 CA bundle here in one place.
inline void configureTls(WiFiClientSecure& client) {
  client.setInsecure();
}
