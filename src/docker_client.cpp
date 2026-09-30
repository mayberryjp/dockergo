#include "docker_client.h"

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <SD_MMC.h>
#include <WiFiClient.h>

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
  respBody = http.getString();
  http.end();
  return code;
}

// Recreate one container so it runs `newImage`, preserving its config. The old
// container is inspected first (config kept in RAM), then stop -> remove ->
// create (same name) -> reconnect extra networks -> start.
bool recreateContainer(const String& api, const String& id, const String& newImage,
                       String& err) {
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

  String name = ins["Name"].as<String>();
  if (name.startsWith("/")) name = name.substring(1);

  JsonDocument body;
  body.set(ins["Config"]);
  body["Image"] = newImage;
  body["HostConfig"] = ins["HostConfig"];

  // Only one network may be attached at create time; connect the rest after.
  std::vector<String> extraNets;
  JsonObject nets = ins["NetworkSettings"]["Networks"].as<JsonObject>();
  bool first = true;
  if (!nets.isNull()) {
    for (JsonPair kv : nets) {
      if (first) {
        JsonObject ec =
            body["NetworkingConfig"]["EndpointsConfig"][kv.key()].to<JsonObject>();
        JsonObject src = kv.value().as<JsonObject>();
        if (src["Aliases"].is<JsonArray>()) ec["Aliases"] = src["Aliases"];
        if (src["IPAMConfig"].is<JsonObject>()) ec["IPAMConfig"] = src["IPAMConfig"];
        first = false;
      } else {
        extraNets.push_back(String(kv.key().c_str()));
      }
    }
  }

  String r;
  dockerRequest(api, "POST", "/containers/" + id + "/stop?t=10", "", r);
  int rc = dockerRequest(api, "DELETE", "/containers/" + id + "?force=1", "", r);
  if (rc != 204 && rc != 200) {
    err = String("remove HTTP ") + rc;
    return false;
  }

  String createBody;
  serializeJson(body, createBody);
  String cr;
  int cc = dockerRequest(api, "POST", "/containers/create?name=" + name, createBody, cr);
  if (cc != 201 && cc != 200) {
    err = String("create HTTP ") + cc;
    return false;
  }
  JsonDocument crd;
  deserializeJson(crd, cr);
  String newId = crd["Id"].as<String>();
  if (!newId.length()) {
    err = "create returned no id";
    return false;
  }

  for (auto& net : extraNets) {
    JsonDocument cb;
    cb["Container"] = newId;
    JsonObject src = ins["NetworkSettings"]["Networks"][net].as<JsonObject>();
    if (!src.isNull() && src["Aliases"].is<JsonArray>())
      cb["EndpointConfig"]["Aliases"] = src["Aliases"];
    String cbs, rr;
    serializeJson(cb, cbs);
    dockerRequest(api, "POST", "/networks/" + net + "/connect", cbs, rr);
  }

  int sc = dockerRequest(api, "POST", "/containers/" + newId + "/start", "", r);
  if (sc != 204 && sc != 200) {
    err = String("start HTTP ") + sc;
    return false;
  }
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

  uint8_t hdr[512];
  uint8_t buf[2048];
  const uint8_t zeros[512] = {0};
  size_t sent = 0, lastShown = 0;
  int blobIdx = 0;

  for (auto& e : entries) {
    buildTarHeader(hdr, e.tarName, e.size);
    if (client.write(hdr, 512) != 512) {
      err = "socket write";
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
      if (n <= 0) break;
      client.write(buf, n);
      remaining -= n;
      sent += n;
      if (payloadBytes && (sent - lastShown) >= 65536) {
        lastShown = sent;
        Status::progress("upload", float(sent) / float(payloadBytes));
      }
    }
    f.close();
    size_t pad = (512 - (e.size % 512)) % 512;
    if (pad) client.write(zeros, pad);
    if (e.tarName.startsWith("blobs/sha256/")) {
      ++blobIdx;
      Status::event(String("blob ") + blobIdx + "/" + blobCount + " up " + humanBytes(e.size));
    }
  }
  client.write(zeros, 512);
  client.write(zeros, 512);
  Status::clearProgress();

  String statusLine = client.readStringUntil('\n');
  bool ok = statusLine.indexOf(" 200") >= 0;
  uint32_t t = millis();
  while (client.connected() && millis() - t < 3000) {
    while (client.available()) client.read();
  }
  client.stop();
  if (!ok) err = "load response: " + statusLine;
  return ok;
}

int updateContainersForImage(const String& dockerApi, const ImageRef& ref, String& err) {
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

  // Resolve the freshly-loaded image's ID so we can skip containers already on it.
  String newImageId;
  {
    String imgBody;
    if (dockerRequest(dockerApi, "GET", "/images/" + ref.original + "/json", "", imgBody) == 200) {
      JsonDocument idoc;
      if (!deserializeJson(idoc, imgBody)) newImageId = idoc["Id"].as<String>();
    }
  }

  std::vector<String> cands = matchCandidates(ref);
  int updated = 0;
  for (JsonObject cont : doc.as<JsonArray>()) {
    String img = cont["Image"].as<String>();
    bool match = false;
    for (auto& c : cands)
      if (img == c) {
        match = true;
        break;
      }
    if (!match) continue;

    String id = cont["Id"].as<String>();
    String name = cont["Names"][0].as<String>();
    if (name.startsWith("/")) name = name.substring(1);

    // Idempotent: leave containers already running the loaded image untouched.
    if (newImageId.length() && cont["ImageID"].as<String>() == newImageId) {
      Status::info("up-to-date " + name);
      continue;
    }

    Status::info("recreate " + name);
    String e;
    if (recreateContainer(dockerApi, id, ref.original, e)) {
      ++updated;
      Status::event("Updated " + name);
    } else {
      Status::error("update " + name + ": " + e);
    }
  }
  return updated;
}

}  // namespace DockerClient
