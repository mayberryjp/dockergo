#pragma once
#include <Arduino.h>

#include "image_ref.h"

namespace DockerClient {
// Upload a cached OCI-layout image (imageDir on SD) to the daemon via
// POST /images/load. Streams a tar directly to the socket (no temp file).
bool loadImage(const String& dockerApi, const String& imageDir, String& err);

// Recreate every container whose image matches `ref` so it runs the newly
// loaded image (stop -> remove -> create with same config -> start). Returns
// count updated, or -1 on API error (message in `err`).
int updateContainersForImage(const String& dockerApi, const ImageRef& ref, String& err);
}  // namespace DockerClient
