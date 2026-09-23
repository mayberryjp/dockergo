#pragma once
#include <Arduino.h>

// Parsed Docker image reference with Docker Hub normalization applied.
//   pihole/pihole:latest            -> host=registry-1.docker.io repo=pihole/pihole tag=latest
//   docker.io/library/mongo:7.0     -> host=registry-1.docker.io repo=library/mongo tag=7.0
//   ghcr.io/.../frigate:stable      -> host=ghcr.io repo=.../frigate tag=stable
struct ImageRef {
  String original;
  String registryHost;  // network host for the registry API
  String repo;          // namespace/name
  String tag;           // may be empty if digest set
  String digest;        // optional sha256:... reference

  // Filesystem-safe unique id used for the SD cache directory.
  String safeId() const;
  // The manifest reference: digest if present, otherwise tag.
  String manifestRef() const { return digest.length() ? digest : tag; }
  // Short human label for the LCD, e.g. "frigate:stable".
  String shortName() const;
};

bool parseImageRef(const String& in, ImageRef& out);
