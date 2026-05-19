// SPDX-License-Identifier: Apache 2.0
// Copyright 2024 - Present, Light Transport Entertainment Inc.

#include "shared-font-cache.hh"
#include "color-transform.hh"

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

// --- SharedIccCache ---

SharedIccCache& SharedIccCache::instance() {
  static SharedIccCache cache;
  return cache;
}

bool SharedIccCache::find(uint64_t hash, color::IccProfileInfo& out) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = cache_.find(hash);
  if (it == cache_.end()) return false;
  out = it->second;  // copy
  return true;
}

void SharedIccCache::store(uint64_t hash, const color::IccProfileInfo& entry) {
  std::lock_guard<std::mutex> lock(mutex_);
  cache_[hash] = entry;
}

// --- SharedFontFallbackCache ---

SharedFontFallbackCache& SharedFontFallbackCache::instance() {
  static SharedFontFallbackCache cache;
  return cache;
}

bool SharedFontFallbackCache::find(const std::string& font_name, std::string& resolved_path) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = cache_.find(Key{font_name});
  if (it == cache_.end()) return false;
  resolved_path = it->second;
  return true;
}

void SharedFontFallbackCache::store(const std::string& font_name, const std::string& resolved_path) {
  std::lock_guard<std::mutex> lock(mutex_);
  cache_[Key{font_name}] = resolved_path;
}

}  // namespace nanopdf
