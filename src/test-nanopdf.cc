#include "nanopdf.hh"

#include <cstdio>
#include <vector>
#include <string>
#include <fstream>
#include <iostream>
#include <limits>

static bool ReadWholeFile(std::vector<uint8_t> *out, std::string *err,
                   const std::string &filepath, size_t filesize_max,
                   void *userdata) {
  (void)userdata;

#ifdef _WIN32
#if defined(__GLIBCXX__)  // mingw
  int file_descriptor =
      _wopen(UTF8ToWchar(filepath).c_str(), _O_RDONLY | _O_BINARY);
  __gnu_cxx::stdio_filebuf<char> wfile_buf(file_descriptor, std::ios_base::in);
  std::istream f(&wfile_buf);
#elif defined(_MSC_VER) || defined(_LIBCPP_VERSION)
  // For libcxx, assume _LIBCPP_HAS_OPEN_WITH_WCHAR is defined to accept
  // `wchar_t *`
  std::ifstream f(UTF8ToWchar(filepath).c_str(), std::ifstream::binary);
#else
  // Unknown compiler/runtime
  std::ifstream f(filepath.c_str(), std::ifstream::binary);
#endif
#else
  std::ifstream f(filepath.c_str(), std::ifstream::binary);
#endif
  if (!f) {
    if (err) {
      (*err) += "File open error : " + filepath + "\n";
    }
    return false;
  }

  // For directory(and pipe?), peek() will fail(Posix gnustl/libc++ only)
  int buf = f.peek();
  (void)buf;
  if (!f) {
    if (err) {
      (*err) += "File read error. Maybe empty file or invalid file : " + filepath + "\n";
    }
    return false;
  }

  f.seekg(0, f.end);
  size_t sz = static_cast<size_t>(f.tellg());
  f.seekg(0, f.beg);

  if (int64_t(sz) < 0) {
    if (err) {
      (*err) += "Invalid file size : " + filepath +
                " (does the path point to a directory?)";
    }
    return false;
  } else if (sz == 0) {
    if (err) {
      (*err) += "File is empty : " + filepath + "\n";
    }
    return false;
  } else if (uint64_t(sz) >= uint64_t(std::numeric_limits<int64_t>::max())) {
    // Posixish environment.
    if (err) {
      (*err) += "Invalid File(Pipe or special device?) : " + filepath + "\n";
    }
    return false;
  }

  if ((filesize_max > 0) && (sz > filesize_max)) {
    if (err) {
      (*err) += "File size is too large : " + filepath +
                " sz = " + std::to_string(sz) +
                ", allowed max filesize = " + std::to_string(filesize_max) +
                "\n";
    }
    return false;
  }

  out->resize(sz);
  f.read(reinterpret_cast<char *>(&out->at(0)),
         static_cast<std::streamsize>(sz));

  if (!f) {
    // read failure.
    if (err) {
      (*err) += "Failed to read file: " + filepath + "\n";
    }
    return false;
  }

  return true;
}

int main(int argc, char **argv) {

  if (argc < 2) {
    printf("Needs input.pdf\n");
    return -1;
  }

  std::vector<uint8_t> buf;
  std::string err;
  std::string filepath = argv[1];
  size_t size_max = 1024*1024*1024; // Up to 1GB

  if (!ReadWholeFile(&buf, &err, filepath, size_max, nullptr)) {
    std::cerr << err << "\n";
    return -2;
  }

  if (nanopdf::parse_from_memory(buf.data(), buf.size())) {
    return -3;
  }

  return 0;
}
