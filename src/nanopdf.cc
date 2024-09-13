#if defined(NANOPDF_USE_NANOSTL)
#include "nanocstring.h"
#include "nanostring.h"
#else
#include <cstring>
#include <iostream>
#include <string>
#endif

#include "common-macros.inc"
#include "nanopdf.hh"

namespace nanopdf {

namespace {

static inline void swap2(unsigned short *val) {
  unsigned short tmp = *val;
  uint8_t *dst = reinterpret_cast<uint8_t *>(val);
  uint8_t *src = reinterpret_cast<uint8_t *>(&tmp);

  dst[0] = src[1];
  dst[1] = src[0];
}

static inline void swap4(uint32_t *val) {
  uint32_t tmp = *val;
  uint8_t *dst = reinterpret_cast<uint8_t *>(val);
  uint8_t *src = reinterpret_cast<uint8_t *>(&tmp);

  dst[0] = src[3];
  dst[1] = src[2];
  dst[2] = src[1];
  dst[3] = src[0];
}

static inline void swap4(int *val) {
  int tmp = *val;
  uint8_t *dst = reinterpret_cast<uint8_t *>(val);
  uint8_t *src = reinterpret_cast<uint8_t *>(&tmp);

  dst[0] = src[3];
  dst[1] = src[2];
  dst[2] = src[1];
  dst[3] = src[0];
}

static inline void swap8(uint64_t *val) {
  uint64_t tmp = (*val);
  uint8_t *dst = reinterpret_cast<uint8_t *>(val);
  uint8_t *src = reinterpret_cast<uint8_t *>(&tmp);

  dst[0] = src[7];
  dst[1] = src[6];
  dst[2] = src[5];
  dst[3] = src[4];
  dst[4] = src[3];
  dst[5] = src[2];
  dst[6] = src[1];
  dst[7] = src[0];
}

static inline void swap8(int64_t *val) {
  int64_t tmp = (*val);
  uint8_t *dst = reinterpret_cast<uint8_t *>(val);
  uint8_t *src = reinterpret_cast<uint8_t *>(&tmp);

  dst[0] = src[7];
  dst[1] = src[6];
  dst[2] = src[5];
  dst[3] = src[4];
  dst[4] = src[3];
  dst[5] = src[2];
  dst[6] = src[1];
  dst[7] = src[0];
}

}  // namespace

///
/// Simple stream reader
///
class StreamReader {
 public:
  explicit StreamReader(const uint8_t *binary, const uint64_t length,
                        const bool swap_endian)
      : binary_(binary), length_(length), swap_endian_(swap_endian), idx_(0) {
    (void)pad_;
  }

  bool seek_set(const uint64_t offset) const {
    if (offset > length_) {
      return false;
    }

    idx_ = offset;
    return true;
  }

  bool seek_from_current(const int64_t offset) const {
    if ((int64_t(idx_) + offset) < 0) {
      return false;
    }

    if (uint64_t((int64_t(idx_) + offset)) > length_) {
      return false;
    }

    idx_ = uint64_t(int64_t(idx_) + offset);
    return true;
  }

  uint64_t read(const uint64_t n, const uint64_t dst_len, uint8_t *dst) const {
    uint64_t len = n;
    if ((idx_ + len) > length_) {
      len = length_ - uint64_t(idx_);
    }

    if (len > 0) {
      if (dst_len < len) {
        // dst does not have enough space. return 0 for a while.
        return 0;
      }

      size_t nbytes = size_t(len);  // may shorten size on 32bit platform

      memcpy(dst, &binary_[idx_], nbytes);
      idx_ += nbytes;
      return nbytes;

    } else {
      return 0;
    }
  }

  bool read1(uint8_t *ret) const {
    if ((idx_ + 1) > length_) {
      return false;
    }

    const uint8_t val = binary_[idx_];

    (*ret) = val;
    idx_ += 1;

    return true;
  }

  bool read_bool(bool *ret) const {
    if ((idx_ + 1) > length_) {
      return false;
    }

    const char val = static_cast<const char>(binary_[idx_]);

    (*ret) = bool(val);
    idx_ += 1;

    return true;
  }

  bool read1(char *ret) const {
    if ((idx_ + 1) > length_) {
      return false;
    }

    const char val = static_cast<const char>(binary_[idx_]);

    (*ret) = val;
    idx_ += 1;

    return true;
  }

  bool read2(unsigned short *ret) const {
    if ((idx_ + 2) > length_) {
      return false;
    }

    unsigned short val;
    memcpy(&val, &binary_[idx_], sizeof(val));

    if (swap_endian_) {
      swap2(&val);
    }

    (*ret) = val;
    idx_ += 2;

    return true;
  }

  bool read4(uint32_t *ret) const {
    if ((idx_ + 4) > length_) {
      return false;
    }

    uint32_t val;
    memcpy(&val, &binary_[idx_], sizeof(val));

    if (swap_endian_) {
      swap4(&val);
    }

    (*ret) = val;
    idx_ += 4;

    return true;
  }

  bool read4(int *ret) const {
    if ((idx_ + 4) > length_) {
      return false;
    }

    int val;
    memcpy(&val, &binary_[idx_], sizeof(val));

    if (swap_endian_) {
      swap4(&val);
    }

    (*ret) = val;
    idx_ += 4;

    return true;
  }

  bool read8(uint64_t *ret) const {
    if ((idx_ + 8) > length_) {
      return false;
    }

    uint64_t val;
    memcpy(&val, &binary_[idx_], sizeof(val));

    if (swap_endian_) {
      swap8(&val);
    }

    (*ret) = val;
    idx_ += 8;

    return true;
  }

  bool read8(int64_t *ret) const {
    if ((idx_ + 8) > length_) {
      return false;
    }

    int64_t val;
    memcpy(&val, &binary_[idx_], sizeof(val));

    if (swap_endian_) {
      swap8(&val);
    }

    (*ret) = val;
    idx_ += 8;

    return true;
  }

  bool read_float(float *ret) const {
    if (!ret) {
      return false;
    }

    float value{};
    if (!read4(reinterpret_cast<int *>(&value))) {
      return false;
    }

    (*ret) = value;

    return true;
  }

  bool read_double(double *ret) const {
    if (!ret) {
      return false;
    }

    double value{};
    if (!read8(reinterpret_cast<uint64_t *>(&value))) {
      return false;
    }

    (*ret) = value;

    return true;
  }

#if 0
  bool read_value(Value *inout) {
    if (!inout) {
      return false;
    }

    if (inout->Type() == VALUE_TYPE_FLOAT) {
      float value;
      if (!read_float(&value)) {
        return false;
      }

      (*inout) = Value(value);
    } else if (inout->Type() == VALUE_TYPE_INT) {
      int value;
      if (!read4(&value)) {
        return false;
      }

      (*inout) = Value(value);
    } else {
      TINYVDBIO_ASSERT(0);
      return false;
    }

    return true;
  }
#endif

  uint64_t tell() const { return uint64_t(idx_); }
  bool eof() const { return idx_ >= length_; }

  bool is_nullchar() const {
    if (idx_ < length_) {
      return binary_[idx_] == '\0';
    }

    // TODO: report true when eof()?
    return false;
  }

  const uint8_t *data() const { return binary_; }

  bool swap_endian() const { return swap_endian_; }

  uint64_t size() const { return length_; }

 private:
  const uint8_t *binary_;
  const uint64_t length_;
  bool swap_endian_;
  char pad_[7];
  mutable uint64_t idx_;
};

namespace detail {

struct Cursor {
  int row{0};
  int col{0};
};

class Parser {
 public:
  Parser(StreamReader &sr) : _sr(sr) {}

  bool Char1(char *c) {
    return _sr.read1(c);
  }

  bool SkipUntilNewline();
  bool Eof() const { return _sr.eof(); }

 private:
  StreamReader &_sr;
  Cursor _cursor;
};

bool Parser::SkipUntilNewline() {
  while (!Eof()) {
    char c;
    if (!Char1(&c)) {
      // this should not happen.
      return false;
    }

    if (c == '\n') {
      break;
    } else if (c == '\r') {
      // CRLF?
      if (_sr.tell() < (_sr.size() - 1)) {
        char d;
        if (!Char1(&d)) {
          // this should not happen.
          return false;
        }

        if (d == '\n') {
          break;
        }

        // unwind 1 char
        if (!_sr.seek_from_current(-1)) {
          // this should not happen.
          return false;
        }

        break;
      }

    } else {
      // continue
    }
  }

  _cursor.row++;
  _cursor.col = 0;
  return true;
}

bool parse_header(StreamReader &sr, int &minor_version, std::string &err) {
  char buf[8];
  if (!sr.read(8, 8, reinterpret_cast<uint8_t *>(buf))) {
    return false;
  }

  if ((buf[0] == '%') && (buf[1] == 'P') && (buf[2] == 'D') &&
      (buf[3] == 'F') && (buf[4] == '-') && (buf[5] == '1') &&
      (buf[6] == '.')) {
    // ok
  } else {
    err += "Invalid header.\n";
    return false;
  }

  if ((buf[7] >= '0') && (buf[7] < '8')) {
    minor_version = buf[7] - '0';
  } else {
    err += "Invalid or unsupported PDF minor version.\n";
    return false;
  }

  return true;
}

}  // namespace detail

bool parse_from_memory(const uint8_t *addr, const size_t size) {
  if (size < 8) {
    // too short
    return false;
  }

  bool swap_endian = false;  // TODO
  StreamReader sr(addr, size, swap_endian);

  int minor_version{0};
  std::string err;
  if (!detail::parse_header(sr, minor_version, err)) {
    std::cerr << err << "\n";
    return false;
  }

  DCOUT("minor " << minor_version);
  std::cout << "minor\n";

  return true;
}

}  // namespace nanopdf
