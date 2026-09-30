#include "docker_client.h"

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <SD_MMC.h>
#include <WiFiClient.h>

#include <utility>
#include <vector>

#include "image_store.h"
#include "net.h"
#include "status.h"

namespace {

struct TarEntry {
  String tarName;
  String path;
  size_t size;
};

bool parseHostPort(const String& api, String& host, uint16_t& port) {
  String s = api;
  int p = s.indexOf("://");
  if (p >= 0) s = s.substring(p + 3);
  int slash = s.indexOf('/');
  if (slash >= 0) s = s.substring(0, slash);
  int colon = s.indexOf(':');
  if (colon >= 0) {
    host = s.substring(0, colon);
    port = (uint16_t)s.substring(colon + 1).toInt();
  } else {
    host = s;
    port = 2375;
  }
  return host.length() > 0;
}

void octalField(uint8_t* dst, int len, uint64_t v) {
  for (int i = len - 2; i >= 0; --i) {
    dst[i] = '0' + (uint8_t)(v & 7);
    v >>= 3;
  }
  dst[len - 1] = '\0';
}

void buildTarHeader(uint8_t hdr[512], const String& name, size_t size) {
  memset(hdr, 0, 512);
  strncpy((char*)hdr, name.c_str(), 100);
  memcpy(hdr + 100, "0000644", 8);  // mode
  memcpy(hdr + 108, "0000000", 8);  // uid
  memcpy(hdr + 116, "0000000", 8);  // gid
  octalField(hdr + 124, 12, size);  // size
  octalField(hdr + 136, 12, 0);     // mtime
  memset(hdr + 148, ' ', 8);        // checksum placeholder
  hdr[156] = '0';                   // regular file
  memcpy(hdr + 257, "ustar", 5);    // magic (NUL already at 262)
  hdr[263] = '0';
  hdr[264] = '0';  // version "00"

  uint32_t sum = 0;
  for (int i = 0; i < 512; ++i) sum += hdr[i];
  char chk[8];
  snprintf(chk, sizeof(chk), "%06o", (unsigned)(sum & 0777777));
  memcpy(hdr + 148, chk, 6);
  hdr[154] = '\0';
  hdr[155] = ' ';
}

// Compact human-readable byte count for status/milestone lines.
String humanBytes(size_t n) {
  char b[16];
  if (n >= 1024 * 1024)
    snprintf(b, sizeof(b), "%.1fMB", n / 1048576.0);
  else if (n >= 1024)
    snprintf(b, sizeof(b), "%uKB", (unsigned)(n / 1024));
  else
    snprintf(b, sizeof(b), "%uB", (unsigned)n);
  return String(b);
}

bool collectEntries(const String& imageDir, std::vector<TarEntry>& out) {
  auto add = [&](const String& tarName, const String& path) {
    File f = SD_MMC.open(path, FILE_READ);
    if (!f) return false;
    out.push_back({tarName, path, (size_t)f.size()});
    f.close();
    return true;
  };
  if (!add("oci-layout", imageDir + "/oci-layout")) return false;
  if (!add("index.json", imageDir + "/index.json")) return false;
  if (!add("manifest.json", imageDir + "/manifest.json")) return false;

  // Blobs live in the shared store; the refs file names exactly the ones this
  // image uses, sourced from CAS but tarred under the OCI blobs/sha256 path.
  if (File rf = SD_MMC.open(imageDir + "/refs", FILE_READ)) {
    while (rf.available()) {
      String hex = rf.readStringUntil('\n');
      hex.trim();
      if (!hex.length()) continue;
      if (!add("blobs/sha256/" + hex, ImageStore::blobPath(hex))) {
        rf.close();
        return false;
      }
    }
    rf.close();
    return true;
  }

  // Legacy layout (pre-CAS): blobs stored inside the image dir.
  File dir = SD_MMC.open(imageDir + "/blobs/sha256");
  if (!dir) return false;
  for (File e = dir.openNextFile(); e; e = dir.openNextFile()) {
    if (!e.isDirectory()) {
      String nm = e.name();
      String base = nm.substring(nm.lastIndexOf('/') + 1);
      out.push_back({"blobs/sha256/" + base, imageDir + "/blobs/sha256/" + base, (size_t)e.size()});
    }
    e.close();
  }
  dir.close();
  return true;
}

std::vector<String> matchCandidates(const ImageRef& ref) {
  std::vector<String> c;
  c.push_back(ref.original);
  c.push_back(ref.registryHost + "/" + ref.repo + ":" + ref.tag);
  if (ref.registryHost == "registry-1.docker.io") {
    String r = ref.repo;
    if (r.startsWith("library/")) r = r.substring(8);
    c.push_back(r + ":" + ref.tag);
    c.push_back("docker.io/" + ref.repo + ":" + ref.tag);
  }
  return c;
}

int dockerRequest(const String& api, const char* method, const String& path,
                  const String& reqBody, String& respBody) {
  WiFiClient client;
  HTTPClient http;
  if (!http.begin(client, api + path)) return -1;
  http.addHeader("User-Agent", DOCKERGO_UA);
  if (reqBody.length()) http.addHeader("Content-Type", "application/json");
  http.setConnectTimeout(10000);
  http.setTimeout(20000);
  int code = http.sendRequest(method, (uint8_t*)reqBody.c_str(), reqBody.length());
  // 204/304 carry no body and no Content-Length; getString() would then block on
  // the keep-alive socket waiting for bytes that never arrive (stop/rename/start/
  // delete all return 204). Only read a body when the response actually has one.
  if (code > 0 && code != 204 && code != 304) respBody = http.getString();
  http.end();
  return code;
}

// A container captured before its old image is removed, holding everything
// needed to recreate it on the new image afterwards.
struct SavedContainer {
  String id;          // original container id (to stop/remove)
  String name;        // recreate under the same name
  String createBody;  // /containers/create body, Image already set to the new ref
  std::vector<std::pair<String, String>> extraNets;  // net name -> EndpointConfig json
};

// Inspect a container and capture its create body (Config + HostConfig + first
// network) plus any extra networks, so it can be recreated on `newImage` after
// the old image is removed. Does not modify the container.
bool captureContainer(const String& api, const String& id, const String& newImage,
                      SavedContainer& out, String& err) {
  String insBody;
  int c = dockerRequest(api, "GET", "/containers/" + id + "/json", "", insBody);
  if (c != 200) {
    err = String("inspect HTTP ") + c;
    return false;
  }
  JsonDocument ins;
  if (deserializeJson(ins, insBody)) {
    err = "inspect parse";
    return false;
  }

  out.id = id;
  out.name = ins["Name"].as<String>();
  if (out.name.startsWith("/")) out.name = out.name.substring(1);

  JsonObject cfg = ins["Config"].as<JsonObject>();
  JsonObject hcfg = ins["HostConfig"].as<JsonObject>();

  // Reconstruct a clean create body from the fields /containers/create accepts;
  // replaying the raw inspect Config/HostConfig makes the daemon reject the
  // create (which then silently rolls back to the old image).
  JsonDocument body;
  body["Image"] = newImage;
  if (cfg["User"].as<String>().length()) body["User"] = cfg["User"];
  if (cfg["WorkingDir"].as<String>().length()) body["WorkingDir"] = cfg["WorkingDir"];
  if (!cfg["Env"].isNull()) body["Env"] = cfg["Env"];
  if (!cfg["Cmd"].isNull()) body["Cmd"] = cfg["Cmd"];
  if (!cfg["Entrypoint"].isNull()) body["Entrypoint"] = cfg["Entrypoint"];
  if (!cfg["Labels"].isNull()) body["Labels"] = cfg["Labels"];
  if (!cfg["ExposedPorts"].isNull()) body["ExposedPorts"] = cfg["ExposedPorts"];
  if (!cfg["Volumes"].isNull()) body["Volumes"] = cfg["Volumes"];

  // Drop the auto-assigned 12-hex hostname so the daemon assigns a fresh one.
  String hn = cfg["Hostname"].as<String>();
  bool autoHn = hn.length() == 12;
  for (size_t i = 0; autoHn && i < hn.length(); ++i)
    if (!isxdigit((int)hn[i])) autoHn = false;
  if (hn.length() && !autoHn) body["Hostname"] = hn;

  JsonObject hc = body["HostConfig"].to<JsonObject>();
  if (!hcfg["Binds"].isNull()) hc["Binds"] = hcfg["Binds"];
  if (!hcfg["Mounts"].isNull()) hc["Mounts"] = hcfg["Mounts"];
  if (!hcfg["PortBindings"].isNull()) hc["PortBindings"] = hcfg["PortBindings"];
  if (!hcfg["RestartPolicy"].isNull()) hc["RestartPolicy"] = hcfg["RestartPolicy"];
  if (hcfg["NetworkMode"].as<String>().length()) hc["NetworkMode"] = hcfg["NetworkMode"];
  if (!hcfg["CapAdd"].isNull()) hc["CapAdd"] = hcfg["CapAdd"];
  if (!hcfg["CapDrop"].isNull()) hc["CapDrop"] = hcfg["CapDrop"];
  if (hcfg["Privileged"].as<bool>()) hc["Privileged"] = true;
  if (!hcfg["SecurityOpt"].isNull()) hc["SecurityOpt"] = hcfg["SecurityOpt"];
  if (!hcfg["Devices"].isNull()) hc["Devices"] = hcfg["Devices"];
  if (!hcfg["ExtraHosts"].isNull()) hc["ExtraHosts"] = hcfg["ExtraHosts"];
  if (!hcfg["Dns"].isNull()) hc["Dns"] = hcfg["Dns"];
  if (!hcfg["DnsSearch"].isNull()) hc["DnsSearch"] = hcfg["DnsSearch"];
  if (!hcfg["VolumesFrom"].isNull()) hc["VolumesFrom"] = hcfg["VolumesFrom"];
  if (hcfg["PidMode"].as<String>().length()) hc["PidMode"] = hcfg["PidMode"];
  if (hcfg["IpcMode"].as<String>().length()) hc["IpcMode"] = hcfg["IpcMode"];
  if (!hcfg["Tmpfs"].isNull()) hc["Tmpfs"] = hcfg["Tmpfs"];

  // Only one network may be attached at create time; the rest are reconnected
  // after the container exists.
  JsonObject nets = ins["NetworkSettings"]["Networks"].as<JsonObject>();
  bool first = true;
  if (!nets.isNull()) {
    for (JsonPair kv : nets) {
      JsonObject src = kv.value().as<JsonObject>();
      if (first) {
        JsonObject ec =
            body["NetworkingConfig"]["EndpointsConfig"][kv.key()].to<JsonObject>();
        if (src["Aliases"].is<JsonArray>()) ec["Aliases"] = src["Aliases"];
        if (src["IPAMConfig"].is<JsonObject>()) ec["IPAMConfig"] = src["IPAMConfig"];
        first = false;
      } else {
        JsonDocument ep;
        if (src["Aliases"].is<JsonArray>()) ep["Aliases"] = src["Aliases"];
        String eps;
        serializeJson(ep, eps);
        out.extraNets.push_back({String(kv.key().c_str()), eps});
      }
    }
  }
  serializeJson(body, out.createBody);
  return true;
}

}  // namespace

namespace DockerClient {

bool loadImage(const String& dockerApi, const String& imageDir, String& err) {
  String host;
  uint16_t port;
  if (!parseHostPort(dockerApi, host, port)) {
    err = "bad docker_api";
    return false;
  }

  std::vector<TarEntry> entries;
  if (!collectEntries(imageDir, entries)) {
    err = "image files missing";
    return false;
  }

  size_t contentLength = 1024;  // trailing two zero blocks
  size_t payloadBytes = 0;
  int blobCount = 0;
  for (auto& e : entries) {
    size_t pad = (512 - (e.size % 512)) % 512;
    contentLength += 512 + e.size + pad;
    payloadBytes += e.size;
    if (e.tarName.startsWith("blobs/sha256/")) ++blobCount;
  }

  WiFiClient client;
  client.setTimeout(15000);
  if (!client.connect(host.c_str(), port)) {
    err = "connect " + host;
    return false;
  }

  client.printf("POST /images/load?quiet=1 HTTP/1.1\r\n");
  client.printf("Host: %s:%u\r\n", host.c_str(), port);
  client.print("User-Agent: " DOCKERGO_UA "\r\n");
  client.print("Content-Type: application/x-tar\r\n");
  client.printf("Content-Length: %u\r\n", (unsigned)contentLength);
  client.print("Connection: close\r\n\r\n");

  Status::event(String("upload ") + blobCount + " blobs " + humanBytes(payloadBytes));

  // Static, not stack: loadImage runs deep in the call chain and Status::event
  // below reaches Discord's mbedTLS handshake (~6KB stack). Keeping these 3KB of
  // buffers on the 8KB loopTask stack overflowed it -> PANIC. Safe: single loopTask.
  static uint8_t hdr[512];
  static uint8_t buf[2048];
  static const uint8_t zeros[512] = {0};
  size_t sent = 0, lastShown = 0;
  int blobIdx = 0;

  // WiFiClient::write can accept fewer bytes than asked under TCP backpressure;
  // loop until every byte is flushed so the tar stream matches the sizes declared
  // in the headers. A short write here would misalign the whole archive.
  auto writeFully = [&](const uint8_t* p, size_t n) -> bool {
    size_t off = 0;
    while (off < n) {
      int w = client.write(p + off, n - off);
      if (w <= 0) return false;
      off += (size_t)w;
    }
    return true;
  };

  for (auto& e : entries) {
    buildTarHeader(hdr, e.tarName, e.size);
    if (!writeFully(hdr, 512)) {
      err = "tar header write";
      client.stop();
      return false;
    }
    File f = SD_MMC.open(e.path, FILE_READ);
    if (!f) {
      err = "reopen " + e.path;
      client.stop();
      return false;
    }
    size_t remaining = e.size;
    while (remaining) {
      size_t toRead = remaining < sizeof(buf) ? remaining : sizeof(buf);
      int n = f.read(buf, toRead);
      if (n <= 0) {  // short/failed read would truncate the entry and corrupt the tar
        f.close();
        err = "short read " + e.tarName;
        client.stop();
        return false;
      }
      if (!writeFully(buf, n)) {
        f.close();
        err = "blob write " + e.tarName;
        client.stop();
        return false;
      }
      remaining -= n;
      sent += n;
      if (payloadBytes && (sent - lastShown) >= 65536) {
        lastShown = sent;
        Status::progress("upload", float(sent) / float(payloadBytes));
      }
    }
    f.close();
    size_t pad = (512 - (e.size % 512)) % 512;
    if (pad && !writeFully(zeros, pad)) {
      err = "pad write";
      client.stop();
      return false;
    }
    if (e.tarName.startsWith("blobs/sha256/")) {
      ++blobIdx;
      Status::event(String("blob ") + blobIdx + "/" + blobCount + " up " + humanBytes(e.size));
    }
  }
  if (!writeFully(zeros, 512) || !writeFully(zeros, 512)) {
    err = "tar tail write";
    client.stop();
    return false;
  }
  Status::clearProgress();

  String statusLine = client.readStringUntil('\n');
  bool httpOk = statusLine.indexOf(" 200") >= 0;
  // /images/load streams its real result ("Loaded image ID: sha256:…" or an
  // errorDetail) in the BODY with HTTP 200; the status line alone can't tell a
  // real load from a silently-rejected tar. Docker closes the socket right after
  // replying, so drain buffered bytes even once connected() goes false.
  String resp;
  uint32_t t = millis();
  while (millis() - t < 5000) {
    if (client.available()) {
      char ch = client.read();
      if (resp.length() < 800) resp += ch;
      t = millis();
    } else if (!client.connected()) {
      break;
    }
  }
  client.stop();
  Status::info("load resp: " + resp);
  if (!httpOk) {
    err = "load HTTP: " + statusLine;
    return false;
  }
  if (resp.indexOf("error") >= 0) {
    err = "load: " + resp;
    return false;
  }
  return true;
}

// This daemon's /images/load requires the legacy `docker save` manifest.json;
// an OCI index alone is rejected ("does not contain a manifest.json"). Synthesize
// it from the cached image manifest blob so cached and freshly-pulled images load.
static bool ensureLegacyManifest(const ImageRef& ref, const String& imageDir, String& err) {
  if (SD_MMC.exists(imageDir + "/manifest.json")) return true;

  File rf = SD_MMC.open(imageDir + "/refs", FILE_READ);
  if (!rf) { err = "no refs"; return false; }
  String manHex = rf.readStringUntil('\n');
  manHex.trim();
  rf.close();
  if (!manHex.length()) { err = "empty refs"; return false; }

  File mf = SD_MMC.open(ImageStore::blobPath(manHex), FILE_READ);
  if (!mf) { err = "no manifest blob"; return false; }
  String mbody = mf.readString();
  mf.close();

  JsonDocument md;
  if (deserializeJson(md, mbody)) { err = "manifest blob parse"; return false; }
  String cfg = md["config"]["digest"].as<String>();
  JsonArray layers = md["layers"].as<JsonArray>();
  if (!cfg.length() || layers.isNull()) { err = "manifest blob fields"; return false; }
  if (cfg.startsWith("sha256:")) cfg = cfg.substring(7);

  String repoTag;  // Docker's familiar name so the tag lands on the same ref
  if (ref.registryHost == "registry-1.docker.io") {
    String r = ref.repo;
    if (r.startsWith("library/")) r = r.substring(8);
    repoTag = r + ":" + ref.tag;
  } else {
    repoTag = ref.registryHost + "/" + ref.repo + ":" + ref.tag;
  }

  JsonDocument out;
  JsonObject e = out.add<JsonObject>();
  e["Config"] = "blobs/sha256/" + cfg;
  e["RepoTags"].to<JsonArray>().add(repoTag);
  JsonArray la = e["Layers"].to<JsonArray>();
  for (JsonObject l : layers) {
    String d = l["digest"].as<String>();
    if (d.startsWith("sha256:")) d = d.substring(7);
    la.add("blobs/sha256/" + d);
  }
  File of = SD_MMC.open(imageDir + "/manifest.json", FILE_WRITE);
  if (!of) { err = "write manifest.json"; return false; }
  serializeJson(out, of);
  of.close();
  return true;
}

int deployImage(const String& dockerApi, const ImageRef& ref, const String& imageDir,
                String& err) {
  // Load the new image FIRST: nothing is touched until the transfer succeeds, so
  // a failed download can never take a running container down.
  if (!ensureLegacyManifest(ref, imageDir, err)) return -1;

  // Capture target containers BEFORE loading: loadImage moves the tag off the old
  // image, after which /containers/json reports the container by bare id and the
  // tag-based match can't find it. Capture is read-only, so nothing is mutated
  // until the transfer below succeeds.
  String listBody;
  int code = dockerRequest(dockerApi, "GET", "/containers/json?all=1", "", listBody);
  if (code != 200) {
    err = String("list HTTP ") + code;
    return -1;
  }
  JsonDocument doc;
  if (deserializeJson(doc, listBody)) {
    err = "list parse";
    return -1;
  }

  std::vector<String> cands = matchCandidates(ref);
  std::vector<SavedContainer> targets;
  for (JsonObject cont : doc.as<JsonArray>()) {
    String img = cont["Image"].as<String>();
    bool match = false;
    for (auto& c : cands)
      if (img == c) {
        match = true;
        break;
      }
    if (!match) continue;

    SavedContainer scn;
    String cerr;
    if (!captureContainer(dockerApi, cont["Id"].as<String>(), ref.original, scn, cerr)) {
      Status::error("capture: " + cerr);
      continue;
    }
    targets.push_back(scn);
  }

  if (!loadImage(dockerApi, imageDir, err)) return -1;

  int started = 0;
  for (auto& scn : targets) {
    // Stop the old container and park it as <name>_old (a rollback target) rather
    // than deleting it; clear any stale backup from an interrupted run first.
    String r;
    String backup = scn.name + "_old";
    dockerRequest(dockerApi, "POST", "/containers/" + scn.id + "/stop?t=10", "", r);
    dockerRequest(dockerApi, "DELETE", "/containers/" + backup + "?force=1", "", r);
    int rn = dockerRequest(dockerApi, "POST",
                           "/containers/" + scn.id + "/rename?name=" + backup, "", r);
    if (rn != 204 && rn != 200) {
      Status::error("rename " + scn.name + " HTTP " + rn);
      dockerRequest(dockerApi, "POST", "/containers/" + scn.id + "/start", "", r);
      continue;
    }

    String fail, newId, cr;
    int cc = dockerRequest(dockerApi, "POST", "/containers/create?name=" + scn.name,
                           scn.createBody, cr);
    if (cc == 201 || cc == 200) {
      JsonDocument crd;
      deserializeJson(crd, cr);
      newId = crd["Id"].as<String>();
      if (!newId.length()) fail = "create no id";
    } else {
      fail = String("create HTTP ") + cc;
    }

    if (!fail.length()) {
      for (auto& en : scn.extraNets) {
        JsonDocument cb;
        cb["Container"] = newId;
        JsonDocument ep;
        deserializeJson(ep, en.second);
        cb["EndpointConfig"] = ep;
        String cbs, rr;
        serializeJson(cb, cbs);
        dockerRequest(dockerApi, "POST", "/networks/" + en.first + "/connect", cbs, rr);
      }
      int scode = dockerRequest(dockerApi, "POST", "/containers/" + newId + "/start", "", r);
      if (scode != 204 && scode != 200) fail = String("start HTTP ") + scode;
    }

    if (!fail.length()) {
      // New container is up: discard the parked old one.
      dockerRequest(dockerApi, "DELETE", "/containers/" + scn.id + "?force=1", "", r);
      Status::event("Updated " + scn.name);
      ++started;
    } else {
      // Roll back: drop the failed new container, restore the old one, restart it.
      if (newId.length())
        dockerRequest(dockerApi, "DELETE", "/containers/" + newId + "?force=1", "", r);
      dockerRequest(dockerApi, "POST", "/containers/" + scn.id + "/rename?name=" + scn.name, "", r);
      dockerRequest(dockerApi, "POST", "/containers/" + scn.id + "/start", "", r);
      Status::error("rollback " + scn.name + ": " + fail);
    }
  }
  return started;
}

}  // namespace DockerClient
