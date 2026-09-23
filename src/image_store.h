#pragma once
#include <Arduino.h>

#include "image_ref.h"

// SD-card cache of downloaded images. Layout per image (see SPEC §6.2):
//   /images/<safeId>/  manifest.json, config.json, <diffid>/layer.tar, .complete
namespace ImageStore {
bool begin();
String dirFor(const ImageRef& ref);
bool isComplete(const ImageRef& ref);
bool markComplete(const ImageRef& ref);
bool removeImage(const ImageRef& ref);
bool ensureDir(const String& path);
}  // namespace ImageStore
