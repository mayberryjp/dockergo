#include "image_ref.h"

#include <ctype.h>

bool parseImageRef(const String& in, ImageRef& out) {
  out = ImageRef{};
  out.original = in;

  String s = in;
  String host;
  String remainder;

  int slash = s.indexOf('/');
  bool hasRegistry = false;
  if (slash > 0) {
    String first = s.substring(0, slash);
    // A leading segment is a registry only if it looks like a host.
    if (first.indexOf('.') >= 0 || first.indexOf(':') >= 0 || first == "localhost") {
      hasRegistry = true;
      host = first;
      remainder = s.substring(slash + 1);
    }
  }
  if (!hasRegistry) {
    host = "registry-1.docker.io";
    remainder = s;
  } else if (host == "docker.io") {
    host = "registry-1.docker.io";
  }

  // Optional @sha256:... digest.
  int at = remainder.indexOf('@');
  if (at >= 0) {
    out.digest = remainder.substring(at + 1);
    remainder = remainder.substring(0, at);
  }

  // Tag (remainder no longer contains a host, so ':' is the tag separator).
  String repo = remainder;
  String tag;
  int colon = remainder.lastIndexOf(':');
  if (colon >= 0) {
    repo = remainder.substring(0, colon);
    tag = remainder.substring(colon + 1);
  }

  // Docker Hub implicit library/ namespace.
  if (host == "registry-1.docker.io" && repo.indexOf('/') < 0) {
    repo = "library/" + repo;
  }

  out.registryHost = host;
  out.repo = repo;
  if (out.digest.length() == 0) out.tag = tag.length() ? tag : "latest";
  else out.tag = tag;  // keep tag alongside digest if the ref had both

  return out.repo.length() > 0;
}

String ImageRef::shortName() const {
  int s = repo.lastIndexOf('/');
  String n = (s >= 0) ? repo.substring(s + 1) : repo;
  return n + ":" + (tag.length() ? tag : "latest");
}

String ImageRef::safeId() const {
  String key = registryHost + "_" + repo + "_" + (digest.length() ? digest : tag);
  String out;
  out.reserve(key.length());
  for (unsigned i = 0; i < key.length(); ++i) {
    char c = key.charAt(i);
    out += (isalnum((unsigned char)c) ? c : '_');
  }
  return out;
}
