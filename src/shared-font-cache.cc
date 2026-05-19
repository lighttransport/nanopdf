// SPDX-License-Identifier: Apache 2.0
// Copyright 2024 - Present, Light Transport Entertainment Inc.

#include "shared-font-cache.hh"

namespace nanopdf {

SharedFontCache& SharedFontCache::instance() {
  static SharedFontCache cache;
  return cache;
}

bool SharedFontCache::find(const void* pdf, const std::string& font_name,
                           SharedFontEntry& out) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = cache_.find(Key{pdf, font_name});
  if (it == cache_.end()) return false;
  out = it->second;  // copy
  return true;
}

void SharedFontCache::store(const void* pdf, const std::string& font_name,
                            SharedFontEntry&& entry) {
  std::lock_guard<std::mutex> lock(mutex_);
  cache_[Key{pdf, font_name}] = std::move(entry);
}

}  // namespace nanopdf
