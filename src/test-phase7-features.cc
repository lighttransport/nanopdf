#include <cstddef>
#include <cstdint>

#include "nanopdf-log.hh"
#include "nanopdf.hh"
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

using namespace nanopdf;

namespace {

struct Sample {
  std::string base;
  std::string ccitt_path;
  std::string params_path;
  std::string pbm_path;
};

std::string data_root() {
#ifdef NANOPDF_SOURCE_DIR
  return std::string(NANOPDF_SOURCE_DIR) + "/data/ccitt";
#else
  return "../data/ccitt";
#endif
}

std::string default_pdf_path() {
#ifdef NANOPDF_SOURCE_DIR
  return std::string(NANOPDF_SOURCE_DIR) + "/data/blank.pdf";
#else
  return "../data/blank.pdf";
#endif
}

bool file_exists(const std::string& path) {
  std::ifstream ifs(path, std::ios::binary);
  return static_cast<bool>(ifs);
}

std::vector<uint8_t> read_binary(const std::string& path) {
  std::ifstream ifs(path, std::ios::binary);
  if (!ifs) {
    return {};
  }
  ifs.seekg(0, std::ios::end);
  std::streamoff size = ifs.tellg();
  if (size <= 0) {
    return {};
  }
  std::vector<uint8_t> buffer(static_cast<size_t>(size));
  ifs.seekg(0, std::ios::beg);
  ifs.read(reinterpret_cast<char*>(buffer.data()), size);
  if (!ifs) {
    buffer.clear();
  }
  return buffer;
}

std::vector<std::string> read_manifest(const std::string& path) {
  std::ifstream ifs(path);
  std::vector<std::string> entries;
  if (!ifs) {
    return entries;
  }
  std::string line;
  while (std::getline(ifs, line)) {
    if (line.empty() || line[0] == '#') {
      continue;
    }
    std::string trimmed = line;
    while (!trimmed.empty() &&
           (trimmed.back() == '\r' || trimmed.back() == '\n' ||
            trimmed.back() == ' ' || trimmed.back() == '\t')) {
      trimmed.pop_back();
    }
    if (!trimmed.empty()) {
      entries.push_back(trimmed);
    }
  }
  return entries;
}

bool parse_params_file(const std::string& path, filters::DecodeParams* params) {
  std::ifstream ifs(path);
  if (!ifs) {
    return false;
  }

  std::vector<std::string> tokens;
  std::string token;
  while (ifs >> token) {
    tokens.push_back(token);
  }
  if (tokens.empty()) {
    return false;
  }

  if (tokens[0] == "-4") {
    params->k = -1;
  } else if (tokens[0] == "-1") {
    params->k = 0;
  } else if (tokens[0] == "-2") {
    params->k = 1;
  } else {
    return false;
  }

  params->end_of_line = false;
  params->encoded_byte_align = false;
  params->black_is_1 = false;
  params->end_of_block = true;

  for (size_t i = 1; i < tokens.size(); ++i) {
    if (tokens[i] == "-A") {
      params->end_of_line = true;
    } else if (tokens[i] == "-P") {
      params->end_of_line = false;
    } else if (tokens[i] == "-W") {
      params->black_is_1 = true;
    } else if (tokens[i] == "-B") {
      params->black_is_1 = false;
    } else if (tokens[i] == "-X") {
      if (i + 1 >= tokens.size()) {
        return false;
      }
      params->columns = std::atoi(tokens[i + 1].c_str());
      ++i;
    }
  }

  return params->columns > 0;
}

bool next_token(const std::vector<uint8_t>& data, size_t* idx,
                std::string* out) {
  const size_t n = data.size();
  while (*idx < n) {
    uint8_t c = data[*idx];
    if (c == '#') {
      while (*idx < n && data[*idx] != '\n' && data[*idx] != '\r') {
        ++(*idx);
      }
      continue;
    }
    if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
      ++(*idx);
      continue;
    }
    break;
  }
  if (*idx >= n) {
    return false;
  }
  size_t start = *idx;
  while (*idx < n) {
    uint8_t c = data[*idx];
    if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
      break;
    }
    ++(*idx);
  }
  out->assign(reinterpret_cast<const char*>(data.data() + start), *idx - start);
  return true;
}

bool read_pbm(const std::string& path, int* width, int* height,
              std::vector<uint8_t>* payload) {
  std::vector<uint8_t> data = read_binary(path);
  if (data.empty()) {
    return false;
  }

  size_t idx = 0;
  std::string magic;
  std::string width_str;
  std::string height_str;
  if (!next_token(data, &idx, &magic) ||
      !next_token(data, &idx, &width_str) ||
      !next_token(data, &idx, &height_str)) {
    return false;
  }
  if (magic != "P4") {
    return false;
  }
  *width = std::atoi(width_str.c_str());
  *height = std::atoi(height_str.c_str());
  if (*width <= 0 || *height <= 0) {
    return false;
  }

  while (idx < data.size()) {
    uint8_t c = data[idx];
    if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
      ++idx;
      continue;
    }
    break;
  }

  if (idx >= data.size()) {
    return false;
  }

  payload->assign(data.begin() + static_cast<long>(idx), data.end());
  return true;
}

uint64_t count_bit_errors(const std::vector<uint8_t>& a,
                          const std::vector<uint8_t>& b) {
  uint64_t errors = 0;
  size_t n = a.size();
  if (b.size() < n) {
    n = b.size();
  }
  for (size_t i = 0; i < n; ++i) {
    errors += static_cast<uint64_t>(__builtin_popcount(a[i] ^ b[i]));
  }
  return errors;
}

bool compare_sample(const Sample& sample) {
  filters::DecodeParams params;
  if (!parse_params_file(sample.params_path, &params)) {
    std::cerr << "Failed to parse params: " << sample.params_path << std::endl;
    return false;
  }

  int width = 0;
  int height = 0;
  std::vector<uint8_t> ref;
  if (!read_pbm(sample.pbm_path, &width, &height, &ref)) {
    std::cerr << "Failed to read PBM: " << sample.pbm_path << std::endl;
    return false;
  }
  if (params.columns != width) {
    std::cerr << "Width mismatch for " << sample.base << ": params "
              << params.columns << " pbm " << width << std::endl;
    return false;
  }
  const size_t expected_size =
      static_cast<size_t>((width + 7) / 8) * static_cast<size_t>(height);
  if (ref.size() != expected_size) {
    std::cerr << "PBM payload size mismatch for " << sample.base << ": "
              << ref.size() << " expected " << expected_size << std::endl;
    return false;
  }
  params.rows = height;

  std::vector<uint8_t> ccitt = read_binary(sample.ccitt_path);
  if (ccitt.empty()) {
    std::cerr << "Failed to read CCITT: " << sample.ccitt_path << std::endl;
    return false;
  }

  DecodedStream decoded =
      filters::decode_ccittfax(ccitt.data(), ccitt.size(), params);
  if (!decoded.success) {
    std::cerr << "Decode failed for " << sample.base << ": "
              << decoded.error << std::endl;
    return false;
  }

  if (decoded.data.size() != ref.size()) {
    std::cerr << "Size mismatch for " << sample.base << ": decoded "
              << decoded.data.size() << " ref " << ref.size() << std::endl;
    return false;
  }

  uint64_t bad_bits = count_bit_errors(decoded.data, ref);
  if (bad_bits != 0) {
    std::cerr << "Bit mismatch for " << sample.base << ": " << bad_bits
              << " bit errors" << std::endl;
    return false;
  }

  std::cout << "  " << sample.base << ": PASS" << std::endl;
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  nanopdf::log::set_log_level(nanopdf::log::Level::kError);

  std::string pdf_path = (argc > 1) ? argv[1] : default_pdf_path();
  if (!file_exists(pdf_path)) {
    std::cerr << "PDF not found: " << pdf_path << std::endl;
    std::cerr << "Usage: " << argv[0] << " <pdf-path>" << std::endl;
    return 1;
  }

  std::string root = data_root();
  std::string manifest_path = root + "/manifest.txt";
  std::vector<std::string> entries = read_manifest(manifest_path);
  if (entries.empty()) {
    std::cerr << "No CCITT samples found in " << manifest_path << std::endl;
    return 1;
  }

  std::cout << "=== Phase 7 CCITT Regression Tests ===" << std::endl;
  std::cout << "PDF: " << pdf_path << std::endl;

  bool ok = true;
  for (const auto& base : entries) {
    Sample sample;
    sample.base = base;
    sample.ccitt_path = root + "/" + base + ".ccitt";
    sample.params_path = root + "/" + base + ".params";
    sample.pbm_path = root + "/" + base + ".pbm";
    if (!compare_sample(sample)) {
      ok = false;
    }
  }

  if (!ok) {
    std::cerr << "CCITT regression tests failed" << std::endl;
    return 1;
  }

  std::cout << "All CCITT regression tests passed!" << std::endl;
  return 0;
}
