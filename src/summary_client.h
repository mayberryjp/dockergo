#pragma once
#include <Arduino.h>

#include <vector>

struct SummaryResult {
  bool ok = false;
  int pendingUpdates = 0;
  std::vector<String> pendingImages;
  String error;
};

namespace SummaryClient {
// GET the site's summary endpoint and extract checks.docker_updater.pending_images.
SummaryResult fetch(const String& url);
}  // namespace SummaryClient
