#include "image_store.h"

#include <SD_MMC.h>

#include <vector>

namespace {
constexpr const char* kBase = "/images";
constexpr const char* kCas = "/blobs/sha256";  // shared content-addressed blobs
constexpr const char* kRefs = "refs";          // per-image list of blob digests

String childPath(const String& parent, File& entry) {
  String n = entry.name();
  if (n.startsWith("/")) return n;  // core returned full path
  return parent + "/" + n;          // core returned base name
}

bool rmrf(const String& path) {
  File f = SD_MMC.open(path);
  if (!f) return true;
  if (!f.isDirectory()) {
    f.close();
    return SD_MMC.remove(path);
  }

  // Collect children first, then delete (avoid mutating an open directory).
  std::vector<String> files;
  std::vector<String> dirs;
  for (File e = f.openNextFile(); e; e = f.openNextFile()) {
    String cp = childPath(path, e);
    if (e.isDirectory())
      dirs.push_back(cp);
    else
      files.push_back(cp);
    e.close();
  }
  f.close();

  for (auto& fp : files) SD_MMC.remove(fp);
  for (auto& dp : dirs) rmrf(dp);
  return SD_MMC.rmdir(path);
}
}  // namespace

namespace ImageStore {

bool begin() { return ensureDir(kBase) && ensureDir(kCas); }

String dirFor(const ImageRef& ref) { return String(kBase) + "/" + ref.safeId(); }

bool isComplete(const ImageRef& ref) { return SD_MMC.exists(dirFor(ref) + "/.complete"); }

bool markComplete(const ImageRef& ref) {
  File f = SD_MMC.open(dirFor(ref) + "/.complete", FILE_WRITE);
  if (!f) return false;
  f.print(ref.original);
  f.close();
  return true;
}

bool removeImage(const ImageRef& ref) {
  bool ok = rmrf(dirFor(ref));
  gcUnreferencedBlobs();  // reclaim blobs no surviving image references
  return ok;
}

bool ensureCasDir() { return ensureDir(kCas); }

String blobPath(const String& digestHex) { return String(kCas) + "/" + digestHex; }

bool writeRefs(const ImageRef& ref, const std::vector<String>& digestsHex) {
  File f = SD_MMC.open(dirFor(ref) + "/" + kRefs, FILE_WRITE);
  if (!f) return false;
  for (const auto& h : digestsHex) {
    f.print(h);
    f.print('\n');
  }
  f.close();
  return true;
}

int gcUnreferencedBlobs() {
  // Union of digests still named by some image's refs file.
  std::vector<String> referenced;
  if (File images = SD_MMC.open(kBase)) {
    for (File e = images.openNextFile(); e; e = images.openNextFile()) {
      if (e.isDirectory()) {
        if (File rf = SD_MMC.open(childPath(kBase, e) + "/" + kRefs, FILE_READ)) {
          while (rf.available()) {
            String h = rf.readStringUntil('\n');
            h.trim();
            if (h.length()) referenced.push_back(h);
          }
          rf.close();
        }
      }
      e.close();
    }
    images.close();
  }

  // Sweep CAS blobs absent from that union (collect first, then delete so we
  // never mutate the directory we're still iterating).
  std::vector<String> victims;
  if (File cas = SD_MMC.open(kCas)) {
    for (File b = cas.openNextFile(); b; b = cas.openNextFile()) {
      if (!b.isDirectory()) {
        String nm = b.name();
        String base = nm.substring(nm.lastIndexOf('/') + 1);
        bool keep = false;
        for (const auto& r : referenced)
          if (r == base) {
            keep = true;
            break;
          }
        if (!keep) victims.push_back(String(kCas) + "/" + base);
      }
      b.close();
    }
    cas.close();
  }
  for (auto& v : victims) SD_MMC.remove(v);
  return (int)victims.size();
}

bool ensureDir(const String& path) {
  unsigned i = 1;  // skip leading '/'
  while (i <= path.length()) {
    int slash = path.indexOf('/', i);
    String sub = (slash < 0) ? path : path.substring(0, slash);
    if (sub.length() && !SD_MMC.exists(sub)) SD_MMC.mkdir(sub);
    if (slash < 0) break;
    i = slash + 1;
  }
  return SD_MMC.exists(path);
}

}  // namespace ImageStore
