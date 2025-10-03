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
  test_color_space_parsing();
  test_image_xobject_parsing();

  std::cout << "=== All Phase 1 tests passed! ===" << std::endl;

  return 0;
}
