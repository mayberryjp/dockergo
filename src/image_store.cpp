#include "image_store.h"

#include <SD_MMC.h>

#include <vector>

namespace {
constexpr const char* kBase = "/images";

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

bool begin() { return ensureDir(kBase); }

String dirFor(const ImageRef& ref) { return String(kBase) + "/" + ref.safeId(); }

bool isComplete(const ImageRef& ref) { return SD_MMC.exists(dirFor(ref) + "/.complete"); }

bool markComplete(const ImageRef& ref) {
  File f = SD_MMC.open(dirFor(ref) + "/.complete", FILE_WRITE);
  if (!f) return false;
  f.print(ref.original);
  f.close();
  return true;
}

bool removeImage(const ImageRef& ref) { return rmrf(dirFor(ref)); }

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
