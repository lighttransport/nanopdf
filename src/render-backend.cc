// SPDX-License-Identifier: Apache-2.0
// Copyright 2024 - Present, Light Transport Entertainment Inc.

#include "render-backend.hh"

#include <algorithm>
#include <cctype>

#ifdef NANOPDF_USE_LIGHTVG
#include "lightvg-backend.hh"
#endif

#ifdef NANOPDF_USE_THORVG
#include "thorvg-backend.hh"
#endif

namespace nanopdf {

std::unique_ptr<RenderBackend> make_backend(BackendKind kind) {
  switch (kind) {
    case BackendKind::LightVG:
#ifdef NANOPDF_USE_LIGHTVG
      return std::unique_ptr<RenderBackend>(new LightVGBackend());
#else
      return nullptr;
#endif
    case BackendKind::ThorVG:
#ifdef NANOPDF_USE_THORVG
      return std::unique_ptr<RenderBackend>(new ThorVGBackend());
#else
      return nullptr;
#endif
  }
  return nullptr;
}

bool backend_available(BackendKind kind) {
  switch (kind) {
    case BackendKind::LightVG:
#ifdef NANOPDF_USE_LIGHTVG
      return true;
#else
      return false;
#endif
    case BackendKind::ThorVG:
#ifdef NANOPDF_USE_THORVG
      return true;
#else
      return false;
#endif
  }
  return false;
}

bool parse_backend_kind(const std::string& name, BackendKind* out) {
  if (!out) return false;
  std::string lower(name);
  std::transform(lower.begin(), lower.end(), lower.begin(),
                 [](unsigned char c) { return std::tolower(c); });
  if (lower == "lightvg" || lower == "light" || lower == "lui") {
    *out = BackendKind::LightVG;
    return true;
  }
  if (lower == "thorvg" || lower == "thor" || lower == "tvg") {
    *out = BackendKind::ThorVG;
    return true;
  }
  return false;
}

const char* backend_kind_name(BackendKind kind) {
  switch (kind) {
    case BackendKind::LightVG: return "lightvg";
    case BackendKind::ThorVG:  return "thorvg";
  }
  return "unknown";
}

}  // namespace nanopdf
