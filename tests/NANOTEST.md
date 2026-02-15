# nanotest - Lightweight Unit Testing for nanopdf

**nanotest** is a custom, minimal C++ unit testing framework built specifically for the nanopdf project.

## Design Philosophy

- **Zero external dependencies** - Fully owned by the project
- **Minimal implementation** - ~300 lines of code
- **Simple API** - Easy to learn and use
- **Fast compilation** - Header-only with minimal overhead
- **CTest compatible** - Returns proper exit codes (0 = success, 1 = failure)

## Features

### Test Organization

```cpp
TEST_SUITE("ComponentName") {
    TEST_CASE("Description of test") {
        // Test code here
    }
}
```

### Assertions

| Macro | Description | Behavior on Failure |
|-------|-------------|---------------------|
| `CHECK(expr)` | Non-fatal assertion | Continues test execution |
| `REQUIRE(expr)` | Fatal assertion | Stops current test |
| `CHECK_FALSE(expr)` | Check false | Continues test execution |
| `REQUIRE_FALSE(expr)` | Fatal false check | Stops current test |
| `CHECK_EQ(a, b)` | Check equality with detailed output | Continues test execution |
| `REQUIRE_EQ(a, b)` | Fatal equality check | Stops current test |
| `CHECK_NE(a, b)` | Check inequality | Continues test execution |

### Example Output

```
[nanotest] Running tests...
===============================================================================

TEST SUITE: FlateDecode
TEST CASE:  Decompress 'Hello, World!'

TEST SUITE: FlateDecode
TEST CASE:  Decompress repeated data

===============================================================================
[nanotest] test cases: 9 | 9 passed | 0 failed
[nanotest] assertions: 33 | 33 passed | 0 failed
[nanotest] Status: SUCCESS!
```

### Error Reporting

When an assertion fails, nanotest provides clear error messages:

```
/path/to/test.cc:42: ERROR: CHECK( result.success ) failed

/path/to/test.cc:45: FATAL ERROR: REQUIRE( data.size() == expected ) failed
  values: data.size() = 10, expected = 20
```

## Usage

### Basic Test File

```cpp
#include "nanotest.hh"
#include "nanopdf.hh"

TEST_SUITE("MyComponent") {
    TEST_CASE("Basic functionality") {
        int result = 2 + 2;
        CHECK(result == 4);
        REQUIRE(result > 0);
    }

    TEST_CASE("Error handling") {
        bool error = process_invalid_input();
        CHECK_FALSE(error);
    }
}

int main() {
    return nanotest::run_all_tests();
}
```

### Comparison with Other Frameworks

| Feature | nanotest | doctest | Google Test |
|---------|----------|---------|-------------|
| **Lines of Code** | ~300 | ~6,000 | ~30,000 |
| **External Dependency** | No | Yes | Yes |
| **Header-only** | Yes | Yes | No |
| **Compile Time** | Fastest | Fast | Slower |
| **Setup Required** | Include header | Download header | Link library |
| **Customizable** | Easy (edit one file) | No | Difficult |
| **CTest Integration** | Yes | Yes | Yes |
| **Test Discovery** | Automatic | Automatic | Automatic |

## Implementation Details

### Automatic Test Registration

Tests are automatically registered using static initialization:

```cpp
struct TestRegistrar {
    TestRegistrar(const char* suite, const char* name,
                 TestFunction func, const char* file, int line) {
        register_test(suite, name, func, file, line);
    }
};
```

Each `TEST_CASE` macro creates a unique test function and a static `TestRegistrar` instance that registers the test at program startup.

### Test Execution

The `run_all_tests()` function:
1. Iterates through all registered tests
2. Executes each test function
3. Catches exceptions
4. Tracks statistics (test cases passed/failed, assertions passed/failed)
5. Prints summary
6. Returns exit code (0 for success, 1 for failure)

### Assertion Macros

- **CHECK/REQUIRE**: Evaluates expression, reports if false
- **CHECK_EQ/REQUIRE_EQ**: Compares two values, shows both values on failure
- **Fatal vs Non-fatal**: REQUIRE returns from test function on failure, CHECK continues

## Extending nanotest

Since nanotest is a single header file owned by the project, it's easy to extend:

### Adding New Assertion Types

```cpp
// In nanotest.hh
#define CHECK_GT(lhs, rhs) \
    do { \
        auto lhs_val_ = (lhs); \
        auto rhs_val_ = (rhs); \
        bool gt_result_ = (lhs_val_ > rhs_val_); \
        nanotest::report_comparison(gt_result_, lhs_val_, rhs_val_, \
                                    #lhs, #rhs, ">", __FILE__, __LINE__, false); \
    } while (false)
```

### Adding Test Fixtures

```cpp
// In nanotest.hh - add fixture support
#define TEST_CASE_WITH_FIXTURE(name, fixture) \
    // Implementation here
```

### Adding Parameterized Tests

```cpp
// In nanotest.hh - add parameterized test support
#define TEST_CASE_P(name, params) \
    // Implementation here
```

## Why Not Use an Existing Framework?

### Advantages of Custom Implementation

1. **No external dependencies** - Reduces project complexity
2. **Full control** - Can customize to exact needs
3. **Minimal code** - Easier to understand and maintain
4. **Project-specific** - Can add nanopdf-specific features
5. **Learning** - Team understands the implementation completely
6. **Stability** - Not affected by external framework changes

### When to Consider Switching

Consider switching to an established framework if:
- Need advanced features (mocking, BDD syntax, data-driven tests)
- Team is already familiar with another framework
- Need IDE integration features
- Project grows to >100 test files

For nanopdf's current needs (~60 planned test files), nanotest is ideal.

## Performance

Current performance on nanopdf test suite:
- **Compilation**: ~0.1s per test file (minimal overhead)
- **Execution**: ~0.01s for 9 test cases with 33 assertions
- **Total test suite**: ~0.15s for 21 tests (1 nanotest + 20 legacy)

## File Location

- Implementation: `/home/syoyo/work/nanopdf/main/tests/nanotest.hh`
- Example usage: `/home/syoyo/work/nanopdf/main/tests/unit/filters/test_flate_decode.cc`

## Contributing

To improve nanotest:
1. Edit `tests/nanotest.hh`
2. Add new features or fix bugs
3. Test with existing test files
4. Update this documentation

Keep the implementation simple and focused on nanopdf's testing needs.
