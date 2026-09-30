#pragma once
#include <Arduino.h>

#include "image_ref.h"

namespace DockerClient {
// Upload a cached OCI-layout image (imageDir on SD) to the daemon via
// POST /images/load. Streams a tar directly to the socket (no temp file).
bool loadImage(const String& dockerApi, const String& imageDir, String& err);

// Deploy a cached OCI-layout image safely (docker-updater pattern): load the new
// image first, then for each container on the old image stop + rename it to
// <name>_old, create + start the replacement on the same config, and on success
// drop the backup. If the replacement fails it is removed and <name>_old is
// renamed back and restarted (rollback). Returns count (re)started, or -1 on
// load/API error (message in `err`).
int deployImage(const String& dockerApi, const ImageRef& ref, const String& imageDir,
                String& err);
}  // namespace DockerClient
