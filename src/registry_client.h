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
// Pull an image into the SD OCI-layout cache (see SPEC §6). `platform` is like
// "linux/amd64" and selects an entry from a multi-arch manifest index.
PullResult pull(const ImageRef& ref, const String& platform);
}  // namespace RegistryClient
