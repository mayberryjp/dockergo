#include "registry_client.h"

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <SD_MMC.h>
#include <WiFi.h>
#include <WiFiClient.h>

#include <vector>

#include "image_store.h"
#include "mbedtls/sha256.h"
#include "net.h"
#include "status.h"

namespace {

const char* kManifestAccept =
    "application/vnd.oci.image.index.v1+json,"
    "application/vnd.docker.distribution.manifest.list.v2+json,"
    "application/vnd.oci.image.manifest.v1+json,"
    "application/vnd.docker.distribution.manifest.v2+json";

String toHex(const uint8_t* d, size_t n) {
  static const char* H = "0123456789abcdef";
  String o;
  o.reserve(n * 2);
  for (size_t i = 0; i < n; ++i) {
    o += H[d[i] >> 4];
    o += H[d[i] & 0xf];
  }
  return o;
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

String sha256Hex(const uint8_t* d, size_t n) {
  uint8_t out[32];
  mbedtls_sha256_context c;
  mbedtls_sha256_init(&c);
  mbedtls_sha256_starts(&c, 0);
  mbedtls_sha256_update(&c, d, n);
  mbedtls_sha256_finish(&c, out);
  mbedtls_sha256_free(&c);
  return toHex(out, 32);
}

// Registry digests arrive as "sha256:<hex>"; the store keys blobs by bare hex.
String stripDigest(const String& d) {
  return d.startsWith("sha256:") ? d.substring(7) : d;
}

String urlEncode(const String& s) {
  String o;
  char buf[4];
  for (unsigned i = 0; i < s.length(); ++i) {
    char c = s.charAt(i);
    if (isalnum((unsigned char)c) || c == '-' || c == '_' || c == '.' || c == '~') {
      o += c;
    } else {
      snprintf(buf, sizeof(buf), "%%%02X", (uint8_t)c);
      o += buf;
    }
  }
  return o;
}

String extractAuthParam(const String& header, const String& key) {
  int i = header.indexOf(key + "=\"");
  if (i < 0) return "";
  i += key.length() + 2;
  int j = header.indexOf('"', i);
  if (j < 0) return "";
  return header.substring(i, j);
}

// --- HTTP proxy ---
// The device speaks only plain HTTP to the proxy; the real upstream host travels
// in X-Upstream and the proxy terminates TLS to the registry / signed CDN.
String s_proxy;  // e.g. "http://192.168.1.10:8080"

void splitUrl(const String& url, String& host, String& path) {
  int h = url.indexOf("://");
  h = (h < 0) ? 0 : h + 3;
  int p = url.indexOf('/', h);
  if (p < 0) {
    host = url.substring(h);
    path = "/";
  } else {
    host = url.substring(h, p);
    path = url.substring(p);
  }
}

bool beginProxied(HTTPClient& http, WiFiClient& client, const String& host, const String& path) {
  if (!s_proxy.length()) return false;
  if (!http.begin(client, s_proxy + path)) return false;
  http.addHeader("X-Upstream", host);
  http.addHeader("User-Agent", DOCKERGO_UA);
  return true;
}

// Stream sink: writes incoming bytes to an SD file while hashing them, and
// throttles an LCD progress bar against a known expected size.
class HashingFileSink : public Stream {
 public:
  HashingFileSink(File f, size_t expected, const String& caption)
      : _f(f), _expected(expected), _caption(caption) {
    mbedtls_sha256_init(&_ctx);
    mbedtls_sha256_starts(&_ctx, 0);
    _lastMs = millis();
  }
  size_t write(uint8_t b) override { return write(&b, 1); }
  size_t write(const uint8_t* buf, size_t n) override {
    mbedtls_sha256_update(&_ctx, buf, n);
    size_t w = _f.write(buf, n);
    _total += w;
    // Refresh on a time cadence, not per-chunk, so the bar + rate keep moving
    // through a slow multi-MB layer and a stall visibly drops the rate to 0.
    uint32_t now = millis();
    if (now - _lastMs >= 400) report(now);
    return w;
  }
  int available() override { return 0; }
  int read() override { return -1; }
  int peek() override { return -1; }

  String hexDigest() {
    uint8_t out[32];
    mbedtls_sha256_finish(&_ctx, out);
    mbedtls_sha256_free(&_ctx);
    return toHex(out, 32);
  }
  size_t total() const { return _total; }

 private:
  void report(uint32_t now) {
    uint32_t dt = now - _lastMs;
    float kbps = dt ? float(_total - _lastBytes) * 1000.0f / 1024.0f / float(dt) : 0.0f;
    _lastMs = now;
    _lastBytes = _total;
    // Lead with the layer id so the bar always names what's downloading, then live
    // throughput + RSSI so a slow pull can be pinned on signal at a glance.
    char line[32];
    if (kbps >= 1024.0f)
      snprintf(line, sizeof(line), "%s %.1fM/s %ddB", _caption.c_str(), kbps / 1024.0f,
               int(WiFi.RSSI()));
    else
      snprintf(line, sizeof(line), "%s %dK/s %ddB", _caption.c_str(), int(kbps + 0.5f),
               int(WiFi.RSSI()));
    float pct = _expected ? float(_total) / float(_expected) : 0.0f;
    Status::progress(line, pct);
  }

  File _f;
  size_t _expected;
  String _caption;
  size_t _total = 0;
  uint32_t _lastMs = 0;
  size_t _lastBytes = 0;
  mbedtls_sha256_context _ctx;
};

bool httpGetToSink(const String& host, const String& path, const String& bearer,
                   HashingFileSink& sink, String& err, int depth = 0) {
  if (depth > 5) {
    err = "too many redirects";
    return false;
  }
  WiFiClient client;
  HTTPClient http;
  if (!beginProxied(http, client, host, path)) {
    err = "begin failed";
    return false;
  }
  http.addHeader("Accept", "*/*");
  if (bearer.length()) http.addHeader("Authorization", "Bearer " + bearer);
  const char* keys[] = {"Location"};
  http.collectHeaders(keys, 1);
  http.setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS);
  http.setConnectTimeout(10000);
  http.setTimeout(15000);

  int code = http.GET();
  if (code == HTTP_CODE_OK) {
    int r = http.writeToStream(&sink);
    http.end();
    if (r < 0) {
      err = "stream error";
      return false;
    }
    return true;
  }
  if (code == 301 || code == 302 || code == 303 || code == 307 || code == 308) {
    String loc = http.header("Location");
    http.end();
    if (!loc.length()) {
      err = "redirect without Location";
      return false;
    }
    // Caddy relays the upstream redirect; re-drive it through the proxy. Absolute
    // Location -> new upstream host; relative -> same host. Drop bearer (signed URL).
    String nhost = host, npath;
    if (loc.startsWith("/"))
      npath = loc;
    else
      splitUrl(loc, nhost, npath);
    return httpGetToSink(nhost, npath, "", sink, err, depth + 1);
  }
  err = String("HTTP ") + code + " @ " + host;
  http.end();
  return false;
}

bool getAuthToken(const ImageRef& ref, String& token, String& err);

// Download a blob into the shared content-addressed store and verify its digest.
// Transient failures (dropped TLS stream, read timeout, truncated body) and an
// expired anonymous token (HTTP 401 mid-pull) are retried a few times so a single
// blip no longer aborts a multi-GB image. On 401 the token is re-minted and written
// back through `bearer`, so every later layer carries the fresh one. Completed blobs
// are skipped up front, so a retry only re-fetches the one layer that failed.
bool downloadBlob(const ImageRef& ref, const String& digest, size_t size,
                  String& bearer, const String& label, const String& caption,
                  String& err) {
  String want = stripDigest(digest);
  String dest = ImageStore::blobPath(want);

  // The final path exists only after a blob is fully written AND digest-verified,
  // so its presence is a trustworthy "already have it". The store is shared across
  // images, so a layer any image already pulled is reused instead of re-downloaded.
  if (SD_MMC.exists(dest)) return true;

  String path = "/v2/" + ref.repo + "/blobs/" + digest;

  const int kMaxAttempts = 4;
  for (int attempt = 1; attempt <= kMaxAttempts; ++attempt) {
    // Stream to a temp file; an interrupted pull leaves <digest>.part, never a
    // short file masquerading as a complete blob on the next run.
    String tmp = dest + ".part";
    File f = SD_MMC.open(tmp, FILE_WRITE);
    if (!f) {
      err = "open " + tmp;
      return false;
    }
    HashingFileSink sink(f, size, caption);
    bool ok = httpGetToSink(ref.registryHost, path, bearer, sink, err);
    String got = sink.hexDigest();
    f.close();
    Status::clearProgress();

    if (ok && got.equalsIgnoreCase(want)) {
      if (!SD_MMC.rename(tmp, dest)) {
        SD_MMC.remove(tmp);
        err = "rename " + label;
        return false;
      }
      return true;
    }

    // Failure: discard the partial and (unless out of tries) go around again.
    // A clean transfer that hashed wrong is a truncated body — also transient.
    SD_MMC.remove(tmp);
    if (ok) err = "digest mismatch " + label;
    if (attempt == kMaxAttempts) break;

    // A 401 means the anonymous token expired mid-pull; mint a fresh one so this
    // retry and every later layer carry a valid bearer.
    if (err.endsWith("401")) {
      String fresh, terr;
      if (getAuthToken(ref, fresh, terr) && fresh.length()) bearer = fresh;
    }
    Status::info(label + " retry " + attempt + ": " + err);
    delay(400 * attempt);
  }
  return false;
}

bool getAuthToken(const ImageRef& ref, String& token, String& err) {
  token = "";
  WiFiClient client;
  HTTPClient http;
  if (!beginProxied(http, client, ref.registryHost, "/v2/")) {
    err = "v2 begin";
    return false;
  }
  const char* keys[] = {"WWW-Authenticate"};
  http.collectHeaders(keys, 1);
  int code = http.GET();
  String challenge = http.header("WWW-Authenticate");
  http.end();

  if (code == HTTP_CODE_OK) return true;  // registry needs no auth
  if (code != HTTP_CODE_UNAUTHORIZED) {
    err = String("v2 probe HTTP ") + code;
    return false;
  }

  String realm = extractAuthParam(challenge, "realm");
  String service = extractAuthParam(challenge, "service");
  if (!realm.length()) {
    err = "no auth realm";
    return false;
  }
  // realm is an absolute URL (e.g. https://auth.docker.io/token); reach it via the proxy too.
  String authHost, authPath;
  splitUrl(realm, authHost, authPath);
  String tokenPath = authPath + "?service=" + urlEncode(service) +
                     "&scope=" + urlEncode("repository:" + ref.repo + ":pull");

  WiFiClient client2;
  HTTPClient http2;
  if (!beginProxied(http2, client2, authHost, tokenPath)) {
    err = "token begin";
    return false;
  }
  int code2 = http2.GET();
  if (code2 != HTTP_CODE_OK) {
    err = String("token HTTP ") + code2;
    http2.end();
    return false;
  }
  // Buffer the body (de-chunks) before parsing; auth.docker.io often replies
  // chunked, and parsing the raw stream chokes on the chunk-size markers.
  String body = http2.getString();
  http2.end();
  JsonDocument doc;
  DeserializationError jerr = deserializeJson(doc, body);
  if (jerr) {
    err = String("token json: ") + jerr.c_str() + " (" + body.length() + "B)";
    return false;
  }
  if (doc["token"].is<const char*>())
    token = doc["token"].as<String>();
  else if (doc["access_token"].is<const char*>())
    token = doc["access_token"].as<String>();
  if (!token.length()) {
    err = "empty token";
    return false;
  }
  return true;
}

bool fetchManifest(const ImageRef& ref, const String& reference, const String& bearer,
                   String& body, String& contentType, String& err) {
  WiFiClient client;
  HTTPClient http;
  String path = "/v2/" + ref.repo + "/manifests/" + reference;
  if (!beginProxied(http, client, ref.registryHost, path)) {
    err = "manifest begin";
    return false;
  }
  http.addHeader("Accept", kManifestAccept);
  if (bearer.length()) http.addHeader("Authorization", "Bearer " + bearer);
  const char* keys[] = {"Content-Type"};
  http.collectHeaders(keys, 1);
  http.setTimeout(15000);

  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    err = String("manifest HTTP ") + code;
    http.end();
    return false;
  }
  body = http.getString();
  contentType = http.header("Content-Type");
  http.end();
  return true;
}

// Choose the manifest digest matching os/arch(/variant) from an index.
String selectFromIndex(JsonDocument& doc, const String& platform) {
  String os = "linux", arch = platform, variant;
  int slash = platform.indexOf('/');
  if (slash >= 0) {
    os = platform.substring(0, slash);
    String rest = platform.substring(slash + 1);
    int slash2 = rest.indexOf('/');
    if (slash2 >= 0) {
      arch = rest.substring(0, slash2);
      variant = rest.substring(slash2 + 1);
    } else {
      arch = rest;
    }
  }
  for (JsonObject m : doc["manifests"].as<JsonArray>()) {
    JsonObject p = m["platform"].as<JsonObject>();
    String mos = p["os"].as<String>();
    String march = p["architecture"].as<String>();
    if (mos == "unknown" || march == "unknown") continue;  // attestation entries
    if (mos == os && march == arch) {
      if (variant.length() && p["variant"].is<const char*>() &&
          p["variant"].as<String>() != variant)
        continue;
      return m["digest"].as<String>();
    }
  }
  return "";
}

bool writeTextFile(const String& path, const String& content) {
  File f = SD_MMC.open(path, FILE_WRITE);
  if (!f) return false;
  f.print(content);
  f.close();
  return true;
}

}  // namespace

namespace RegistryClient {

void setProxy(const String& proxyBase) { s_proxy = proxyBase; }

PullResult pull(const ImageRef& ref, const String& platform) {
  PullResult res;
  res.imageDir = ImageStore::dirFor(ref);
  if (!s_proxy.length()) {
    res.error = "no proxy configured";
    return res;
  }

  String token, err;
  if (!getAuthToken(ref, token, err)) {
    res.error = "auth: " + err;
    return res;
  }

  // Fetch top manifest; resolve index -> image manifest if needed.
  String body, ctype;
  if (!fetchManifest(ref, ref.manifestRef(), token, body, ctype, err)) {
    res.error = err;
    return res;
  }
  {
    JsonDocument doc;
    if (deserializeJson(doc, body)) {
      res.error = "manifest parse";
      return res;
    }
    bool isIndex = doc["manifests"].is<JsonArray>();
    if (isIndex) {
      String sub = selectFromIndex(doc, platform);
      if (!sub.length()) {
        res.error = "no manifest for " + platform;
        return res;
      }
      Status::info("arch " + platform);
      if (!fetchManifest(ref, sub, token, body, ctype, err)) {
        res.error = err;
        return res;
      }
    }
  }

  // Parse the image manifest (config + layers).
  JsonDocument mdoc;
  if (deserializeJson(mdoc, body)) {
    res.error = "image manifest parse";
    return res;
  }
  String manifestMediaType =
      mdoc["mediaType"].is<const char*>() ? mdoc["mediaType"].as<String>() : ctype;
  if (!manifestMediaType.length())
    manifestMediaType = "application/vnd.docker.distribution.manifest.v2+json";

  String configDigest = mdoc["config"]["digest"].as<String>();
  size_t configSize = mdoc["config"]["size"] | 0;
  JsonArray layers = mdoc["layers"].as<JsonArray>();
  if (!configDigest.length() || layers.isNull()) {
    res.error = "manifest missing config/layers";
    return res;
  }
  res.layerCount = layers.size();

  size_t totalBytes = configSize;
  for (JsonObject l : layers) totalBytes += (size_t)(l["size"] | 0);
  Status::event(ref.shortName() + ": " + res.layerCount + " layers " + humanBytes(totalBytes));

  // Prepare the image dir + shared blob store on SD.
  if (!ImageStore::ensureDir(res.imageDir) || !ImageStore::ensureCasDir()) {
    res.error = "mkdir store";
    return res;
  }

  // Blobs this image references (bare hex), persisted for load + GC.
  std::vector<String> refs;

  // Store the image manifest as a blob; the index references it by digest.
  String manifestDigestHex = sha256Hex((const uint8_t*)body.c_str(), body.length());
  if (!writeTextFile(ImageStore::blobPath(manifestDigestHex), body)) {
    res.error = "write manifest blob";
    return res;
  }
  refs.push_back(manifestDigestHex);

  // Config blob.
  Status::info("cfg " + configDigest.substring(7, 19));
  if (!downloadBlob(ref, configDigest, configSize, token, "config", "cfg", err)) {
    res.error = err;
    return res;
  }
  refs.push_back(stripDigest(configDigest));

  // Layer blobs (verbatim, gzip-compressed).
  int idx = 0;
  for (JsonObject layer : layers) {
    ++idx;
    String d = layer["digest"].as<String>();
    size_t sz = layer["size"] | 0;
    String label = String("layer ") + idx + "/" + res.layerCount;
    String cap = String("L") + idx + "/" + res.layerCount;
    Status::event(label + " start " + humanBytes(sz));
    if (!downloadBlob(ref, d, sz, token, label, cap, err)) {
      res.error = err;
      return res;
    }
    refs.push_back(stripDigest(d));
    Status::event(label + " ok " + humanBytes(sz));
  }

  // Persist the blob list so load tars the right shared blobs and GC can see
  // which blobs remain in use.
  if (!ImageStore::writeRefs(ref, refs)) {
    res.error = "write refs";
    return res;
  }

  // oci-layout marker.
  if (!writeTextFile(res.imageDir + "/oci-layout", "{\"imageLayoutVersion\":\"1.0.0\"}")) {
    res.error = "write oci-layout";
    return res;
  }

  // index.json referencing the image manifest, tagged with the original ref.
  {
    JsonDocument idoc;
    idoc["schemaVersion"] = 2;
    idoc["mediaType"] = "application/vnd.oci.image.index.v1+json";
    JsonObject m = idoc["manifests"].add<JsonObject>();
    m["mediaType"] = manifestMediaType;
    m["digest"] = "sha256:" + manifestDigestHex;
    m["size"] = (uint32_t)body.length();
    JsonObject ann = m["annotations"].to<JsonObject>();
    ann["io.containerd.image.name"] = ref.original;
    ann["org.opencontainers.image.ref.name"] = ref.original;
    String idx_json;
    serializeJson(idoc, idx_json);
    if (!writeTextFile(res.imageDir + "/index.json", idx_json)) {
      res.error = "write index.json";
      return res;
    }
  }

  res.ok = true;
  return res;
}

}  // namespace RegistryClient
