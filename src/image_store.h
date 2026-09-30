#pragma once
#include <Arduino.h>

#include <vector>

#include "image_ref.h"

// SD-card cache of downloaded images. Blobs are content-addressed and shared
// across images in one store; each image dir holds only its OCI metadata plus a
// refs list naming the shared blobs it uses (see SPEC §6.2):
//   /blobs/sha256/<hex>   shared blob store (manifests, configs, layers)
//   /images/<safeId>/     oci-layout, index.json, refs, .complete
namespace ImageStore {
bool begin();
String dirFor(const ImageRef& ref);
bool isComplete(const ImageRef& ref);
// Manifest digest (bare hex) of the cached image, i.e. what it was pulled as;
// empty if not cached. Used to detect a moved mutable tag like `latest`.
String cachedDigest(const ImageRef& ref);
bool markComplete(const ImageRef& ref);
bool removeImage(const ImageRef& ref);
bool ensureDir(const String& path);

// Shared content-addressed blob store.
bool ensureCasDir();
String blobPath(const String& digestHex);  // /blobs/sha256/<hex>
// Record the bare-hex blob digests an image references (for load + GC).
bool writeRefs(const ImageRef& ref, const std::vector<String>& digestsHex);
// Delete blobs no surviving image references; returns the count removed.
int gcUnreferencedBlobs();
}  // namespace ImageStore
