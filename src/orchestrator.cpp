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
  RegistryClient::setProxy(cfg->proxy);
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
    if (_wifi->isVisible(site.ssid)) {
      Status::info(site.name + " visible " + _wifi->rssiOf(site.ssid) + "dBm");
      doRemote(site);
    } else {
      Status::info(site.name + " not seen: " + site.ssid);
    }
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

  // Each remote site reports its own needs; download the union while bandwidth
  // is good. Home is skipped: it has ample bandwidth and pulls images directly.
  std::vector<String> wanted;
  for (const auto& site : _cfg->sites) {
    if (&site == &home) continue;
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
      bool fresh = true;  // assume cached copy is current
      if (!ref.digest.length()) {  // mutable tag: verify it hasn't moved upstream
        String cur, derr;
        if (RegistryClient::resolveDigest(ref, platform, cur, derr))
          fresh = cur == ImageStore::cachedDigest(ref);
        else
          Status::info("check " + ref.shortName() + ": " + derr);  // keep cache on error
      }
      if (fresh) {
        ++have;
        Status::info("cached " + ref.shortName());
        continue;
      }
      Status::info("changed " + ref.shortName());
    }
    Status::stage(DeviceState::Working, ref.original);
    PullResult pr = RegistryClient::pull(ref, platform);
    if (pr.ok) {
      ImageStore::markComplete(ref);
      ++got;
      Status::event("FINISHED " + ref.original + " (" + pr.layerCount + " layers)");
    } else {
      ++fail;
      Status::error(ref.original + ": " + pr.error);
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
  Status::info("api " + site.dockerApi);

  SummaryResult sum = SummaryClient::fetch(site.summaryUrl);
  if (!sum.ok) {
    Status::error("summary: " + sum.error);
    _wifi->disconnect();
    return;
  }
  Status::info(String("pending ") + sum.pendingImages.size());

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
    int n = DockerClient::deployImage(site.dockerApi, ref, ImageStore::dirFor(ref), err);
    if (n < 0) {
      ++fail;
      Status::error("deploy " + ref.shortName() + ": " + err);
      continue;
    }
    Status::event("Loaded " + ref.original + " @ " + site.name);
    ++applied;
  }
  Status::event(site.name + ": applied " + applied + " miss " + missing + " fail " + fail);
  _wifi->disconnect();
}
