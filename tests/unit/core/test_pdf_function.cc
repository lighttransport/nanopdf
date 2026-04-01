#include "nanotest.hh"

#include <cmath>
#include <initializer_list>
#include <vector>

#include "nanopdf.hh"
#include "pdf-function.hh"

namespace {

nanopdf::Value make_number(double value) {
  nanopdf::Value number(nanopdf::Value::NUMBER);
  number.number = value;
  return number;
}

nanopdf::Value make_array(std::initializer_list<double> values) {
  nanopdf::Value array(nanopdf::Value::ARRAY);
  for (double value : values) {
    array.array.push_back(make_number(value));
  }
  return array;
}

nanopdf::Value make_sampled_function_stream(
    const std::vector<uint8_t>& data,
    std::initializer_list<double> domain,
    std::initializer_list<double> range) {
  nanopdf::Value function(nanopdf::Value::STREAM);
  function.stream.data = data;
  function.stream.dict["FunctionType"] = make_number(0.0);
  function.stream.dict["BitsPerSample"] = make_number(8.0);
  function.stream.dict["Size"] = make_array({1.0});
  function.stream.dict["Domain"] = make_array(domain);
  function.stream.dict["Range"] = make_array(range);
  return function;
}

nanopdf::Value make_type2_function(double exponent,
                                   std::initializer_list<double> c0,
                                   std::initializer_list<double> c1) {
  nanopdf::Value function(nanopdf::Value::DICTIONARY);
  function.dict["FunctionType"] = make_number(2.0);
  function.dict["N"] = make_number(exponent);
  function.dict["C0"] = make_array(c0);
  function.dict["C1"] = make_array(c1);
  return function;
}

}  // namespace

TEST_SUITE("PdfFunction") {

TEST_CASE("sampled function with zero-width domain falls back safely") {
  nanopdf::Pdf pdf;
  auto function = make_sampled_function_stream({128}, {0.0, 0.0}, {0.0, 1.0});

  std::vector<double> outputs;
  REQUIRE(nanopdf::pdfunc::evaluate(pdf, function, {0.0}, outputs));
  REQUIRE_EQ(outputs.size(), size_t(1));
  CHECK_EQ(outputs[0], 0.5);
}

TEST_CASE("type 2 function suppresses invalid pow results") {
  nanopdf::Pdf pdf;
  auto function = make_type2_function(-1.0, {2.0}, {4.0});
  function.dict["Domain"] = make_array({0.0, 1.0});
  function.dict["Range"] = make_array({0.0, 4.0});

  std::vector<double> outputs;
  REQUIRE(nanopdf::pdfunc::evaluate(pdf, function, {0.0}, outputs));
  REQUIRE_EQ(outputs.size(), size_t(1));
  CHECK(std::isfinite(outputs[0]));
  CHECK_EQ(outputs[0], 2.0);
}

TEST_CASE("postscript bitshift clamps oversized shifts") {
  std::vector<double> outputs;
  REQUIRE(nanopdf::pdfunc::evaluate_postscript_function(
      "{ 1 1000 bitshift }", {}, {}, {}, outputs));
  REQUIRE_EQ(outputs.size(), size_t(1));
  CHECK_EQ(outputs[0], 0.0);
}

}  // TEST_SUITE
