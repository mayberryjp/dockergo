#pragma once
#include <Arduino.h>

#include "image_ref.h"

struct PullResult {
  bool ok = false;
  String error;
  String imageDir;  // /images/<safeId> on success
  int layerCount = 0;
};

namespace RegistryClient {
// All registry traffic routes through this HTTP proxy ("http://host:port"); the
// device speaks plain HTTP and the proxy terminates TLS to the real registry.
void setProxy(const String& proxyBase);
// Pull an image into the SD OCI-layout cache (see SPEC §6). `platform` is like
// "linux/amd64" and selects an entry from a multi-arch manifest index.
PullResult pull(const ImageRef& ref, const String& platform);
// Resolve the tag's current image-manifest digest (bare hex) for `platform`,
// matching what pull() stores, so a mutable tag can be checked for changes
// without downloading blobs.
bool resolveDigest(const ImageRef& ref, const String& platform, String& outHex, String& err);
}  // namespace RegistryClient
