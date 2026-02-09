#include "nanopdf.hh"
#include <cassert>
#include <cstring>
#include <iostream>
#include <vector>

using namespace nanopdf;

// Test RunLengthDecode filter
void test_runlength_decode() {
  std::cout << "Testing RunLengthDecode filter..." << std::endl;

  // Test case 1: Simple literal copy
  {
    uint8_t encoded[] = {0x02, 'A', 'B', 'C', 128};  // Copy 3 bytes literally, then EOD
    filters::DecodeParams params;
    DecodedStream result = filters::decode_runlength(encoded, sizeof(encoded), params);

    assert(result.success);
    assert(result.data.size() == 3);
    assert(result.data[0] == 'A');
    assert(result.data[1] == 'B');
    assert(result.data[2] == 'C');
    std::cout << "  Test 1 (literal copy): PASSED" << std::endl;
  }

  // Test case 2: Run-length encoding
  {
    uint8_t encoded[] = {254, 'X', 128};  // Repeat 'X' 3 times (257-254), then EOD
    filters::DecodeParams params;
    DecodedStream result = filters::decode_runlength(encoded, sizeof(encoded), params);

    assert(result.success);
    assert(result.data.size() == 3);
    assert(result.data[0] == 'X');
    assert(result.data[1] == 'X');
    assert(result.data[2] == 'X');
    std::cout << "  Test 2 (run-length): PASSED" << std::endl;
  }

  // Test case 3: Mixed literal and run-length
  {
    uint8_t encoded[] = {0x01, 'A', 'B', 253, 'Z', 128};  // Copy 2 bytes, repeat 'Z' 4 times
    filters::DecodeParams params;
    DecodedStream result = filters::decode_runlength(encoded, sizeof(encoded), params);

    assert(result.success);
    assert(result.data.size() == 6);
    assert(result.data[0] == 'A');
    assert(result.data[1] == 'B');
    assert(result.data[2] == 'Z');
    assert(result.data[3] == 'Z');
    assert(result.data[4] == 'Z');
    assert(result.data[5] == 'Z');
    std::cout << "  Test 3 (mixed): PASSED" << std::endl;
  }

  std::cout << "RunLengthDecode tests completed successfully!" << std::endl << std::endl;
}

// Test ASCIIHexDecode filter
void test_asciihex_decode() {
  std::cout << "Testing ASCIIHexDecode filter..." << std::endl;

  // Test case 1: Simple hex pairs
  {
    const char *encoded = "48656C6C6F>";
    filters::DecodeParams params;
    DecodedStream result = filters::decode_asciihex(
        reinterpret_cast<const uint8_t *>(encoded), strlen(encoded), params);

    assert(result.success);
    assert(result.data.size() == 5);
    assert(result.data[0] == 'H');
    assert(result.data[1] == 'e');
    assert(result.data[2] == 'l');
    assert(result.data[3] == 'l');
    assert(result.data[4] == 'o');
    std::cout << "  Test 1 (simple hex pairs): PASSED" << std::endl;
  }

  // Test case 2: Lowercase hex digits
  {
    const char *encoded = "4d616e>";
    filters::DecodeParams params;
    DecodedStream result = filters::decode_asciihex(
        reinterpret_cast<const uint8_t *>(encoded), strlen(encoded), params);

    assert(result.success);
    assert(result.data.size() == 3);
    assert(result.data[0] == 'M');
    assert(result.data[1] == 'a');
    assert(result.data[2] == 'n');
    std::cout << "  Test 2 (lowercase hex): PASSED" << std::endl;
  }

  // Test case 3: Mixed case
  {
    const char *encoded = "4d61 6E>";
    filters::DecodeParams params;
    DecodedStream result = filters::decode_asciihex(
        reinterpret_cast<const uint8_t *>(encoded), strlen(encoded), params);

    assert(result.success);
    assert(result.data.size() == 3);
    assert(result.data[0] == 'M');
    assert(result.data[1] == 'a');
    assert(result.data[2] == 'n');
    std::cout << "  Test 3 (mixed case): PASSED" << std::endl;
  }

  // Test case 4: Whitespace is ignored (spaces, tabs, newlines)
  {
    const char *encoded = "48 65\t6C\r\n6C 6F>";
    filters::DecodeParams params;
    DecodedStream result = filters::decode_asciihex(
        reinterpret_cast<const uint8_t *>(encoded), strlen(encoded), params);

    assert(result.success);
    assert(result.data.size() == 5);
    assert(result.data[0] == 'H');
    assert(result.data[1] == 'e');
    assert(result.data[2] == 'l');
    assert(result.data[3] == 'l');
    assert(result.data[4] == 'o');
    std::cout << "  Test 4 (whitespace ignored): PASSED" << std::endl;
  }

  // Test case 5: Odd number of hex digits — trailing nibble padded with 0
  {
    const char *encoded = "ABC>";  // AB C0
    filters::DecodeParams params;
    DecodedStream result = filters::decode_asciihex(
        reinterpret_cast<const uint8_t *>(encoded), strlen(encoded), params);

    assert(result.success);
    assert(result.data.size() == 2);
    assert(result.data[0] == 0xAB);
    assert(result.data[1] == 0xC0);
    std::cout << "  Test 5 (odd nibble padded): PASSED" << std::endl;
  }

  // Test case 6: Empty input (just end marker)
  {
    const char *encoded = ">";
    filters::DecodeParams params;
    DecodedStream result = filters::decode_asciihex(
        reinterpret_cast<const uint8_t *>(encoded), strlen(encoded), params);

    assert(result.success);
    assert(result.data.size() == 0);
    std::cout << "  Test 6 (empty input): PASSED" << std::endl;
  }

  // Test case 7: Invalid hex character
  {
    const char *encoded = "48GG>";
    filters::DecodeParams params;
    DecodedStream result = filters::decode_asciihex(
        reinterpret_cast<const uint8_t *>(encoded), strlen(encoded), params);

    assert(!result.success);
    std::cout << "  Test 7 (invalid character): PASSED" << std::endl;
  }

  // Test case 8: All zero bytes
  {
    const char *encoded = "00000000>";
    filters::DecodeParams params;
    DecodedStream result = filters::decode_asciihex(
        reinterpret_cast<const uint8_t *>(encoded), strlen(encoded), params);

    assert(result.success);
    assert(result.data.size() == 4);
    assert(result.data[0] == 0);
    assert(result.data[1] == 0);
    assert(result.data[2] == 0);
    assert(result.data[3] == 0);
    std::cout << "  Test 8 (all zeros): PASSED" << std::endl;
  }

  // Test case 9: All FF bytes
  {
    const char *encoded = "FFFF>";
    filters::DecodeParams params;
    DecodedStream result = filters::decode_asciihex(
        reinterpret_cast<const uint8_t *>(encoded), strlen(encoded), params);

    assert(result.success);
    assert(result.data.size() == 2);
    assert(result.data[0] == 0xFF);
    assert(result.data[1] == 0xFF);
    std::cout << "  Test 9 (FF bytes): PASSED" << std::endl;
  }

  std::cout << "ASCIIHexDecode tests completed successfully!" << std::endl << std::endl;
}

// Test ASCII85Decode filter
void test_ascii85_decode() {
  std::cout << "Testing ASCII85Decode filter..." << std::endl;

  // Test case 1: Standard encoding of "Man " (RFC example)
  // "Man " = 0x4D616E20 => base-85 digits: 9*85^3 + 60*85^2 + 51*85 + 8 + 33 offsets = "9jqo^"
  {
    const char *encoded = "9jqo^~>";
    filters::DecodeParams params;
    DecodedStream result = filters::decode_ascii85(
        reinterpret_cast<const uint8_t *>(encoded), strlen(encoded), params);

    assert(result.success);
    assert(result.data.size() == 4);
    assert(result.data[0] == 'M');
    assert(result.data[1] == 'a');
    assert(result.data[2] == 'n');
    assert(result.data[3] == ' ');
    std::cout << "  Test 1 (standard group): PASSED" << std::endl;
  }

  // Test case 2: 'z' shorthand for four zero bytes
  {
    const char *encoded = "z~>";
    filters::DecodeParams params;
    DecodedStream result = filters::decode_ascii85(
        reinterpret_cast<const uint8_t *>(encoded), strlen(encoded), params);

    assert(result.success);
    assert(result.data.size() == 4);
    assert(result.data[0] == 0);
    assert(result.data[1] == 0);
    assert(result.data[2] == 0);
    assert(result.data[3] == 0);
    std::cout << "  Test 2 (z shorthand): PASSED" << std::endl;
  }

  // Test case 3: Partial group (fewer than 5 encoded chars)
  // Encoding of "Ma" (2 bytes) → 3 encoded chars "9jq"
  {
    const char *encoded = "9jq~>";
    filters::DecodeParams params;
    DecodedStream result = filters::decode_ascii85(
        reinterpret_cast<const uint8_t *>(encoded), strlen(encoded), params);

    assert(result.success);
    assert(result.data.size() == 2);
    assert(result.data[0] == 'M');
    assert(result.data[1] == 'a');
    std::cout << "  Test 3 (partial group): PASSED" << std::endl;
  }

  // Test case 4: Whitespace is ignored
  {
    const char *encoded = "9jqo ^\r\n~>";
    filters::DecodeParams params;
    DecodedStream result = filters::decode_ascii85(
        reinterpret_cast<const uint8_t *>(encoded), strlen(encoded), params);

    assert(result.success);
    assert(result.data.size() == 4);
    assert(result.data[0] == 'M');
    assert(result.data[1] == 'a');
    assert(result.data[2] == 'n');
    assert(result.data[3] == ' ');
    std::cout << "  Test 4 (whitespace ignored): PASSED" << std::endl;
  }

  // Test case 5: Multiple groups + z
  {
    // "Man " + 4 zeros = "9jqo^" + "z"
    const char *encoded = "9jqo^z~>";
    filters::DecodeParams params;
    DecodedStream result = filters::decode_ascii85(
        reinterpret_cast<const uint8_t *>(encoded), strlen(encoded), params);

    assert(result.success);
    assert(result.data.size() == 8);
    assert(result.data[0] == 'M');
    assert(result.data[4] == 0);
    assert(result.data[5] == 0);
    assert(result.data[6] == 0);
    assert(result.data[7] == 0);
    std::cout << "  Test 5 (multiple groups + z): PASSED" << std::endl;
  }

  // Test case 6: Empty input (just end marker)
  {
    const char *encoded = "~>";
    filters::DecodeParams params;
    DecodedStream result = filters::decode_ascii85(
        reinterpret_cast<const uint8_t *>(encoded), strlen(encoded), params);

    assert(result.success);
    assert(result.data.size() == 0);
    std::cout << "  Test 6 (empty input): PASSED" << std::endl;
  }

  // Test case 7: Invalid character
  {
    const char *encoded = "9jqo{~>";  // '{' is out of '!'..'u' range
    filters::DecodeParams params;
    DecodedStream result = filters::decode_ascii85(
        reinterpret_cast<const uint8_t *>(encoded), strlen(encoded), params);

    assert(!result.success);
    std::cout << "  Test 7 (invalid character): PASSED" << std::endl;
  }

  // Test case 8: Longer known encoding
  // "Hello" = 0x48656C6C 6F => "87cUR" + partial "DZ"
  {
    const char *encoded = "87cURDZ~>";
    filters::DecodeParams params;
    DecodedStream result = filters::decode_ascii85(
        reinterpret_cast<const uint8_t *>(encoded), strlen(encoded), params);

    assert(result.success);
    assert(result.data.size() == 5);
    assert(result.data[0] == 'H');
    assert(result.data[1] == 'e');
    assert(result.data[2] == 'l');
    assert(result.data[3] == 'l');
    assert(result.data[4] == 'o');
    std::cout << "  Test 8 (Hello encoding): PASSED" << std::endl;
  }

  std::cout << "ASCII85Decode tests completed successfully!" << std::endl << std::endl;
}

// Test LZWDecode filter
void test_lzw_decode() {
  std::cout << "Testing LZWDecode filter..." << std::endl;

  // LZW encoding uses MSB-first bit packing, starting with 9-bit codes.
  // Clear code = 256, EOD code = 257.

  // Test case 1: Clear + single byte + EOD
  // Encode: CLEAR(256) + 'A'(65) + EOD(257)
  // 9-bit codes: 256=0x100, 65=0x041, 257=0x101
  // Bit stream (MSB first):
  //   100000000 000100001 100000001
  //   = 1000 0000 0000 1000 0110 0000 001(0 0000)
  //   = 0x80 0x08 0x60 0x20
  {
    uint8_t encoded[] = {0x80, 0x10, 0x60, 0x20};
    filters::DecodeParams params;
    DecodedStream result =
        filters::decode_lzw(encoded, sizeof(encoded), params);

    assert(result.success);
    assert(result.data.size() == 1);
    assert(result.data[0] == 'A');
    std::cout << "  Test 1 (single byte): PASSED" << std::endl;
  }

  // Test case 2: Clear + multiple bytes + EOD
  // Encode: CLEAR(256) + 'A'(65) + 'B'(66) + 'C'(67) + EOD(257)
  // 9-bit codes: 256, 65, 66, 67, 257
  // Binary (MSB-first, 9 bits each):
  //   100000000 001000001 001000010 001000011 100000001
  // Pack into bytes:
  //   10000000 00010000 01001000 01000100 00111000 00001(000)
  //   0x80     0x10     0x48     0x44     0x38     0x08
  {
    uint8_t encoded[] = {0x80, 0x10, 0x48, 0x44, 0x38, 0x08};
    filters::DecodeParams params;
    DecodedStream result =
        filters::decode_lzw(encoded, sizeof(encoded), params);

    assert(result.success);
    assert(result.data.size() == 3);
    assert(result.data[0] == 'A');
    assert(result.data[1] == 'B');
    assert(result.data[2] == 'C');
    std::cout << "  Test 2 (multiple bytes): PASSED" << std::endl;
  }

  // Test case 3: Clear + repeated byte (triggers dictionary entry) + EOD
  // Encode: CLEAR(256) + 'A'(65) + 'A'(65) + 258(="AA") + EOD(257)
  // After first 'A' and second 'A', dict entry 258="AA" is created.
  // Then code 258 outputs "AA".
  // 9-bit codes: 256, 65, 65, 258, 257
  // Binary:
  //   100000000 001000001 001000001 100000010 100000001
  // Pack:
  //   10000000 00010000 01001000 00110000 00101000 00001(000)
  //   0x80     0x10     0x48     0x30     0x28     0x08
  {
    uint8_t encoded[] = {0x80, 0x10, 0x48, 0x30, 0x28, 0x08};
    filters::DecodeParams params;
    DecodedStream result =
        filters::decode_lzw(encoded, sizeof(encoded), params);

    assert(result.success);
    assert(result.data.size() == 4);
    assert(result.data[0] == 'A');
    assert(result.data[1] == 'A');
    assert(result.data[2] == 'A');
    assert(result.data[3] == 'A');
    std::cout << "  Test 3 (repeated byte with dict): PASSED" << std::endl;
  }

  // Test case 4: Empty data should fail
  {
    filters::DecodeParams params;
    DecodedStream result = filters::decode_lzw(nullptr, 0, params);

    // Should either fail or produce empty output
    // Implementation returns error on get_code failure
    assert(!result.success || result.data.empty());
    std::cout << "  Test 4 (empty input): PASSED" << std::endl;
  }

  // Test case 5: Clear + EOD only (no data)
  // 9-bit codes: 256, 257
  // Binary: 100000000 100000001
  // Pack: 10000000 01000000 01(000000)
  //        0x80     0x40     0x40
  {
    uint8_t encoded[] = {0x80, 0x40, 0x40};
    filters::DecodeParams params;
    DecodedStream result =
        filters::decode_lzw(encoded, sizeof(encoded), params);

    assert(result.success);
    assert(result.data.size() == 0);
    std::cout << "  Test 5 (clear + EOD only): PASSED" << std::endl;
  }

  std::cout << "LZWDecode tests completed successfully!" << std::endl << std::endl;
}

// Test Color Space parsing
void test_color_space_parsing() {
  std::cout << "Testing ColorSpace parsing..." << std::endl;

  // Mock Pdf object for testing
  Pdf pdf;

  // Test case 1: Simple device color spaces
  {
    Value cs_value;
    cs_value.SetType(Value::NAME);
    cs_value.name = "DeviceRGB";

    ColorSpace cs = parse_color_space(pdf, cs_value);
    assert(cs.type == ColorSpaceType::DeviceRGB);
    assert(cs.name == "DeviceRGB");
    std::cout << "  Test 1 (DeviceRGB): PASSED" << std::endl;
  }

  // Test case 2: DeviceGray
  {
    Value cs_value;
    cs_value.SetType(Value::NAME);
    cs_value.name = "DeviceGray";

    ColorSpace cs = parse_color_space(pdf, cs_value);
    assert(cs.type == ColorSpaceType::DeviceGray);
    std::cout << "  Test 2 (DeviceGray): PASSED" << std::endl;
  }

  // Test case 3: DeviceCMYK
  {
    Value cs_value;
    cs_value.SetType(Value::NAME);
    cs_value.name = "DeviceCMYK";

    ColorSpace cs = parse_color_space(pdf, cs_value);
    assert(cs.type == ColorSpaceType::DeviceCMYK);
    std::cout << "  Test 3 (DeviceCMYK): PASSED" << std::endl;
  }

  // Test case 4: CalGray with parameters
  {
    Value cs_value;
    cs_value.SetType(Value::ARRAY);

    Value type_val;
    type_val.SetType(Value::NAME);
    type_val.name = "CalGray";
    cs_value.array.push_back(type_val);

    Value params_val;
    params_val.SetType(Value::DICTIONARY);

    // Add WhitePoint
    Value wp_val;
    wp_val.SetType(Value::ARRAY);
    Value wp_x, wp_y, wp_z;
    wp_x.SetType(Value::NUMBER);
    wp_y.SetType(Value::NUMBER);
    wp_z.SetType(Value::NUMBER);
    wp_x.number = 0.9505;
    wp_y.number = 1.0;
    wp_z.number = 1.0890;
    wp_val.array.push_back(wp_x);
    wp_val.array.push_back(wp_y);
    wp_val.array.push_back(wp_z);
    params_val.dict["WhitePoint"] = wp_val;

    // Add Gamma
    Value gamma_val;
    gamma_val.SetType(Value::NUMBER);
    gamma_val.number = 2.2;
    params_val.dict["Gamma"] = gamma_val;

    cs_value.array.push_back(params_val);

    ColorSpace cs = parse_color_space(pdf, cs_value);
    assert(cs.type == ColorSpaceType::CalGray);
    assert(cs.white_point.size() == 3);
    assert(cs.white_point[0] == 0.9505);
    assert(cs.gamma.size() == 1);
    assert(cs.gamma[0] == 2.2);
    std::cout << "  Test 4 (CalGray with params): PASSED" << std::endl;
  }

  std::cout << "ColorSpace parsing tests completed successfully!" << std::endl << std::endl;
}

// Test Image XObject parsing
void test_image_xobject_parsing() {
  std::cout << "Testing ImageXObject parsing..." << std::endl;

  // Mock Pdf object for testing
  Pdf pdf;

  // Test case 1: Basic image with width, height, and bits per component
  {
    Value stream_val;
    stream_val.SetType(Value::STREAM);

    // Set up dictionary
    Value width_val;
    width_val.SetType(Value::NUMBER);
    width_val.number = 100;
    stream_val.stream.dict["Width"] = width_val;

    Value height_val;
    height_val.SetType(Value::NUMBER);
    height_val.number = 50;
    stream_val.stream.dict["Height"] = height_val;

    Value bpc_val;
    bpc_val.SetType(Value::NUMBER);
    bpc_val.number = 8;
    stream_val.stream.dict["BitsPerComponent"] = bpc_val;

    // Add color space
    Value cs_val;
    cs_val.SetType(Value::NAME);
    cs_val.name = "DeviceRGB";
    stream_val.stream.dict["ColorSpace"] = cs_val;

    // Add some dummy data
    stream_val.stream.data = {0x01, 0x02, 0x03};

    ImageXObject image = parse_image_xobject(pdf, stream_val);
    assert(image.width == 100);
    assert(image.height == 50);
    assert(image.bits_per_component == 8);
    assert(image.color_space.type == ColorSpaceType::DeviceRGB);
    assert(!image.image_mask);
    std::cout << "  Test 1 (basic image): PASSED" << std::endl;
  }

  // Test case 2: Image mask
  {
    Value stream_val;
    stream_val.SetType(Value::STREAM);

    Value width_val;
    width_val.SetType(Value::NUMBER);
    width_val.number = 32;
    stream_val.stream.dict["Width"] = width_val;

    Value height_val;
    height_val.SetType(Value::NUMBER);
    height_val.number = 32;
    stream_val.stream.dict["Height"] = height_val;

    Value mask_val;
    mask_val.SetType(Value::BOOLEAN);
    mask_val.boolean = true;
    stream_val.stream.dict["ImageMask"] = mask_val;

    Value bpc_val;
    bpc_val.SetType(Value::NUMBER);
    bpc_val.number = 1;
    stream_val.stream.dict["BitsPerComponent"] = bpc_val;

    ImageXObject image = parse_image_xobject(pdf, stream_val);
    assert(image.width == 32);
    assert(image.height == 32);
    assert(image.bits_per_component == 1);
    assert(image.image_mask);
    std::cout << "  Test 2 (image mask): PASSED" << std::endl;
  }

  // Test case 3: Image with interpolation
  {
    Value stream_val;
    stream_val.SetType(Value::STREAM);

    Value width_val;
    width_val.SetType(Value::NUMBER);
    width_val.number = 64;
    stream_val.stream.dict["Width"] = width_val;

    Value height_val;
    height_val.SetType(Value::NUMBER);
    height_val.number = 64;
    stream_val.stream.dict["Height"] = height_val;

    Value interp_val;
    interp_val.SetType(Value::BOOLEAN);
    interp_val.boolean = true;
    stream_val.stream.dict["Interpolate"] = interp_val;

    ImageXObject image = parse_image_xobject(pdf, stream_val);
    assert(image.width == 64);
    assert(image.height == 64);
    assert(image.interpolate);
    std::cout << "  Test 3 (interpolation): PASSED" << std::endl;
  }

  std::cout << "ImageXObject parsing tests completed successfully!" << std::endl << std::endl;
}

int main() {
  std::cout << "=== Phase 1 Feature Tests ===" << std::endl << std::endl;

  test_runlength_decode();
  test_asciihex_decode();
  test_ascii85_decode();
  test_lzw_decode();
  test_color_space_parsing();
  test_image_xobject_parsing();

  std::cout << "=== All Phase 1 tests passed! ===" << std::endl;

  return 0;
}
