#include "orchestrator.h"

#include <algorithm>

#include "docker_client.h"
#include "image_ref.h"
#include "image_store.h"
#include "registry_client.h"
#include "status.h"
#include "summary_client.h"

void Orchestrator::begin(AppConfig* cfg, WifiManager* wifi) {
  _cfg = cfg;
  _wifi = wifi;
}

void Orchestrator::runCycle() {
  Status::stage(DeviceState::Scanning, "Scanning");
  _wifi->scan();

  const SiteConfig* home = _cfg->homeSite();

  // Phase 1: at home (good bandwidth) download pending images to SD.
  if (home && _wifi->isVisible(home->ssid)) {
    doHome(*home);
  } else if (home) {
    Status::info("Home AP not seen");
  }

  // Phase 2: at any visible remote site, apply cached images.
  for (const auto& site : _cfg->sites) {
    if (home && &site == home) continue;
    if (site.home) continue;
    if (_wifi->isVisible(site.ssid)) doRemote(site);
  }

  Status::stage(DeviceState::Idle, "Idle");
}

void Orchestrator::doHome(const SiteConfig& home) {
  Status::stage(DeviceState::ConnectingHome, "HOME " + home.name);
  if (!_wifi->connect(home)) {
    Status::error("HOME connect failed");
    return;
  }
  Status::stage(DeviceState::Home, home.name);

  // Each site reports its own needs; download the union while bandwidth is good.
  std::vector<String> wanted;
  for (const auto& site : _cfg->sites) {
    SummaryResult s = SummaryClient::fetch(site.summaryUrl);
    if (!s.ok) {
      Status::info(site.name + " sum: " + s.error);
      continue;
    }
    Status::info(site.name + " needs " + String(s.pendingImages.size()));
    for (const auto& img : s.pendingImages) {
      if (std::find(wanted.begin(), wanted.end(), img) == wanted.end())
        wanted.push_back(img);
    }
  }
  Status::event(String("Wanted total: ") + wanted.size());

  int got = 0, have = 0, fail = 0;
  const String platform = _cfg->imagePlatform;
  for (const auto& imgStr : wanted) {
    ImageRef ref;
    if (!parseImageRef(imgStr, ref)) {
      Status::info("bad ref " + imgStr);
      continue;
    }
    if (ImageStore::isComplete(ref)) {
      ++have;
      Status::info("cached " + ref.shortName());
      continue;
    }
    Status::stage(DeviceState::Working, "DL " + ref.shortName());
    PullResult pr = RegistryClient::pull(ref, platform);
    if (pr.ok) {
      ImageStore::markComplete(ref);
      ++got;
      Status::event("Downloaded " + ref.original);
    } else {
      ++fail;
      Status::error("DL " + ref.shortName() + ": " + pr.error);
    }
  }
  Status::event(String("HOME: +") + got + " have " + have + " fail " + fail);
  _wifi->disconnect();
}

void Orchestrator::doRemote(const SiteConfig& site) {
  Status::stage(DeviceState::ConnectingRemote, "SITE " + site.name);
  if (!_wifi->connect(site)) {
    Status::error("connect failed " + site.name);
    return;
  }
  Status::stage(DeviceState::Remote, site.name);

  SummaryResult sum = SummaryClient::fetch(site.summaryUrl);
  if (!sum.ok) {
    Status::error("summary: " + sum.error);
    _wifi->disconnect();
    return;
  }

  int applied = 0, missing = 0, fail = 0;
  for (const auto& imgStr : sum.pendingImages) {
    ImageRef ref;
    if (!parseImageRef(imgStr, ref)) continue;
    if (!ImageStore::isComplete(ref)) {
      ++missing;
      Status::info("not cached " + ref.shortName());
      continue;
    }
    Status::stage(DeviceState::Working, "UP " + ref.shortName());
    String err;
    if (!DockerClient::loadImage(site.dockerApi, ImageStore::dirFor(ref), err)) {
      ++fail;
      Status::error("load " + ref.shortName() + ": " + err);
      continue;
    }
    Status::event("Loaded " + ref.original + " @ " + site.name);
    int n = DockerClient::updateContainersForImage(site.dockerApi, ref, err);
    if (n < 0)
      Status::error("update: " + err);
    else if (n == 0)
      Status::info("no container for " + ref.shortName());
    ++applied;
  }
  Status::event(site.name + ": applied " + applied + " miss " + missing + " fail " + fail);
  _wifi->disconnect();
}
