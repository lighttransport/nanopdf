# nanopdf Test Infrastructure

This directory contains the reorganized test suite using the **doctest** framework, organized by feature area rather than development phase.

## Quick Start

### Build and Run All Tests
```bash
cd build
cmake .. -DNANOPDF_BUILD_TESTS=ON
make -j8
ctest --output-on-failure
```

### Run Only New Unit Tests
```bash
ctest -L unit --output-on-failure
```

### Run Only Legacy Tests
```bash
ctest -L legacy --output-on-failure
```

### Run Specific Test
```bash
ctest -R test_flate_decode -V
# or run directly:
./tests/unit/filters/test_flate_decode
```

### Custom Test Targets
```bash
make test_unit           # Run all unit tests
make test_integration    # Run all integration tests
make test_all           # Run all tests (same as ctest)
```

## Directory Structure

```
tests/
├── unit/                     # Unit tests (component isolation)
│   ├── core/                 # Core PDF parsing (Value, Parser, XRef)
│   ├── filters/              # Stream decoders (FlateDecode, etc.)
│   ├── fonts/                # Font system (Type1, TrueType, CMap, etc.)
│   ├── graphics/             # Color spaces, images, patterns
│   ├── text/                 # Text extraction and layout
│   ├── annotations/          # Annotation types
│   ├── forms/                # Interactive form fields
│   ├── security/             # Encryption and cryptography
│   ├── document/             # Document structure and metadata
│   └── backends/             # Rendering backends (ThorVG, Canvas)
├── integration/              # End-to-end tests with real PDFs
├── fixtures/                 # Shared test utilities
│   ├── test_helpers.hh       # Common helper functions
│   ├── test_helpers.cc       # Helper implementations
│   └── visual/               # Committed visual regression PDF fixtures
└── data/                     # Optional downloaded corpora and local test data
```

## Test Framework: nanotest

We use **nanotest** - a custom, lightweight, header-only C++ testing framework built specifically for nanopdf.

### Why nanotest?
- ✅ **Header-only** - No compilation overhead, just include nanotest.hh
- ✅ **No external dependencies** - Fully owned by the project
- ✅ **Fast** - Minimal overhead, lightning-fast compile times
- ✅ **C++17 compatible** - Matches nanopdf's requirement
- ✅ **Simple assertions** - `CHECK`, `REQUIRE`, `CHECK_EQ`, etc.
- ✅ **CTest integration** - Returns proper exit codes for test reporting
- ✅ **Minimal** - ~300 LOC vs doctest's 6,000 LOC or Google Test's 30,000+ LOC
- ✅ **Project-controlled** - Easy to extend or modify as needed

### Basic Test Structure

```cpp
#include "nanotest.hh"
#include "nanopdf.hh"
#include "test_helpers.hh"

using namespace nanopdf;
using namespace nanopdf::test;

TEST_SUITE("ComponentName") {
    TEST_CASE("Description of what is being tested") {
        // Arrange
        uint8_t input[] = {0x01, 0x02, 0x03};

        // Act
        DecodedStream result = some_function(input, sizeof(input));

        // Assert
        REQUIRE(result.success);  // Fatal - stops on failure
        CHECK(result.data.size() == 3);  // Non-fatal - continues
        CHECK(result.data[0] == 0x01);
    }

    TEST_CASE("Error handling") {
        DecodedStream result = some_function(nullptr, 0);

        CHECK_FALSE(result.success);
        CHECK_FALSE(result.error.empty());
    }
}

int main() {
    return nanotest::run_all_tests();
}
```

### Assertion Macros

- `REQUIRE(expr)` - Fatal assertion (stops test on failure)
- `CHECK(expr)` - Non-fatal assertion (continues on failure)
- `CHECK_EQ(a, b)` - Check equality with detailed error messages
- `CHECK_NE(a, b)` - Check inequality
- `CHECK_FALSE(expr)` - Check that expression is false
- `REQUIRE_FALSE(expr)` - Fatal false check
- `REQUIRE_EQ(a, b)` - Fatal equality check

### Running Tests

```bash
# Run all tests in a test executable
./test_flate_decode

# Tests run automatically via main() - no command-line options needed
# All test cases in the file are executed sequentially
```

## Test Helpers

The `tests/fixtures/test_helpers.hh` provides common utilities:

### Path Utilities
```cpp
std::string path = get_test_pdf_path("sample.pdf", "minimal");
std::string font_path = get_test_font_path("test.ttf");
```

### File I/O
```cpp
std::vector<uint8_t> data;
bool success = read_file(path, data);
```

### Binary Comparison
```cpp
bool equal = compare_bytes(vec1, vec2);
```

### PDF Building
```cpp
std::vector<uint8_t> pdf = create_minimal_pdf();
std::vector<uint8_t> text_pdf = create_text_pdf("Hello World");
```

### Test Data Generation
```cpp
std::vector<uint8_t> data = create_test_data(1024, true);  // compressible
std::vector<uint8_t> pattern = create_pattern_data(bytes, size, count);
```

## Migration Status

### ✅ Phase 1: Infrastructure (Complete)
- [x] doctest framework installed
- [x] Directory structure created
- [x] CMake configuration
- [x] Test fixtures library
- [x] Pilot test (test_flate_decode.cc)

### 🔄 Phase 2-7: Test Migration (In Progress)

Current test count:
- **Legacy tests**: 20 tests (686+ assertions)
- **New unit tests**: 1 test (33 assertions)
- **Total**: 21 tests

Target test count:
- **Unit tests**: ~60 test files (800+ test cases)
- **Integration tests**: ~10 test files (100+ test cases)

### Next Steps

1. **Migrate remaining filter tests** (Week 2)
   - ASCII85Decode, ASCIIHexDecode, LZWDecode
   - RunLengthDecode, DCTDecode
   - CCITTFaxDecode, JBIG2Decode
   - Filter chains

2. **Migrate security tests** (Week 3)
   - RC4, AES-128, AES-256
   - MD5, SHA-256
   - PDF security handlers

3. **Migrate font/text tests** (Week 4)
   - Font types (Type1, TrueType, Type0, Type3)
   - CMap and encoding
   - Text extraction and layout

4. **Create new tests** (Ongoing)
   - Core PDF parsing (Value, Parser, XRef)
   - Graphics (color spaces, patterns)
   - Document structure

## CMake Configuration

### Options

```cmake
-DNANOPDF_BUILD_TESTS=ON           # Enable all tests
-DNANOPDF_BUILD_LEGACY_TESTS=OFF   # Disable legacy phase tests
```

### Adding New Tests

1. Create test file: `tests/unit/category/test_feature.cc`

2. Add to `tests/unit/category/CMakeLists.txt`:
```cmake
add_nanopdf_unit_test(test_feature test_feature.cc)
```

3. Build and run:
```bash
make test_feature
ctest -R test_feature -V
```

## Best Practices

### ✅ DO:
- Use `REQUIRE` for preconditions that must pass
- Use `CHECK` for multiple assertions you want to all verify
- Group related tests in `TEST_SUITE`
- Write descriptive test case names
- Test both success and error paths
- Add edge cases (empty input, null pointers, boundary values)
- Use test helpers for common operations

### ❌ DON'T:
- Use manual `cout` statements (doctest shows values)
- Write tests with side effects between test cases
- Skip error testing
- Create tests that depend on external network/files
- Mix multiple unrelated assertions in one test case

## Performance

Current test execution time:
- **All tests**: ~0.15 seconds (21 tests)
- **Unit tests**: ~0.01 seconds (1 test)
- **Legacy tests**: ~0.13 seconds (20 tests)

Target after migration:
- **All tests**: <5 seconds (800+ test cases)
- **Unit tests**: <2 seconds
- **Integration tests**: <3 seconds

## Continuous Integration

Tests are designed to work with CI/CD:

```yaml
# Example GitHub Actions workflow
- name: Build and Test
  run: |
    cmake -B build -DNANOPDF_BUILD_TESTS=ON
    cmake --build build -j
    cd build && ctest --output-on-failure
```

## Contributing

When adding new features:

1. Write tests in the appropriate `tests/unit/` subdirectory
2. Follow the doctest pattern (see examples)
3. Use test helpers for common operations
4. Ensure tests pass locally before committing
5. Add integration tests for user-facing features

## Resources

- nanotest source: `tests/nanotest.hh` (custom implementation)
- [CMake CTest](https://cmake.org/cmake/help/latest/manual/ctest.1.html)

## nanotest Implementation

The nanotest framework is implemented in a single header file (`tests/nanotest.hh`):
- **~300 lines of code** - Easy to understand and modify
- **Automatic test registration** - Tests are registered at static initialization time
- **Simple macro-based API** - TEST_CASE, CHECK, REQUIRE macros
- **Clear output** - Shows test names, file locations, and failure details
- **Exit code support** - Returns 0 on success, 1 on failure (CTest compatible)

If you need to extend nanotest with new features (e.g., test fixtures, parameterized tests), simply edit `tests/nanotest.hh`.
