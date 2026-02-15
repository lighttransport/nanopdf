// SPDX-License-Identifier: Apache 2.0
// Copyright 2024 - Present, Light Transport Entertainment Inc.
//
// Limited JPEG2000 (JPX) decoder implementation
// Implements codeblock decoding with MQ arithmetic coding,
// packet parsing with tag trees, and full decode pipeline.

#include "jpx-decoder.hh"
#include <cstring>
#include <cmath>
#include <algorithm>
#include <cassert>

#ifdef NANOPDF_DEBUG_PRINT
#include <cstdio>
#endif

namespace nanopdf {
namespace jpx {

// MQ decoder probability estimation table (from JPEG2000 spec, Table D.3)
static const uint16_t qe_table[] = {
  0x5601, 0x3401, 0x1801, 0x0AC1, 0x0521, 0x0221, 0x5601, 0x5401,
  0x4801, 0x3801, 0x3001, 0x2401, 0x1C01, 0x1601, 0x5601, 0x5401,
  0x5101, 0x4801, 0x3801, 0x3401, 0x3001, 0x2801, 0x2401, 0x2201,
  0x1C01, 0x1801, 0x1601, 0x1401, 0x1201, 0x1101, 0x0AC1, 0x09C1,
  0x08A1, 0x0521, 0x0441, 0x02A1, 0x0221, 0x0141, 0x0111, 0x0085,
  0x0049, 0x0025, 0x0015, 0x0009, 0x0005, 0x0001, 0x5601
};

static const uint8_t nmps_table[] = {
  1, 2, 3, 4, 5, 38, 7, 8, 9, 10, 11, 12, 13, 29, 15, 16,
  17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32,
  33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 45, 46
};

static const uint8_t nlps_table[] = {
  1, 6, 9, 12, 29, 33, 6, 14, 14, 14, 17, 18, 20, 21, 14, 14,
  15, 16, 17, 18, 19, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29,
  30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 46
};

static const uint8_t switch_table[] = {
  1, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 1, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
};

// 5/3 wavelet filter coefficients (lossless) - not used for lifting, kept
// for reference
// static const float lift_53_alpha = -0.5f;
// static const float lift_53_beta = 0.25f;

// 9/7 wavelet filter coefficients (lossy)
static const float lift_97_alpha = -1.586134342f;
static const float lift_97_beta = -0.052980118f;
static const float lift_97_gamma = 0.882911075f;
static const float lift_97_delta = 0.443506852f;
static const float lift_97_k = 1.230174105f;

// Codeblock state flags
static const uint8_t STATE_SIG     = 0x01;  // Coefficient is significant
static const uint8_t STATE_VISITED = 0x02;  // Visited in current pass
static const uint8_t STATE_REFINED = 0x04;  // Has been magnitude-refined

// Utility: integer ceiling division
static inline int ceildiv(int a, int b) {
  if (b <= 0) return 0;
  return (a + b - 1) / b;
}

//-----------------------------------------------------------------------------
// TagTree implementation
//-----------------------------------------------------------------------------

TagTree::TagTree(int width, int height) : width_(width), height_(height) {
  if (width <= 0 || height <= 0) return;

  int w = width, h = height;
  while (true) {
    levels_.push_back(std::vector<Node>(static_cast<size_t>(w) * h));
    if (w == 1 && h == 1) break;
    w = ceildiv(w, 2);
    h = ceildiv(h, 2);
  }
}

bool TagTree::decode(int x, int y, int threshold, MQDecoder& mq, int ctx) {
  if (levels_.empty()) return false;

  int num_levels = static_cast<int>(levels_.size());

  // Build path from leaf (level 0) to root (highest level)
  std::vector<int> path_x(num_levels), path_y(num_levels);
  std::vector<int> path_w(num_levels);

  {
    int cx = x, cy = y;
    int w = width_;
    for (int lev = 0; lev < num_levels; ++lev) {
      path_x[lev] = cx;
      path_y[lev] = cy;
      path_w[lev] = w;
      cx >>= 1;
      cy >>= 1;
      w = ceildiv(w, 2);
    }
  }

  // Traverse from root to leaf
  int min_value = 0;
  for (int lev = num_levels - 1; lev >= 0; --lev) {
    int idx = path_y[lev] * path_w[lev] + path_x[lev];
    if (idx < 0 || idx >= static_cast<int>(levels_[lev].size())) {
      return false;
    }
    Node& node = levels_[lev][idx];

    if (node.known) {
      min_value = std::max(min_value, node.value);
      continue;
    }

    if (node.value < min_value) {
      node.value = min_value;
    }

    while (node.value < threshold) {
      int bit = mq.decode(ctx);
      if (bit) {
        node.known = true;
        break;
      }
      node.value++;
    }

    if (!node.known && node.value >= threshold) {
      min_value = node.value;
      if (lev == 0) return false;
      continue;
    }

    min_value = node.value;
  }

  int leaf_idx = path_y[0] * path_w[0] + path_x[0];
  if (leaf_idx < 0 || leaf_idx >= static_cast<int>(levels_[0].size())) {
    return false;
  }
  return levels_[0][leaf_idx].known && levels_[0][leaf_idx].value <= threshold;
}

int TagTree::value(int x, int y) const {
  if (levels_.empty()) return 0;
  int idx = y * width_ + x;
  if (idx < 0 || idx >= static_cast<int>(levels_[0].size())) return 0;
  return levels_[0][idx].value;
}

//-----------------------------------------------------------------------------
// BitReader implementation
//-----------------------------------------------------------------------------

JPXDecoder::BitReader::BitReader(const uint8_t* data, size_t size)
    : data_(data), size_(size), pos_(0), bit_pos_(0) {}

uint32_t JPXDecoder::BitReader::read_bits(int n) {
  uint32_t result = 0;
  for (int i = 0; i < n; ++i) {
    result = (result << 1) | (read_bit() ? 1 : 0);
  }
  return result;
}

bool JPXDecoder::BitReader::read_bit() {
  if (pos_ >= size_) return false;
  bool bit = (data_[pos_] >> (7 - bit_pos_)) & 1;
  bit_pos_++;
  if (bit_pos_ >= 8) {
    bit_pos_ = 0;
    pos_++;
    // Handle bit stuffing after 0xFF
    if (pos_ > 0 && data_[pos_ - 1] == 0xFF && pos_ < size_) {
      if ((data_[pos_] & 0x80) == 0) {
        bit_pos_ = 1;
      }
    }
  }
  return bit;
}

void JPXDecoder::BitReader::align_to_byte() {
  if (bit_pos_ > 0) {
    bit_pos_ = 0;
    pos_++;
  }
}

bool JPXDecoder::BitReader::eof() const {
  return pos_ >= size_;
}

//-----------------------------------------------------------------------------
// MQDecoder implementation
//-----------------------------------------------------------------------------

MQDecoder::MQDecoder() {
  reset();
}

void MQDecoder::reset() {
  memset(cx_states_, 0, sizeof(cx_states_));
  memset(cx_mps_, 0, sizeof(cx_mps_));
  // Initialize contexts per JPEG2000 spec
  cx_states_[17] = 3;   // Run length context
  cx_states_[18] = 46;  // Uniform context
}

void MQDecoder::init(const uint8_t* data, size_t size) {
  data_ = data;
  size_ = size;
  pos_ = 0;

  reset();

  if (size == 0) {
    a_ = 0x8000;
    c_ = 0;
    ct_ = 0;
    return;
  }

  // Initialize C register per D.2 in the standard
  c_ = static_cast<uint32_t>(data_[0]) << 16;
  if (size_ > 1) {
    c_ |= static_cast<uint32_t>(data_[1]) << 8;
  }
  pos_ = 2;
  bytein();
  c_ <<= 7;
  ct_ = ct_ - 7;
  a_ = 0x8000;
}

void MQDecoder::bytein() {
  if (pos_ >= size_) {
    c_ += 0xFF;
    ct_ = 8;
    return;
  }

  uint8_t b = data_[pos_];
  if (pos_ > 0 && data_[pos_ - 1] == 0xFF) {
    if (b > 0x8F) {
      c_ += 0xFF;
      ct_ = 8;
    } else {
      pos_++;
      c_ += static_cast<uint32_t>(b) << 9;
      ct_ = 7;
    }
  } else {
    pos_++;
    c_ += static_cast<uint32_t>(b) << 8;
    ct_ = 8;
  }
}

int MQDecoder::decode(int cx) {
  uint16_t qe = qe_table[cx_states_[cx]];
  a_ -= qe;

  if ((c_ >> 16) < a_) {
    if ((a_ & 0x8000) == 0) {
      return mps_exchange(cx);
    }
    return cx_mps_[cx];
  }

  c_ -= static_cast<uint32_t>(a_) << 16;
  return lps_exchange(cx);
}

int MQDecoder::mps_exchange(int cx) {
  int d;
  uint16_t qe = qe_table[cx_states_[cx]];

  if (a_ < qe) {
    d = 1 - cx_mps_[cx];
    if (switch_table[cx_states_[cx]]) {
      cx_mps_[cx] = 1 - cx_mps_[cx];
    }
    cx_states_[cx] = nlps_table[cx_states_[cx]];
  } else {
    d = cx_mps_[cx];
    cx_states_[cx] = nmps_table[cx_states_[cx]];
  }
  renormd();
  return d;
}

int MQDecoder::lps_exchange(int cx) {
  int d;
  uint16_t qe = qe_table[cx_states_[cx]];

  if (a_ < qe) {
    a_ = qe;
    d = cx_mps_[cx];
    cx_states_[cx] = nmps_table[cx_states_[cx]];
  } else {
    a_ = qe;
    d = 1 - cx_mps_[cx];
    if (switch_table[cx_states_[cx]]) {
      cx_mps_[cx] = 1 - cx_mps_[cx];
    }
    cx_states_[cx] = nlps_table[cx_states_[cx]];
  }
  renormd();
  return d;
}

void MQDecoder::renormd() {
  do {
    if (ct_ == 0) {
      bytein();
    }
    a_ <<= 1;
    c_ <<= 1;
    ct_--;
  } while ((a_ & 0x8000) == 0);
}

//-----------------------------------------------------------------------------
// Context formation helpers for codeblock coding
//-----------------------------------------------------------------------------

// Significance context labels (Table D.1, D.2, D.3 in JPEG2000 spec)
// subband_type: 0=HL, 1=LH, 2=HH/LL
int JPXDecoder::get_significance_context(const std::vector<uint8_t>& state,
                                         int x, int y, int width, int height,
                                         int subband_type) const {
  int h0 = 0, h1 = 0, v0 = 0, v1 = 0, d = 0;

  if (x > 0 && (state[y * width + (x - 1)] & STATE_SIG)) h0 = 1;
  if (x < width - 1 && (state[y * width + (x + 1)] & STATE_SIG)) h1 = 1;
  if (y > 0 && (state[(y - 1) * width + x] & STATE_SIG)) v0 = 1;
  if (y < height - 1 && (state[(y + 1) * width + x] & STATE_SIG)) v1 = 1;

  if (x > 0 && y > 0 &&
      (state[(y - 1) * width + (x - 1)] & STATE_SIG)) d++;
  if (x < width - 1 && y > 0 &&
      (state[(y - 1) * width + (x + 1)] & STATE_SIG)) d++;
  if (x > 0 && y < height - 1 &&
      (state[(y + 1) * width + (x - 1)] & STATE_SIG)) d++;
  if (x < width - 1 && y < height - 1 &&
      (state[(y + 1) * width + (x + 1)] & STATE_SIG)) d++;

  int h = h0 + h1;
  int v = v0 + v1;

  if (subband_type == 0) {
    // HL subband
    if (h == 2) return 8;
    if (h == 1) {
      if (v >= 1) return 7;
      if (d >= 1) return 6;
      return 5;
    }
    if (v == 2) return 4;
    if (v == 1) {
      if (d >= 1) return 3;
      return 2;
    }
    if (d >= 2) return 1;
    if (d == 1) return 1;
    return 0;
  } else if (subband_type == 1) {
    // LH subband
    if (v == 2) return 8;
    if (v == 1) {
      if (h >= 1) return 7;
      if (d >= 1) return 6;
      return 5;
    }
    if (h == 2) return 4;
    if (h == 1) {
      if (d >= 1) return 3;
      return 2;
    }
    if (d >= 2) return 1;
    if (d == 1) return 1;
    return 0;
  } else {
    // HH or LL subband
    int hv = h + v;
    if (d >= 3) return 8;
    if (d == 2) {
      if (hv >= 1) return 7;
      return 6;
    }
    if (d == 1) {
      if (hv >= 2) return 7;
      if (hv == 1) return 6;
      return 5;
    }
    if (hv >= 2) return 4;
    if (hv == 1) return 3;
    return 0;
  }
}

int JPXDecoder::get_sign_context(const std::vector<uint8_t>& state,
                                 const std::vector<int32_t>& coeffs,
                                 int x, int y, int width, int height,
                                 int& sign_flip) const {
  int h_contrib = 0;
  if (x > 0 && (state[y * width + (x - 1)] & STATE_SIG)) {
    h_contrib += (coeffs[y * width + (x - 1)] >= 0) ? 1 : -1;
  }
  if (x < width - 1 && (state[y * width + (x + 1)] & STATE_SIG)) {
    h_contrib += (coeffs[y * width + (x + 1)] >= 0) ? 1 : -1;
  }

  int v_contrib = 0;
  if (y > 0 && (state[(y - 1) * width + x] & STATE_SIG)) {
    v_contrib += (coeffs[(y - 1) * width + x] >= 0) ? 1 : -1;
  }
  if (y < height - 1 && (state[(y + 1) * width + x] & STATE_SIG)) {
    v_contrib += (coeffs[(y + 1) * width + x] >= 0) ? 1 : -1;
  }

  // Table D.4 from JPEG2000 spec
  sign_flip = 0;

  if (h_contrib > 0) {
    if (v_contrib > 0) { sign_flip = 0; return 13; }
    if (v_contrib == 0) { sign_flip = 0; return 12; }
    /* v < 0 */ { sign_flip = 0; return 11; }
  } else if (h_contrib == 0) {
    if (v_contrib > 0) { sign_flip = 0; return 10; }
    if (v_contrib == 0) { sign_flip = 0; return 9; }
    /* v < 0 */ { sign_flip = 1; return 10; }
  } else {
    // h < 0
    if (v_contrib > 0) { sign_flip = 1; return 11; }
    if (v_contrib == 0) { sign_flip = 1; return 12; }
    /* v < 0 */ { sign_flip = 1; return 13; }
  }
  return 9;
}

int JPXDecoder::get_magnitude_refinement_context(
    const std::vector<uint8_t>& state,
    int x, int y, int width, int height) const {

  bool first_refinement = !(state[y * width + x] & STATE_REFINED);

  bool has_sig_neighbor = false;
  for (int dy = -1; dy <= 1 && !has_sig_neighbor; ++dy) {
    for (int dx = -1; dx <= 1 && !has_sig_neighbor; ++dx) {
      if (dx == 0 && dy == 0) continue;
      int nx = x + dx, ny = y + dy;
      if (nx >= 0 && nx < width && ny >= 0 && ny < height) {
        if (state[ny * width + nx] & STATE_SIG) {
          has_sig_neighbor = true;
        }
      }
    }
  }

  if (first_refinement) {
    if (has_sig_neighbor) return 15;
    return 14;
  }
  return 16;
}

//-----------------------------------------------------------------------------
// Codeblock coding passes
//-----------------------------------------------------------------------------

void JPXDecoder::significance_propagation_pass(
    MQDecoder& mq, std::vector<int32_t>& coeffs, std::vector<uint8_t>& state,
    int width, int height, int bit_plane, int subband_type) {

  for (int x = 0; x < width; ++x) {
    for (int y0 = 0; y0 < height; y0 += 4) {
      int y_end = std::min(y0 + 4, height);
      for (int y = y0; y < y_end; ++y) {
        int idx = y * width + x;

        if (state[idx] & STATE_SIG) continue;
        if (state[idx] & STATE_VISITED) continue;

        int ctx = get_significance_context(state, x, y, width, height,
                                           subband_type);
        if (ctx == 0) continue;

        state[idx] |= STATE_VISITED;

        int sig = mq.decode(ctx);
        if (sig) {
          int sign_flip = 0;
          int sign_ctx = get_sign_context(state, coeffs, x, y, width, height,
                                          sign_flip);
          int sign_bit = mq.decode(sign_ctx);
          sign_bit ^= sign_flip;

          int32_t magnitude = 1 << bit_plane;
          coeffs[idx] = sign_bit ? -magnitude : magnitude;
          state[idx] |= STATE_SIG;
        }
      }
    }
  }
}

void JPXDecoder::magnitude_refinement_pass(
    MQDecoder& mq, std::vector<int32_t>& coeffs, std::vector<uint8_t>& state,
    int width, int height, int bit_plane) {

  for (int x = 0; x < width; ++x) {
    for (int y0 = 0; y0 < height; y0 += 4) {
      int y_end = std::min(y0 + 4, height);
      for (int y = y0; y < y_end; ++y) {
        int idx = y * width + x;

        if (!(state[idx] & STATE_SIG)) continue;
        if (state[idx] & STATE_VISITED) continue;

        state[idx] |= STATE_VISITED;

        int ctx = get_magnitude_refinement_context(state, x, y, width, height);
        int bit = mq.decode(ctx);

        int32_t abs_val = std::abs(coeffs[idx]);
        abs_val |= (bit << bit_plane);
        coeffs[idx] = (coeffs[idx] < 0) ? -abs_val : abs_val;

        state[idx] |= STATE_REFINED;
      }
    }
  }
}

void JPXDecoder::cleanup_pass(
    MQDecoder& mq, std::vector<int32_t>& coeffs, std::vector<uint8_t>& state,
    int width, int height, int bit_plane, int subband_type) {

  for (int x = 0; x < width; ++x) {
    for (int y0 = 0; y0 < height; y0 += 4) {
      int y_end = std::min(y0 + 4, height);
      int stripe_height = y_end - y0;

      int y = y0;

      // Check if run-length mode can be used
      if (stripe_height == 4) {
        bool can_run = true;
        for (int yy = y0; yy < y_end; ++yy) {
          int idx2 = yy * width + x;
          if ((state[idx2] & STATE_SIG) || (state[idx2] & STATE_VISITED)) {
            can_run = false;
            break;
          }
          int ctx = get_significance_context(state, x, yy, width, height,
                                             subband_type);
          if (ctx != 0) {
            can_run = false;
            break;
          }
        }

        if (can_run) {
          int run_bit = mq.decode(17);  // CX=17: run-length context
          if (!run_bit) {
            // Entire stripe is zero
            for (int yy = y0; yy < y_end; ++yy) {
              state[yy * width + x] &= static_cast<uint8_t>(~STATE_VISITED);
            }
            continue;
          }
          // Decode position of significant sample using uniform context
          int pos = mq.decode(18);
          pos = (pos << 1) | mq.decode(18);
          y = y0 + pos;

          // Set the significant sample
          {
            int idx2 = y * width + x;
            int sign_flip = 0;
            int sign_ctx = get_sign_context(state, coeffs, x, y, width,
                                            height, sign_flip);
            int sign_bit = mq.decode(sign_ctx);
            sign_bit ^= sign_flip;
            int32_t magnitude = 1 << bit_plane;
            coeffs[idx2] = sign_bit ? -magnitude : magnitude;
            state[idx2] |= STATE_SIG;
          }
          y++;
        }
      }

      // Process remaining samples normally
      for (; y < y_end; ++y) {
        int idx2 = y * width + x;

        if ((state[idx2] & STATE_SIG) || (state[idx2] & STATE_VISITED)) {
          state[idx2] &= static_cast<uint8_t>(~STATE_VISITED);
          continue;
        }

        int ctx = get_significance_context(state, x, y, width, height,
                                           subband_type);
        int sig = mq.decode(ctx);

        if (sig) {
          int sign_flip = 0;
          int sign_ctx = get_sign_context(state, coeffs, x, y, width, height,
                                          sign_flip);
          int sign_bit = mq.decode(sign_ctx);
          sign_bit ^= sign_flip;
          int32_t magnitude = 1 << bit_plane;
          coeffs[idx2] = sign_bit ? -magnitude : magnitude;
          state[idx2] |= STATE_SIG;
        }

        state[idx2] &= static_cast<uint8_t>(~STATE_VISITED);
      }

      // Clear visited flags for the entire stripe
      for (int yy = y0; yy < y_end; ++yy) {
        state[yy * width + x] &= static_cast<uint8_t>(~STATE_VISITED);
      }
    }
  }
}

//-----------------------------------------------------------------------------
// Codeblock decoding
//-----------------------------------------------------------------------------

bool JPXDecoder::decode_codeblock(Codeblock& cb, int subband_type) {
  if (cb.data.empty() || cb.width <= 0 || cb.height <= 0) return true;
  if (cb.num_passes <= 0) return true;

  int w = cb.width;
  int h = cb.height;

  cb.coeffs.assign(static_cast<size_t>(w) * h, 0);
  std::vector<uint8_t> state(static_cast<size_t>(w) * h, 0);

  MQDecoder mq;
  mq.init(cb.data.data(), cb.data.size());

  // Determine the number of bit-planes
  uint8_t bit_depth = 8;
  if (!siz_.components.empty()) {
    bit_depth = siz_.components[0].bit_depth;
  }

  int num_bp = static_cast<int>(bit_depth) + qcd_.num_guard_bits - 1;
  int first_bp = num_bp - cb.zero_bit_planes;
  if (first_bp < 0) first_bp = 0;

  // The coding passes cycle:
  // The very first pass is always a Cleanup pass
  // Then: Significance Propagation, Magnitude Refinement, Cleanup, ...
  int pass_idx = 0;
  int current_bp = first_bp;
  int pass_type = 2;  // 0=sig_prop, 1=mag_ref, 2=cleanup

  while (pass_idx < cb.num_passes && current_bp >= 0) {
    switch (pass_type) {
      case 0:
        significance_propagation_pass(mq, cb.coeffs, state,
                                      w, h, current_bp, subband_type);
        break;
      case 1:
        magnitude_refinement_pass(mq, cb.coeffs, state, w, h, current_bp);
        break;
      case 2:
        cleanup_pass(mq, cb.coeffs, state, w, h, current_bp, subband_type);
        break;
    }

    pass_idx++;
    pass_type++;
    if (pass_type > 2) {
      pass_type = 0;
      current_bp--;
    }
  }

  return true;
}

//-----------------------------------------------------------------------------
// Tile component building
//-----------------------------------------------------------------------------

void JPXDecoder::build_tile_component(TileComponent& tc, int comp_idx,
                                      int tile_x0, int tile_y0,
                                      int tile_x1, int tile_y1) {
  (void)comp_idx;
  int num_levels = cod_.num_decomp_levels;

  tc.x0 = tile_x0;
  tc.y0 = tile_y0;
  tc.width = tile_x1 - tile_x0;
  tc.height = tile_y1 - tile_y0;
  tc.coeffs.assign(static_cast<size_t>(tc.width) * tc.height, 0);

  int cb_width_exp = cod_.codeblock_width + 2;
  int cb_height_exp = cod_.codeblock_height + 2;
  int cb_width = 1 << cb_width_exp;
  int cb_height = 1 << cb_height_exp;

  tc.res_levels.resize(num_levels + 1);

  for (int r = 0; r <= num_levels; ++r) {
    ResLevel& res = tc.res_levels[r];
    res.level = r;

    int div = 1 << (num_levels - r);
    res.width = ceildiv(tc.width, div);
    res.height = ceildiv(tc.height, div);
    res.x0 = ceildiv(tile_x0, div);
    res.y0 = ceildiv(tile_y0, div);

    // Precinct sizes
    if (!cod_.precinct_sizes.empty() &&
        r < static_cast<int>(cod_.precinct_sizes.size())) {
      uint8_t ps = cod_.precinct_sizes[r];
      res.precinct_width = 1 << (ps & 0x0F);
      res.precinct_height = 1 << ((ps >> 4) & 0x0F);
    } else {
      res.precinct_width = 1 << 15;
      res.precinct_height = 1 << 15;
    }

    if (res.width <= 0 || res.height <= 0) {
      res.num_precincts_x = 0;
      res.num_precincts_y = 0;
      continue;
    }

    res.num_precincts_x = ceildiv(res.width, res.precinct_width);
    res.num_precincts_y = ceildiv(res.height, res.precinct_height);

    if (r == 0) {
      // Lowest resolution level: LL subband only
      Subband sb;
      sb.type = SubbandType::LL;
      sb.res_level = 0;
      sb.band_index = 0;
      sb.x0 = res.x0;
      sb.y0 = res.y0;
      sb.width = res.width;
      sb.height = res.height;

      if (sb.width > 0 && sb.height > 0) {
        sb.num_xcb = ceildiv(sb.width, cb_width);
        sb.num_ycb = ceildiv(sb.height, cb_height);
        sb.codeblocks.resize(
            static_cast<size_t>(sb.num_xcb) * sb.num_ycb);

        for (int cby = 0; cby < sb.num_ycb; ++cby) {
          for (int cbx = 0; cbx < sb.num_xcb; ++cbx) {
            Codeblock& blk = sb.codeblocks[cby * sb.num_xcb + cbx];
            blk.x0 = cbx * cb_width;
            blk.y0 = cby * cb_height;
            blk.width = std::min(cb_width, sb.width - blk.x0);
            blk.height = std::min(cb_height, sb.height - blk.y0);
          }
        }
      }

      res.subbands.push_back(sb);
    } else {
      // Higher resolution levels: HL, LH, HH
      int prev_div = 1 << (num_levels - r + 1);
      int ll_w = ceildiv(tc.width, prev_div);
      int ll_h = ceildiv(tc.height, prev_div);

      SubbandType types[3] = { SubbandType::HL, SubbandType::LH,
                                SubbandType::HH };

      for (int b = 0; b < 3; ++b) {
        Subband sb;
        sb.type = types[b];
        sb.res_level = r;
        sb.band_index = 1 + (r - 1) * 3 + b;

        if (b == 0) {
          sb.width = res.width - ll_w;
          sb.height = ll_h;
        } else if (b == 1) {
          sb.width = ll_w;
          sb.height = res.height - ll_h;
        } else {
          sb.width = res.width - ll_w;
          sb.height = res.height - ll_h;
        }

        if (sb.width <= 0 || sb.height <= 0) {
          sb.width = std::max(0, sb.width);
          sb.height = std::max(0, sb.height);
          sb.num_xcb = 0;
          sb.num_ycb = 0;
          res.subbands.push_back(sb);
          continue;
        }

        sb.num_xcb = ceildiv(sb.width, cb_width);
        sb.num_ycb = ceildiv(sb.height, cb_height);
        sb.codeblocks.resize(
            static_cast<size_t>(sb.num_xcb) * sb.num_ycb);

        for (int cby = 0; cby < sb.num_ycb; ++cby) {
          for (int cbx = 0; cbx < sb.num_xcb; ++cbx) {
            Codeblock& blk = sb.codeblocks[cby * sb.num_xcb + cbx];
            blk.x0 = cbx * cb_width;
            blk.y0 = cby * cb_height;
            blk.width = std::min(cb_width, sb.width - blk.x0);
            blk.height = std::min(cb_height, sb.height - blk.y0);
          }
        }

        res.subbands.push_back(sb);
      }
    }

    // Build precincts with tag trees
    int num_precincts = res.num_precincts_x * res.num_precincts_y;
    res.precincts.resize(num_precincts);

    for (int py = 0; py < res.num_precincts_y; ++py) {
      for (int px = 0; px < res.num_precincts_x; ++px) {
        Precinct& prec = res.precincts[py * res.num_precincts_x + px];
        prec.x0 = px * res.precinct_width;
        prec.y0 = py * res.precinct_height;
        prec.width = std::min(res.precinct_width, res.width - prec.x0);
        prec.height = std::min(res.precinct_height, res.height - prec.y0);

        prec.subbands.resize(res.subbands.size());

        for (size_t si = 0; si < res.subbands.size(); ++si) {
          const Subband& sb = res.subbands[si];
          Precinct::SubbandInfo& psi = prec.subbands[si];

          if (sb.num_xcb == 0 || sb.num_ycb == 0) {
            psi.num_xcb = 0;
            psi.num_ycb = 0;
            continue;
          }

          int cb_w = 1 << cb_width_exp;
          int cb_h = 1 << cb_height_exp;

          int prec_sb_x0, prec_sb_y0, prec_sb_x1, prec_sb_y1;
          if (r == 0) {
            prec_sb_x0 = prec.x0;
            prec_sb_y0 = prec.y0;
            prec_sb_x1 = prec.x0 + prec.width;
            prec_sb_y1 = prec.y0 + prec.height;
          } else {
            int half_px = prec.x0 / 2;
            int half_py = prec.y0 / 2;
            int half_pw = ceildiv(prec.width, 2);
            int half_ph = ceildiv(prec.height, 2);
            prec_sb_x0 = half_px;
            prec_sb_y0 = half_py;
            prec_sb_x1 = half_px + half_pw;
            prec_sb_y1 = half_py + half_ph;
          }

          prec_sb_x0 = std::max(0, prec_sb_x0);
          prec_sb_y0 = std::max(0, prec_sb_y0);
          prec_sb_x1 = std::min(sb.width, prec_sb_x1);
          prec_sb_y1 = std::min(sb.height, prec_sb_y1);

          psi.cb_x0 = (prec_sb_x0 > 0) ? (prec_sb_x0 / cb_w) : 0;
          psi.cb_y0 = (prec_sb_y0 > 0) ? (prec_sb_y0 / cb_h) : 0;
          int cb_x1 = std::min(sb.num_xcb,
              (prec_sb_x1 > 0) ? ceildiv(prec_sb_x1, cb_w) : 0);
          int cb_y1 = std::min(sb.num_ycb,
              (prec_sb_y1 > 0) ? ceildiv(prec_sb_y1, cb_h) : 0);

          psi.num_xcb = std::max(0, cb_x1 - psi.cb_x0);
          psi.num_ycb = std::max(0, cb_y1 - psi.cb_y0);

          if (psi.num_xcb > 0 && psi.num_ycb > 0) {
            psi.inclusion = TagTree(psi.num_xcb, psi.num_ycb);
            psi.zero_bit_planes = TagTree(psi.num_xcb, psi.num_ycb);
          }
        }
      }
    }
  }
}

//-----------------------------------------------------------------------------
// Packet header bit reader (handles bit-stuffing after 0xFF)
//-----------------------------------------------------------------------------

class PacketHeaderReader {
public:
  PacketHeaderReader(const uint8_t* data, size_t size, size_t offset)
      : data_(data), size_(size), pos_(offset), bit_pos_(8),
        current_byte_(0), max_bits_(8) {}

  int read_bit() {
    if (bit_pos_ >= max_bits_) {
      if (pos_ >= size_) return 0;
      current_byte_ = data_[pos_++];
      bit_pos_ = 0;
      // After 0xFF, next byte has only 7 valid bits
      if (pos_ >= 2 && data_[pos_ - 2] == 0xFF) {
        max_bits_ = 7;
      } else {
        max_bits_ = 8;
      }
    }
    int bit = (current_byte_ >> (max_bits_ - 1 - bit_pos_)) & 1;
    bit_pos_++;
    return bit;
  }

  int read_bits(int n) {
    int val = 0;
    for (int i = 0; i < n; ++i) {
      val = (val << 1) | read_bit();
    }
    return val;
  }

  void align() {
    if (bit_pos_ > 0 && bit_pos_ < max_bits_) {
      bit_pos_ = max_bits_;
    }
  }

  size_t byte_position() const { return pos_; }
  bool eof() const { return pos_ >= size_ && bit_pos_ >= max_bits_; }

private:
  const uint8_t* data_;
  size_t size_;
  size_t pos_;
  int bit_pos_;
  uint8_t current_byte_;
  int max_bits_;
};

//-----------------------------------------------------------------------------
// Packet parsing
//-----------------------------------------------------------------------------

bool JPXDecoder::parse_packet(const uint8_t* data, size_t size,
                              size_t& offset,
                              ResLevel& res, int layer_idx,
                              std::vector<Subband>& subbands) {
  if (offset >= size) return false;

  // Handle SOP marker
  if (cod_.use_sop && offset + 6 <= size) {
    if (data[offset] == 0xFF && data[offset + 1] == 0x91) {
      offset += 6;
    }
  }

  PacketHeaderReader phr(data, size, offset);

  // Read packet non-empty flag
  int non_empty = phr.read_bit();
  if (!non_empty) {
    phr.align();
    offset = phr.byte_position();
    if (cod_.use_eph && offset + 2 <= size) {
      if (data[offset] == 0xFF && data[offset + 1] == 0x92) {
        offset += 2;
      }
    }
    return true;
  }

  // For each precinct
  for (size_t pi = 0; pi < res.precincts.size(); ++pi) {
    Precinct& prec = res.precincts[pi];

    for (size_t si = 0; si < prec.subbands.size(); ++si) {
      Precinct::SubbandInfo& psi = prec.subbands[si];
      if (si >= subbands.size()) continue;
      Subband& sb = subbands[si];

      for (int cby = 0; cby < psi.num_ycb; ++cby) {
        for (int cbx = 0; cbx < psi.num_xcb; ++cbx) {
          int global_cbx = psi.cb_x0 + cbx;
          int global_cby = psi.cb_y0 + cby;

          if (global_cby >= sb.num_ycb || global_cbx >= sb.num_xcb) continue;

          Codeblock& cb = sb.codeblocks[global_cby * sb.num_xcb + global_cbx];

          if (!cb.included) {
            // First inclusion: read inclusion bit
            int inc_bit = phr.read_bit();
            if (!inc_bit) continue;

            cb.included = true;

            // Read zero bit-plane count
            int zbp = 0;
            while (phr.read_bit() == 0) {
              zbp++;
              if (zbp > 30) break;
            }
            cb.zero_bit_planes = zbp;
          } else {
            // Already included: check if new passes in this layer
            int inc_bit = phr.read_bit();
            if (!inc_bit) continue;
          }

          // Read number of new coding passes
          int num_new_passes = 0;
          int first_bit = phr.read_bit();
          if (first_bit == 0) {
            num_new_passes = 1;
          } else {
            int second_bit = phr.read_bit();
            if (second_bit == 0) {
              num_new_passes = 2;
            } else {
              int val = phr.read_bits(2);
              if (val < 3) {
                num_new_passes = 3 + val;
              } else {
                val = phr.read_bits(5);
                if (val < 31) {
                  num_new_passes = 6 + val;
                } else {
                  num_new_passes = 37 + phr.read_bits(7);
                }
              }
            }
          }

          // Read length bits increment
          int delta_len_bits = 0;
          while (phr.read_bit()) {
            delta_len_bits++;
            if (delta_len_bits > 10) break;
          }
          cb.num_len_bits += delta_len_bits;

          // Read codeblock data length
          int data_length = phr.read_bits(cb.num_len_bits);

          cb.pass_lengths.push_back(data_length);
          cb.num_passes += num_new_passes;
        }
      }
    }
  }

  phr.align();
  offset = phr.byte_position();

  // Handle EPH marker
  if (cod_.use_eph && offset + 2 <= size) {
    if (data[offset] == 0xFF && data[offset + 1] == 0x92) {
      offset += 2;
    }
  }

  // Read the actual codeblock compressed data
  for (size_t si = 0; si < subbands.size(); ++si) {
    Subband& sb = subbands[si];
    for (size_t cbi = 0; cbi < sb.codeblocks.size(); ++cbi) {
      Codeblock& cb = sb.codeblocks[cbi];
      if (cb.pass_lengths.empty()) continue;

      int last_len = cb.pass_lengths.back();
      if (last_len > 0 && offset + static_cast<size_t>(last_len) <= size) {
        size_t old_size = cb.data.size();
        cb.data.resize(old_size + last_len);
        memcpy(cb.data.data() + old_size, data + offset, last_len);
        offset += last_len;
      } else if (last_len > 0) {
        size_t available = (offset < size) ? (size - offset) : 0;
        if (available > 0) {
          size_t old_size = cb.data.size();
          cb.data.resize(old_size + available);
          memcpy(cb.data.data() + old_size, data + offset, available);
          offset += available;
        }
      }
    }
  }

  return true;
}

//-----------------------------------------------------------------------------
// Tile data decoding
//-----------------------------------------------------------------------------

bool JPXDecoder::decode_tile_data(const uint8_t* data, size_t size,
                                  std::vector<TileComponent>& tile_comps) {
  size_t offset = 0;
  int num_layers = cod_.num_layers;
  int num_res = cod_.num_decomp_levels + 1;
  int num_comps = static_cast<int>(tile_comps.size());

  // Decode packets according to progression order
  switch (cod_.prog_order) {
    case ProgressionOrder::LRCP:
      for (int l = 0; l < num_layers; ++l) {
        for (int r = 0; r < num_res; ++r) {
          for (int c = 0; c < num_comps; ++c) {
            if (r >= static_cast<int>(tile_comps[c].res_levels.size()))
              continue;
            ResLevel& res = tile_comps[c].res_levels[r];
            if (res.num_precincts_x == 0 || res.num_precincts_y == 0)
              continue;
            if (!parse_packet(data, size, offset, res, l, res.subbands)) {
              goto done_parsing;
            }
          }
        }
      }
      break;

    case ProgressionOrder::RLCP:
      for (int r = 0; r < num_res; ++r) {
        for (int l = 0; l < num_layers; ++l) {
          for (int c = 0; c < num_comps; ++c) {
            if (r >= static_cast<int>(tile_comps[c].res_levels.size()))
              continue;
            ResLevel& res = tile_comps[c].res_levels[r];
            if (res.num_precincts_x == 0 || res.num_precincts_y == 0)
              continue;
            if (!parse_packet(data, size, offset, res, l, res.subbands)) {
              goto done_parsing;
            }
          }
        }
      }
      break;

    default:
      // Fall back to LRCP
      for (int l = 0; l < num_layers; ++l) {
        for (int r = 0; r < num_res; ++r) {
          for (int c = 0; c < num_comps; ++c) {
            if (r >= static_cast<int>(tile_comps[c].res_levels.size()))
              continue;
            ResLevel& res = tile_comps[c].res_levels[r];
            if (res.num_precincts_x == 0 || res.num_precincts_y == 0)
              continue;
            if (!parse_packet(data, size, offset, res, l, res.subbands)) {
              goto done_parsing;
            }
          }
        }
      }
      break;
  }

done_parsing:

  // Decode all codeblocks and place coefficients
  for (int c = 0; c < num_comps; ++c) {
    TileComponent& tc = tile_comps[c];
    for (size_t ri = 0; ri < tc.res_levels.size(); ++ri) {
      ResLevel& res = tc.res_levels[ri];
      for (size_t si = 0; si < res.subbands.size(); ++si) {
        Subband& sb = res.subbands[si];
        int sb_type;
        switch (sb.type) {
          case SubbandType::HL: sb_type = 0; break;
          case SubbandType::LH: sb_type = 1; break;
          default: sb_type = 2; break;
        }

        for (size_t cbi = 0; cbi < sb.codeblocks.size(); ++cbi) {
          Codeblock& cb = sb.codeblocks[cbi];
          if (!cb.included || cb.data.empty()) continue;

          decode_codeblock(cb, sb_type);
          place_codeblock_coeffs(cb, sb, tc);
        }
      }
    }
  }

  return true;
}

//-----------------------------------------------------------------------------
// Place codeblock coefficients
//-----------------------------------------------------------------------------

void JPXDecoder::place_codeblock_coeffs(const Codeblock& cb,
                                        const Subband& sb,
                                        TileComponent& tc) {
  if (cb.coeffs.empty() || tc.coeffs.empty()) return;

  int num_levels = cod_.num_decomp_levels;
  int sb_offset_x = 0, sb_offset_y = 0;

  if (sb.res_level == 0) {
    sb_offset_x = 0;
    sb_offset_y = 0;
  } else {
    int r = sb.res_level;
    int ll_w = ceildiv(tc.width, 1 << (num_levels - r + 1));
    int ll_h = ceildiv(tc.height, 1 << (num_levels - r + 1));

    switch (sb.type) {
      case SubbandType::HL: sb_offset_x = ll_w; sb_offset_y = 0; break;
      case SubbandType::LH: sb_offset_x = 0; sb_offset_y = ll_h; break;
      case SubbandType::HH: sb_offset_x = ll_w; sb_offset_y = ll_h; break;
      default: break;
    }
  }

  for (int y = 0; y < cb.height; ++y) {
    for (int x = 0; x < cb.width; ++x) {
      int dst_x = sb_offset_x + cb.x0 + x;
      int dst_y = sb_offset_y + cb.y0 + y;
      if (dst_x < tc.width && dst_y < tc.height) {
        tc.coeffs[dst_y * tc.width + dst_x] = cb.coeffs[y * cb.width + x];
      }
    }
  }
}

//-----------------------------------------------------------------------------
// Dequantization
//-----------------------------------------------------------------------------

void JPXDecoder::dequantize_subband(Subband& sb, TileComponent& tc,
                                    int comp_idx) {
  if (qcd_.quant_style == 0) return;  // No quantization (reversible)

  int band_idx = sb.band_index;
  float step_size;

  uint8_t bd = siz_.components.empty()
                   ? 8
                   : siz_.components[comp_idx < static_cast<int>(
                                         siz_.components.size())
                                         ? comp_idx
                                         : 0]
                         .bit_depth;

  if (qcd_.quant_style == 1) {
    // Scalar derived
    if (qcd_.step_sizes.empty()) return;
    int base_exp = qcd_.step_sizes[0].exponent;
    int base_man = qcd_.step_sizes[0].mantissa;
    step_size = (1.0f + static_cast<float>(base_man) / 2048.0f) *
                std::pow(2.0f, static_cast<float>(base_exp) -
                         static_cast<float>(bd));
  } else {
    // Scalar expounded
    if (band_idx >= static_cast<int>(qcd_.step_sizes.size())) return;
    int exp = qcd_.step_sizes[band_idx].exponent;
    int man = qcd_.step_sizes[band_idx].mantissa;
    step_size = (1.0f + static_cast<float>(man) / 2048.0f) *
                std::pow(2.0f, static_cast<float>(exp) -
                         static_cast<float>(bd));
  }

  int num_levels = cod_.num_decomp_levels;
  int sb_offset_x = 0, sb_offset_y = 0;

  if (sb.res_level == 0) {
    sb_offset_x = 0;
    sb_offset_y = 0;
  } else {
    int r = sb.res_level;
    int ll_w = ceildiv(tc.width, 1 << (num_levels - r + 1));
    int ll_h = ceildiv(tc.height, 1 << (num_levels - r + 1));
    switch (sb.type) {
      case SubbandType::HL: sb_offset_x = ll_w; sb_offset_y = 0; break;
      case SubbandType::LH: sb_offset_x = 0; sb_offset_y = ll_h; break;
      case SubbandType::HH: sb_offset_x = ll_w; sb_offset_y = ll_h; break;
      default: break;
    }
  }

  for (int y = 0; y < sb.height; ++y) {
    for (int x = 0; x < sb.width; ++x) {
      int dx = sb_offset_x + x;
      int dy = sb_offset_y + y;
      if (dx < tc.width && dy < tc.height) {
        int idx = dy * tc.width + dx;
        float val = static_cast<float>(tc.coeffs[idx]) * step_size;
        tc.coeffs[idx] = static_cast<int32_t>(
            val >= 0 ? val + 0.5f : val - 0.5f);
      }
    }
  }
}

//-----------------------------------------------------------------------------
// JPXDecoder main implementation
//-----------------------------------------------------------------------------

JPXDecoder::JPXDecoder() {}
JPXDecoder::~JPXDecoder() {}

bool JPXDecoder::get_info(const uint8_t* data, size_t size,
                          uint32_t& width, uint32_t& height,
                          uint16_t& num_components, uint8_t& bit_depth) {
  if (size < 4) return false;

  if (data[0] == 0x00 && data[1] == 0x00 && data[2] == 0x00 &&
      data[3] == 0x0C) {
    size_t pos = 0;
    while (pos + 8 <= size) {
      uint32_t box_len = (data[pos] << 24) | (data[pos + 1] << 16) |
                         (data[pos + 2] << 8) | data[pos + 3];
      uint32_t box_type = (data[pos + 4] << 24) | (data[pos + 5] << 16) |
                          (data[pos + 6] << 8) | data[pos + 7];
      if (box_type == 0x6A703263) {
        data = data + pos + 8;
        size = box_len - 8;
        break;
      }
      if (box_len == 0) break;
      pos += box_len;
    }
  }

  if (size < 2 || data[0] != 0xFF || data[1] != 0x4F) return false;

  BitReader reader(data, size);
  reader.read_bits(16);  // Skip SOC

  while (!reader.eof()) {
    if (reader.position() + 2 > size) break;
    uint16_t marker = static_cast<uint16_t>(reader.read_bits(16));
    if (marker == JPX_SIZ) {
      uint16_t len = static_cast<uint16_t>(reader.read_bits(16));
      (void)len;
      reader.read_bits(16);  // Rsiz
      width = reader.read_bits(32);
      height = reader.read_bits(32);
      reader.read_bits(32);  // X offset
      reader.read_bits(32);  // Y offset
      reader.read_bits(32);  // Tile width
      reader.read_bits(32);  // Tile height
      reader.read_bits(32);  // Tile X offset
      reader.read_bits(32);  // Tile Y offset
      num_components = static_cast<uint16_t>(reader.read_bits(16));
      if (num_components > 0) {
        uint8_t ssiz = static_cast<uint8_t>(reader.read_bits(8));
        bit_depth = (ssiz & 0x7F) + 1;
      }
      return true;
    } else if ((marker & 0xFF00) == 0xFF00 && marker != JPX_SOC) {
      uint16_t len = static_cast<uint16_t>(reader.read_bits(16));
      for (int i = 0; i < len - 2; ++i) {
        reader.read_bits(8);
      }
    } else {
      break;
    }
  }
  return false;
}

DecodeResult JPXDecoder::decode(const uint8_t* data, size_t size) {
  DecodeResult result;

  if (size < 4) {
    result.error = "Data too small for JPEG2000";
    return result;
  }

  const uint8_t* cs_data = data;
  size_t cs_size = size;

  if (data[0] == 0x00 && data[1] == 0x00 && data[2] == 0x00) {
    size_t pos = 0;
    while (pos + 8 <= size) {
      uint32_t box_len = (data[pos] << 24) | (data[pos + 1] << 16) |
                         (data[pos + 2] << 8) | data[pos + 3];
      uint32_t box_type = (data[pos + 4] << 24) | (data[pos + 5] << 16) |
                          (data[pos + 6] << 8) | data[pos + 7];
      if (box_type == 0x6A703263) {
        cs_data = data + pos + 8;
        cs_size = (box_len == 1) ? (size - pos - 8) : (box_len - 8);
        break;
      }
      if (box_len == 0) break;
      if (box_len == 1) {
        if (pos + 16 > size) break;
        pos += 16;
        continue;
      }
      pos += box_len;
    }
  }

  codestream_data_ = cs_data;
  codestream_size_ = cs_size;

  if (cs_size < 2 || cs_data[0] != 0xFF || cs_data[1] != 0x4F) {
    result.error = "Invalid JPEG2000 codestream (missing SOC)";
    return result;
  }

  BitReader reader(cs_data, cs_size);
  reader.read_bits(16);  // Skip SOC

  if (!parse_main_header(reader)) {
    result.error = "Failed to parse JPEG2000 main header";
    return result;
  }

#ifdef NANOPDF_DEBUG_PRINT
  printf("JPX: Image %ux%u, %u components, %u bit, %u decomp levels\n",
         siz_.width, siz_.height, siz_.num_components,
         siz_.components.empty() ? 8 : siz_.components[0].bit_depth,
         cod_.num_decomp_levels);
  printf("JPX: Wavelet: %s, MCT: %d, Layers: %u\n",
         cod_.wavelet == WaveletType::Reversible_5_3 ? "5/3" : "9/7",
         cod_.mct, cod_.num_layers);
  printf("JPX: Codeblock: %dx%d, Quant style: %d\n",
         1 << (cod_.codeblock_width + 2), 1 << (cod_.codeblock_height + 2),
         qcd_.quant_style);
#endif

  tile_coeffs_.resize(siz_.num_components);
  for (uint16_t c = 0; c < siz_.num_components; ++c) {
    tile_coeffs_[c].resize(
        static_cast<size_t>(siz_.width) * siz_.height, 0);
  }

  while (!reader.eof()) {
    size_t pos = reader.position();
    if (pos + 2 > cs_size) break;

    uint16_t marker = (cs_data[pos] << 8) | cs_data[pos + 1];

    if (marker == JPX_EOC) {
      break;
    } else if (marker == JPX_SOT) {
      if (!parse_tile_part(reader)) {
#ifdef NANOPDF_DEBUG_PRINT
        printf("JPX: Warning - tile part parsing failed\n");
#endif
      }
    } else {
      reader.read_bits(16);
      if (pos + 4 <= cs_size) {
        uint16_t len = (cs_data[pos + 2] << 8) | cs_data[pos + 3];
        reader.seek(pos + 2 + len);
      } else {
        break;
      }
    }
  }

  // Apply inverse MCT if needed
  if (cod_.mct && siz_.num_components >= 3) {
    apply_inverse_mct(tile_coeffs_);
  }

  result.width = siz_.width;
  result.height = siz_.height;
  result.num_components = siz_.num_components;
  result.bit_depth = siz_.components.empty() ? 8
                                             : siz_.components[0].bit_depth;

  coeffs_to_pixels(tile_coeffs_, result.pixels);
  result.success = true;

  return result;
}

//-----------------------------------------------------------------------------
// Header parsing
//-----------------------------------------------------------------------------

bool JPXDecoder::parse_main_header(BitReader& reader) {
  while (!reader.eof()) {
    size_t pos = reader.position();
    uint16_t marker = static_cast<uint16_t>(reader.read_bits(16));

    if (marker == JPX_SOT) {
      reader.seek(pos);
      return true;
    }

    if ((marker & 0xFF00) != 0xFF00) return false;

    uint16_t length = static_cast<uint16_t>(reader.read_bits(16));

    switch (marker) {
      case JPX_SIZ:
        if (!parse_siz(reader, length)) return false;
        break;
      case JPX_COD:
        if (!parse_cod(reader, length)) return false;
        break;
      case JPX_QCD:
        if (!parse_qcd(reader, length)) return false;
        break;
      default:
        for (int i = 0; i < length - 2; ++i) {
          reader.read_bits(8);
        }
        break;
    }
  }
  return siz_.width > 0 && siz_.height > 0;
}

bool JPXDecoder::parse_siz(BitReader& reader, uint16_t length) {
  (void)length;
  siz_.rsiz = static_cast<uint16_t>(reader.read_bits(16));
  siz_.width = reader.read_bits(32);
  siz_.height = reader.read_bits(32);
  siz_.x_offset = reader.read_bits(32);
  siz_.y_offset = reader.read_bits(32);
  siz_.tile_width = reader.read_bits(32);
  siz_.tile_height = reader.read_bits(32);
  siz_.tile_x_offset = reader.read_bits(32);
  siz_.tile_y_offset = reader.read_bits(32);
  siz_.num_components = static_cast<uint16_t>(reader.read_bits(16));

  siz_.components.resize(siz_.num_components);
  for (uint16_t i = 0; i < siz_.num_components; ++i) {
    uint8_t ssiz = static_cast<uint8_t>(reader.read_bits(8));
    siz_.components[i].is_signed = (ssiz & 0x80) != 0;
    siz_.components[i].bit_depth = (ssiz & 0x7F) + 1;
    siz_.components[i].x_separation =
        static_cast<uint8_t>(reader.read_bits(8));
    siz_.components[i].y_separation =
        static_cast<uint8_t>(reader.read_bits(8));
  }
  return true;
}

bool JPXDecoder::parse_cod(BitReader& reader, uint16_t length) {
  (void)length;
  uint8_t scod = static_cast<uint8_t>(reader.read_bits(8));
  cod_.use_sop = (scod & 0x02) != 0;
  cod_.use_eph = (scod & 0x04) != 0;

  uint8_t prog = static_cast<uint8_t>(reader.read_bits(8));
  cod_.prog_order = static_cast<ProgressionOrder>(prog);
  cod_.num_layers = static_cast<uint16_t>(reader.read_bits(16));
  cod_.mct = static_cast<uint8_t>(reader.read_bits(8));
  cod_.num_decomp_levels = static_cast<uint8_t>(reader.read_bits(8));
  cod_.codeblock_width = static_cast<uint8_t>(reader.read_bits(8));
  cod_.codeblock_height = static_cast<uint8_t>(reader.read_bits(8));
  cod_.codeblock_style = static_cast<uint8_t>(reader.read_bits(8));

  uint8_t wavelet = static_cast<uint8_t>(reader.read_bits(8));
  cod_.wavelet = (wavelet == 1) ? WaveletType::Reversible_5_3
                                : WaveletType::Irreversible_9_7;

  if (scod & 0x01) {
    cod_.precinct_sizes.resize(cod_.num_decomp_levels + 1);
    for (int i = 0; i <= cod_.num_decomp_levels; ++i) {
      cod_.precinct_sizes[i] = static_cast<uint8_t>(reader.read_bits(8));
    }
  }
  return true;
}

bool JPXDecoder::parse_qcd(BitReader& reader, uint16_t length) {
  uint8_t sqcd = static_cast<uint8_t>(reader.read_bits(8));
  qcd_.quant_style = sqcd & 0x1F;
  qcd_.num_guard_bits = (sqcd >> 5) & 0x07;

  int remaining = length - 3;

  if (qcd_.quant_style == 0) {
    qcd_.step_sizes.resize(remaining);
    for (int i = 0; i < remaining; ++i) {
      uint8_t val = static_cast<uint8_t>(reader.read_bits(8));
      qcd_.step_sizes[i].exponent = val >> 3;
      qcd_.step_sizes[i].mantissa = 0;
    }
  } else if (qcd_.quant_style == 1) {
    uint16_t val = static_cast<uint16_t>(reader.read_bits(16));
    qcd_.step_sizes.resize(1);
    qcd_.step_sizes[0].exponent = (val >> 11) & 0x1F;
    qcd_.step_sizes[0].mantissa = val & 0x7FF;
  } else {
    int num_steps = remaining / 2;
    qcd_.step_sizes.resize(num_steps);
    for (int i = 0; i < num_steps; ++i) {
      uint16_t val = static_cast<uint16_t>(reader.read_bits(16));
      qcd_.step_sizes[i].exponent = (val >> 11) & 0x1F;
      qcd_.step_sizes[i].mantissa = val & 0x7FF;
    }
  }
  return true;
}

//-----------------------------------------------------------------------------
// Tile part parsing and decoding
//-----------------------------------------------------------------------------

bool JPXDecoder::parse_tile_part(BitReader& reader) {
  size_t sot_marker_pos = reader.position();
  reader.read_bits(16);  // SOT marker
  uint16_t lsot = static_cast<uint16_t>(reader.read_bits(16));
  (void)lsot;

  TilePartHeader tph;
  tph.tile_index = static_cast<uint16_t>(reader.read_bits(16));
  tph.tile_part_length = reader.read_bits(32);
  tph.tile_part_index = static_cast<uint8_t>(reader.read_bits(8));
  tph.num_tile_parts = static_cast<uint8_t>(reader.read_bits(8));

  tile_parts_.push_back(tph);

  // The SOT marker segment is 12 bytes total:
  // marker(2) + Lsot(2) + Isot(2) + Psot(4) + TPsot(1) + TNsot(1)
  // sot_marker_pos is where the marker begins

  // Skip any tile-part header markers between SOT and SOD
  while (!reader.eof()) {
    size_t pos = reader.position();
    uint16_t marker = static_cast<uint16_t>(reader.read_bits(16));

    if (marker == JPX_SOD) {
      break;
    } else if ((marker & 0xFF00) == 0xFF00) {
      uint16_t seg_len = static_cast<uint16_t>(reader.read_bits(16));
      for (int i = 0; i < seg_len - 2; ++i) {
        reader.read_bits(8);
      }
    } else {
      reader.seek(pos);
      break;
    }
  }

  size_t data_start = reader.position();

  // Compute tile data size from Psot
  size_t tile_data_size;
  if (tph.tile_part_length > 0) {
    size_t tile_end = sot_marker_pos + tph.tile_part_length;
    if (tile_end > codestream_size_) tile_end = codestream_size_;
    tile_data_size = (tile_end > data_start) ? (tile_end - data_start) : 0;
  } else {
    // Psot == 0: last tile-part, extends to EOC
    tile_data_size = codestream_size_ - data_start;
    for (size_t i = data_start; i + 1 < codestream_size_; ++i) {
      if (codestream_data_[i] == 0xFF && codestream_data_[i + 1] == 0xD9) {
        tile_data_size = i - data_start;
        break;
      }
    }
  }

  const uint8_t* tile_data = codestream_data_ + data_start;

#ifdef NANOPDF_DEBUG_PRINT
  printf("JPX: Tile %u, part %u, data size: %zu bytes\n",
         tph.tile_index, tph.tile_part_index, tile_data_size);
#endif

  // Compute tile boundaries
  int num_tiles_x = ceildiv(
      static_cast<int>(siz_.width - siz_.tile_x_offset),
      static_cast<int>(siz_.tile_width));
  if (num_tiles_x <= 0) num_tiles_x = 1;
  int tile_col = tph.tile_index % num_tiles_x;
  int tile_row = tph.tile_index / num_tiles_x;

  int tile_x0 = static_cast<int>(siz_.tile_x_offset) +
                 tile_col * static_cast<int>(siz_.tile_width);
  int tile_y0 = static_cast<int>(siz_.tile_y_offset) +
                 tile_row * static_cast<int>(siz_.tile_height);
  int tile_x1 = std::min(tile_x0 + static_cast<int>(siz_.tile_width),
                         static_cast<int>(siz_.width));
  int tile_y1 = std::min(tile_y0 + static_cast<int>(siz_.tile_height),
                         static_cast<int>(siz_.height));

  tile_x0 = std::max(tile_x0, static_cast<int>(siz_.x_offset));
  tile_y0 = std::max(tile_y0, static_cast<int>(siz_.y_offset));

  if (tile_x1 <= tile_x0 || tile_y1 <= tile_y0) {
    reader.seek(data_start + tile_data_size);
    return true;
  }

  // Build tile component structures
  int num_comps = siz_.num_components;
  std::vector<TileComponent> tile_comps(num_comps);

  for (int c = 0; c < num_comps; ++c) {
    build_tile_component(tile_comps[c], c, tile_x0, tile_y0,
                         tile_x1, tile_y1);
  }

  // Decode tile data
  if (tile_data_size > 0) {
    decode_tile_data(tile_data, tile_data_size, tile_comps);
  }

  // Apply dequantization for irreversible wavelet
  if (cod_.wavelet == WaveletType::Irreversible_9_7) {
    for (int c = 0; c < num_comps; ++c) {
      TileComponent& tc = tile_comps[c];
      for (size_t ri = 0; ri < tc.res_levels.size(); ++ri) {
        ResLevel& res = tc.res_levels[ri];
        for (size_t si = 0; si < res.subbands.size(); ++si) {
          dequantize_subband(res.subbands[si], tc, c);
        }
      }
    }
  }

  // Apply inverse DWT
  for (int c = 0; c < num_comps; ++c) {
    TileComponent& tc = tile_comps[c];
    int tw = tc.width;
    int th = tc.height;
    int levels = cod_.num_decomp_levels;

    if (tw <= 0 || th <= 0) continue;

    if (cod_.wavelet == WaveletType::Reversible_5_3) {
      inverse_dwt_53(tc.coeffs, tw, th, levels);
    } else {
      std::vector<float> float_coeffs(static_cast<size_t>(tw) * th);
      for (size_t i = 0; i < float_coeffs.size(); ++i) {
        float_coeffs[i] = static_cast<float>(tc.coeffs[i]);
      }
      inverse_dwt_97(float_coeffs, tw, th, levels);
      for (size_t i = 0; i < float_coeffs.size(); ++i) {
        tc.coeffs[i] = static_cast<int32_t>(
            float_coeffs[i] >= 0 ? float_coeffs[i] + 0.5f
                                 : float_coeffs[i] - 0.5f);
      }
    }

    // Copy reconstructed samples into output, applying DC level shift
    bool is_signed = (!siz_.components.empty() && c < num_comps)
                         ? siz_.components[c].is_signed
                         : false;
    uint8_t bit_depth = (!siz_.components.empty() && c < num_comps)
                            ? siz_.components[c].bit_depth
                            : 8;
    int dc_offset = is_signed ? 0 : (1 << (bit_depth - 1));

    for (int y = 0; y < th; ++y) {
      for (int x = 0; x < tw; ++x) {
        int src_idx = y * tw + x;
        int dst_x = tile_x0 + x - static_cast<int>(siz_.x_offset);
        int dst_y = tile_y0 + y - static_cast<int>(siz_.y_offset);
        if (dst_x >= 0 && dst_x < static_cast<int>(siz_.width) &&
            dst_y >= 0 && dst_y < static_cast<int>(siz_.height)) {
          int dst_idx = dst_y * static_cast<int>(siz_.width) + dst_x;
          tile_coeffs_[c][dst_idx] = tc.coeffs[src_idx] + dc_offset;
        }
      }
    }
  }

  reader.seek(data_start + tile_data_size);
  return true;
}

//-----------------------------------------------------------------------------
// Inverse DWT implementations
//-----------------------------------------------------------------------------

void JPXDecoder::inverse_dwt_53(std::vector<int32_t>& data, int width,
                                int height, int levels) {
  if (levels <= 0 || width <= 1 || height <= 1) return;

  std::vector<int32_t> temp(std::max(width, height));

  for (int level = levels - 1; level >= 0; --level) {
    int w = ceildiv(width, 1 << level);
    int h = ceildiv(height, 1 << level);
    if (w <= 1 && h <= 1) continue;

    int half_w = ceildiv(w, 2);
    int half_h = ceildiv(h, 2);

    // Vertical synthesis
    if (h > 1) {
      for (int x = 0; x < w; ++x) {
        for (int y = 0; y < h; ++y) {
          temp[y] = data[y * width + x];
        }

        int h_high = h - half_h;

        // Inverse lifting for 5/3:
        // Step 1: undo update (even samples)
        if (h_high > 0) {
          temp[0] -= ((temp[half_h] + temp[half_h] + 2) >> 2);
          for (int n = 1; n < half_h; ++n) {
            int d_prev = (n - 1 < h_high) ? temp[half_h + n - 1]
                                           : temp[half_h + h_high - 1];
            int d_curr = (n < h_high) ? temp[half_h + n]
                                      : temp[half_h + h_high - 1];
            temp[n] -= ((d_prev + d_curr + 2) >> 2);
          }
        }

        // Step 2: undo predict (odd samples)
        for (int n = 0; n < h_high; ++n) {
          int s_curr = temp[n];
          int s_next = (n + 1 < half_h) ? temp[n + 1] : temp[half_h - 1];
          temp[half_h + n] += ((s_curr + s_next) >> 1);
        }

        // Interleave
        std::vector<int32_t> interleaved(h);
        for (int n = 0; n < half_h; ++n) {
          interleaved[2 * n] = temp[n];
        }
        for (int n = 0; n < h_high; ++n) {
          interleaved[2 * n + 1] = temp[half_h + n];
        }

        for (int y = 0; y < h; ++y) {
          data[y * width + x] = interleaved[y];
        }
      }
    }

    // Horizontal synthesis
    if (w > 1) {
      for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
          temp[x] = data[y * width + x];
        }

        int w_high = w - half_w;

        if (w_high > 0) {
          temp[0] -= ((temp[half_w] + temp[half_w] + 2) >> 2);
          for (int n = 1; n < half_w; ++n) {
            int d_prev = (n - 1 < w_high) ? temp[half_w + n - 1]
                                           : temp[half_w + w_high - 1];
            int d_curr = (n < w_high) ? temp[half_w + n]
                                      : temp[half_w + w_high - 1];
            temp[n] -= ((d_prev + d_curr + 2) >> 2);
          }
        }

        for (int n = 0; n < w_high; ++n) {
          int s_curr = temp[n];
          int s_next = (n + 1 < half_w) ? temp[n + 1] : temp[half_w - 1];
          temp[half_w + n] += ((s_curr + s_next) >> 1);
        }

        std::vector<int32_t> interleaved(w);
        for (int n = 0; n < half_w; ++n) {
          interleaved[2 * n] = temp[n];
        }
        for (int n = 0; n < w_high; ++n) {
          interleaved[2 * n + 1] = temp[half_w + n];
        }

        for (int x = 0; x < w; ++x) {
          data[y * width + x] = interleaved[x];
        }
      }
    }
  }
}

void JPXDecoder::inverse_dwt_97(std::vector<float>& data, int width,
                                int height, int levels) {
  if (levels <= 0 || width <= 1 || height <= 1) return;

  std::vector<float> temp(std::max(width, height));

  for (int level = levels - 1; level >= 0; --level) {
    int w = ceildiv(width, 1 << level);
    int h = ceildiv(height, 1 << level);
    if (w <= 1 && h <= 1) continue;

    int half_w = ceildiv(w, 2);
    int half_h = ceildiv(h, 2);

    // Vertical synthesis
    if (h > 1) {
      for (int x = 0; x < w; ++x) {
        for (int y = 0; y < h; ++y) {
          temp[y] = data[y * width + x];
        }

        int h_high = h - half_h;

        // Undo scaling
        for (int n = 0; n < half_h; ++n) temp[n] /= lift_97_k;
        for (int n = 0; n < h_high; ++n) temp[half_h + n] *= lift_97_k;

        // Step 4: undo delta (even/update)
        for (int n = 0; n < half_h; ++n) {
          float d0 = (n < h_high) ? temp[half_h + n]
                                  : temp[half_h + h_high - 1];
          float d1 = (n > 0) ? temp[half_h + n - 1] : d0;
          temp[n] -= lift_97_delta * (d1 + d0);
        }

        // Step 3: undo gamma (odd/predict)
        for (int n = 0; n < h_high; ++n) {
          float s0 = temp[n];
          float s1 = (n + 1 < half_h) ? temp[n + 1] : temp[half_h - 1];
          temp[half_h + n] -= lift_97_gamma * (s0 + s1);
        }

        // Step 2: undo beta (even/update)
        for (int n = 0; n < half_h; ++n) {
          float d0 = (n < h_high) ? temp[half_h + n]
                                  : temp[half_h + h_high - 1];
          float d1 = (n > 0) ? temp[half_h + n - 1] : d0;
          temp[n] -= lift_97_beta * (d1 + d0);
        }

        // Step 1: undo alpha (odd/predict)
        for (int n = 0; n < h_high; ++n) {
          float s0 = temp[n];
          float s1 = (n + 1 < half_h) ? temp[n + 1] : temp[half_h - 1];
          temp[half_h + n] -= lift_97_alpha * (s0 + s1);
        }

        // Interleave
        std::vector<float> interleaved(h);
        for (int n = 0; n < half_h; ++n) interleaved[2 * n] = temp[n];
        for (int n = 0; n < h_high; ++n)
          interleaved[2 * n + 1] = temp[half_h + n];

        for (int y = 0; y < h; ++y) data[y * width + x] = interleaved[y];
      }
    }

    // Horizontal synthesis
    if (w > 1) {
      for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) temp[x] = data[y * width + x];

        int w_high = w - half_w;

        for (int n = 0; n < half_w; ++n) temp[n] /= lift_97_k;
        for (int n = 0; n < w_high; ++n) temp[half_w + n] *= lift_97_k;

        for (int n = 0; n < half_w; ++n) {
          float d0 = (n < w_high) ? temp[half_w + n]
                                  : temp[half_w + w_high - 1];
          float d1 = (n > 0) ? temp[half_w + n - 1] : d0;
          temp[n] -= lift_97_delta * (d1 + d0);
        }

        for (int n = 0; n < w_high; ++n) {
          float s0 = temp[n];
          float s1 = (n + 1 < half_w) ? temp[n + 1] : temp[half_w - 1];
          temp[half_w + n] -= lift_97_gamma * (s0 + s1);
        }

        for (int n = 0; n < half_w; ++n) {
          float d0 = (n < w_high) ? temp[half_w + n]
                                  : temp[half_w + w_high - 1];
          float d1 = (n > 0) ? temp[half_w + n - 1] : d0;
          temp[n] -= lift_97_beta * (d1 + d0);
        }

        for (int n = 0; n < w_high; ++n) {
          float s0 = temp[n];
          float s1 = (n + 1 < half_w) ? temp[n + 1] : temp[half_w - 1];
          temp[half_w + n] -= lift_97_alpha * (s0 + s1);
        }

        std::vector<float> interleaved(w);
        for (int n = 0; n < half_w; ++n) interleaved[2 * n] = temp[n];
        for (int n = 0; n < w_high; ++n)
          interleaved[2 * n + 1] = temp[half_w + n];

        for (int x = 0; x < w; ++x) data[y * width + x] = interleaved[x];
      }
    }
  }
}

//-----------------------------------------------------------------------------
// Color transform and pixel conversion
//-----------------------------------------------------------------------------

void JPXDecoder::apply_inverse_mct(
    std::vector<std::vector<int32_t>>& components) {
  if (components.size() < 3) return;

  size_t num_pixels = components[0].size();

  if (cod_.wavelet == WaveletType::Reversible_5_3) {
    // RCT (Reversible Color Transform)
    for (size_t i = 0; i < num_pixels; ++i) {
      int32_t y = components[0][i];
      int32_t cb = components[1][i];
      int32_t cr = components[2][i];

      int32_t g = y - ((cb + cr) >> 2);
      int32_t r = cr + g;
      int32_t b = cb + g;

      components[0][i] = r;
      components[1][i] = g;
      components[2][i] = b;
    }
  } else {
    // ICT (Irreversible Color Transform)
    for (size_t i = 0; i < num_pixels; ++i) {
      float y = static_cast<float>(components[0][i]);
      float cb = static_cast<float>(components[1][i]);
      float cr = static_cast<float>(components[2][i]);

      float r = y + 1.402f * cr;
      float g = y - 0.34413f * cb - 0.71414f * cr;
      float b = y + 1.772f * cb;

      components[0][i] = static_cast<int32_t>(r >= 0 ? r + 0.5f : r - 0.5f);
      components[1][i] = static_cast<int32_t>(g >= 0 ? g + 0.5f : g - 0.5f);
      components[2][i] = static_cast<int32_t>(b >= 0 ? b + 0.5f : b - 0.5f);
    }
  }
}

void JPXDecoder::coeffs_to_pixels(
    const std::vector<std::vector<int32_t>>& components,
    std::vector<uint8_t>& pixels) {
  if (components.empty() || components[0].empty()) return;

  size_t num_pixels = static_cast<size_t>(siz_.width) * siz_.height;
  size_t num_components = components.size();
  pixels.resize(num_pixels * num_components);

  for (size_t i = 0; i < num_pixels; ++i) {
    for (size_t c = 0; c < num_components; ++c) {
      int32_t val = components[c][i];

      uint8_t bit_depth =
          (c < siz_.components.size()) ? siz_.components[c].bit_depth : 8;

      if (bit_depth > 8) {
        val >>= (bit_depth - 8);
      } else if (bit_depth < 8) {
        val <<= (8 - bit_depth);
      }

      if (val < 0) val = 0;
      if (val > 255) val = 255;

      pixels[i * num_components + c] = static_cast<uint8_t>(val);
    }
  }
}

}  // namespace jpx
}  // namespace nanopdf
