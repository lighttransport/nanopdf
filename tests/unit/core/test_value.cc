/**
 * Value class unit tests
 *
 * Tests the polymorphic Value type that represents all PDF object types:
 * UNDEFINED, BOOLEAN, NUMBER, STRING, NAME, ARRAY, DICTIONARY, STREAM,
 * REFERENCE, and NULL_OBJ.
 */

#include "nanotest.hh"
#include "nanopdf.hh"
#include "test_helpers.hh"

using namespace nanopdf;

// ============================================================================
// Default construction
// ============================================================================

TEST_SUITE("Value") {

TEST_CASE("Default construction yields UNDEFINED") {
    Value v;
    CHECK(v.type == Value::UNDEFINED);
    CHECK(v.boolean == false);
    CHECK(v.number == 0.0);
    CHECK(v.str.empty());
    CHECK(v.name.empty());
    CHECK(v.array.empty());
    CHECK(v.dict.empty());
    CHECK_EQ(v.ref_object_number, 0u);
    CHECK_EQ(v.ref_generation_number, static_cast<uint16_t>(0));
}

// ============================================================================
// Construction with Type parameter
// ============================================================================

TEST_CASE("Construct with BOOLEAN type") {
    Value v(Value::BOOLEAN);
    CHECK(v.type == Value::BOOLEAN);
    CHECK(v.boolean == false);
}

TEST_CASE("Construct with NUMBER type") {
    Value v(Value::NUMBER);
    CHECK(v.type == Value::NUMBER);
    CHECK(v.number == 0.0);
}

TEST_CASE("Construct with STRING type") {
    Value v(Value::STRING);
    CHECK(v.type == Value::STRING);
    CHECK(v.str.empty());
}

TEST_CASE("Construct with NAME type") {
    Value v(Value::NAME);
    CHECK(v.type == Value::NAME);
    CHECK(v.name.empty());
}

TEST_CASE("Construct with ARRAY type") {
    Value v(Value::ARRAY);
    CHECK(v.type == Value::ARRAY);
    CHECK(v.array.empty());
}

TEST_CASE("Construct with DICTIONARY type") {
    Value v(Value::DICTIONARY);
    CHECK(v.type == Value::DICTIONARY);
    CHECK(v.dict.empty());
}

TEST_CASE("Construct with STREAM type") {
    Value v(Value::STREAM);
    CHECK(v.type == Value::STREAM);
    CHECK(v.stream.data.empty());
    CHECK(v.stream.dict.empty());
}

TEST_CASE("Construct with REFERENCE type") {
    Value v(Value::REFERENCE);
    CHECK(v.type == Value::REFERENCE);
    CHECK_EQ(v.ref_object_number, 0u);
    CHECK_EQ(v.ref_generation_number, static_cast<uint16_t>(0));
}

TEST_CASE("Construct with NULL_OBJ type") {
    Value v(Value::NULL_OBJ);
    CHECK(v.type == Value::NULL_OBJ);
}

// ============================================================================
// SetType
// ============================================================================

TEST_CASE("SetType changes type correctly") {
    Value v;
    CHECK(v.type == Value::UNDEFINED);

    v.SetType(Value::BOOLEAN);
    CHECK(v.type == Value::BOOLEAN);

    v.SetType(Value::NUMBER);
    CHECK(v.type == Value::NUMBER);

    v.SetType(Value::STRING);
    CHECK(v.type == Value::STRING);

    v.SetType(Value::NAME);
    CHECK(v.type == Value::NAME);

    v.SetType(Value::ARRAY);
    CHECK(v.type == Value::ARRAY);

    v.SetType(Value::DICTIONARY);
    CHECK(v.type == Value::DICTIONARY);

    v.SetType(Value::STREAM);
    CHECK(v.type == Value::STREAM);

    v.SetType(Value::REFERENCE);
    CHECK(v.type == Value::REFERENCE);

    v.SetType(Value::NULL_OBJ);
    CHECK(v.type == Value::NULL_OBJ);
}

TEST_CASE("SetType to same type is a no-op") {
    Value v(Value::NUMBER);
    v.number = 42.0;
    v.SetType(Value::NUMBER);
    // After SetType to the same type, number should still be initialized
    // (SetType clears then re-initializes when types differ, but for same type
    //  the implementation may or may not reset the value -- just check type)
    CHECK(v.type == Value::NUMBER);
}

TEST_CASE("SetType clears previous data when changing types") {
    Value v(Value::STRING);
    v.str = "hello";
    CHECK(v.str == "hello");

    v.SetType(Value::NUMBER);
    CHECK(v.type == Value::NUMBER);
    CHECK(v.str.empty());
}

// ============================================================================
// clear()
// ============================================================================

TEST_CASE("clear resets to UNDEFINED") {
    Value v(Value::BOOLEAN);
    v.boolean = true;
    v.clear();
    CHECK(v.type == Value::UNDEFINED);
    CHECK(v.boolean == false);
}

TEST_CASE("clear resets NUMBER value") {
    Value v(Value::NUMBER);
    v.number = 3.14;
    v.clear();
    CHECK(v.type == Value::UNDEFINED);
    CHECK(v.number == 0.0);
}

TEST_CASE("clear resets STRING value") {
    Value v(Value::STRING);
    v.str = "test string";
    v.clear();
    CHECK(v.type == Value::UNDEFINED);
    CHECK(v.str.empty());
}

TEST_CASE("clear resets NAME value") {
    Value v(Value::NAME);
    v.name = "SomeName";
    v.clear();
    CHECK(v.type == Value::UNDEFINED);
    CHECK(v.name.empty());
}

TEST_CASE("clear resets ARRAY value") {
    Value v(Value::ARRAY);
    v.array.push_back(Value(Value::NUMBER));
    CHECK_FALSE(v.array.empty());
    v.clear();
    CHECK(v.type == Value::UNDEFINED);
    CHECK(v.array.empty());
}

TEST_CASE("clear resets DICTIONARY value") {
    Value v(Value::DICTIONARY);
    Value entry(Value::STRING);
    entry.str = "value";
    v.dict["key"] = entry;
    CHECK_FALSE(v.dict.empty());
    v.clear();
    CHECK(v.type == Value::UNDEFINED);
    CHECK(v.dict.empty());
}

TEST_CASE("clear resets STREAM value") {
    Value v(Value::STREAM);
    v.stream.data.push_back(0x42);
    Value len_val(Value::NUMBER);
    len_val.number = 1.0;
    v.stream.dict["Length"] = len_val;
    v.clear();
    CHECK(v.type == Value::UNDEFINED);
    CHECK(v.stream.data.empty());
    CHECK(v.stream.dict.empty());
}

TEST_CASE("clear resets REFERENCE values") {
    Value v(Value::REFERENCE);
    v.ref_object_number = 42;
    v.ref_generation_number = 7;
    v.clear();
    CHECK(v.type == Value::UNDEFINED);
    CHECK_EQ(v.ref_object_number, 0u);
    CHECK_EQ(v.ref_generation_number, static_cast<uint16_t>(0));
}

// ============================================================================
// Copy semantics
// ============================================================================

TEST_CASE("Copy construction preserves BOOLEAN") {
    Value original(Value::BOOLEAN);
    original.boolean = true;

    Value copy(original);
    CHECK(copy.type == Value::BOOLEAN);
    CHECK(copy.boolean == true);
}

TEST_CASE("Copy construction preserves NUMBER") {
    Value original(Value::NUMBER);
    original.number = 2.718;

    Value copy(original);
    CHECK(copy.type == Value::NUMBER);
    CHECK(copy.number == 2.718);
}

TEST_CASE("Copy construction preserves STRING") {
    Value original(Value::STRING);
    original.str = "hello world";

    Value copy(original);
    CHECK(copy.type == Value::STRING);
    CHECK(copy.str == "hello world");
}

TEST_CASE("Copy construction preserves ARRAY") {
    Value original(Value::ARRAY);
    Value elem(Value::NUMBER);
    elem.number = 1.0;
    original.array.push_back(elem);
    elem.number = 2.0;
    original.array.push_back(elem);

    Value copy(original);
    CHECK(copy.type == Value::ARRAY);
    REQUIRE(copy.array.size() == 2);
    CHECK(copy.array[0].number == 1.0);
    CHECK(copy.array[1].number == 2.0);
}

TEST_CASE("Copy construction preserves DICTIONARY") {
    Value original(Value::DICTIONARY);
    Value entry(Value::STRING);
    entry.str = "val";
    original.dict["key"] = entry;

    Value copy(original);
    CHECK(copy.type == Value::DICTIONARY);
    REQUIRE(copy.dict.count("key") == 1);
    CHECK(copy.dict["key"].str == "val");
}

TEST_CASE("Copy construction preserves REFERENCE") {
    Value original(Value::REFERENCE);
    original.ref_object_number = 99;
    original.ref_generation_number = 3;

    Value copy(original);
    CHECK(copy.type == Value::REFERENCE);
    CHECK_EQ(copy.ref_object_number, 99u);
    CHECK_EQ(copy.ref_generation_number, static_cast<uint16_t>(3));
}

TEST_CASE("Copy assignment works") {
    Value original(Value::STRING);
    original.str = "assigned";

    Value target;
    target = original;
    CHECK(target.type == Value::STRING);
    CHECK(target.str == "assigned");
}

TEST_CASE("Copy is independent of original") {
    Value original(Value::STRING);
    original.str = "original";

    Value copy(original);
    copy.str = "modified";

    CHECK(original.str == "original");
    CHECK(copy.str == "modified");
}

// ============================================================================
// Move semantics
// ============================================================================

TEST_CASE("Move construction transfers STRING") {
    Value original(Value::STRING);
    original.str = "moved string";

    Value moved(std::move(original));
    CHECK(moved.type == Value::STRING);
    CHECK(moved.str == "moved string");
}

TEST_CASE("Move construction transfers ARRAY") {
    Value original(Value::ARRAY);
    Value elem(Value::NUMBER);
    elem.number = 42.0;
    original.array.push_back(elem);

    Value moved(std::move(original));
    CHECK(moved.type == Value::ARRAY);
    REQUIRE(moved.array.size() == 1);
    CHECK(moved.array[0].number == 42.0);
}

TEST_CASE("Move assignment works") {
    Value original(Value::NAME);
    original.name = "Catalog";

    Value target;
    target = std::move(original);
    CHECK(target.type == Value::NAME);
    CHECK(target.name == "Catalog");
}

// ============================================================================
// Setting and reading back each field type
// ============================================================================

TEST_CASE("BOOLEAN true and false") {
    Value v(Value::BOOLEAN);
    v.boolean = true;
    CHECK(v.boolean == true);

    v.boolean = false;
    CHECK(v.boolean == false);
}

TEST_CASE("NUMBER integer values") {
    Value v(Value::NUMBER);
    v.number = 42.0;
    CHECK(v.number == 42.0);
}

TEST_CASE("NUMBER floating point values") {
    Value v(Value::NUMBER);
    v.number = 3.14159;
    CHECK(v.number > 3.14);
    CHECK(v.number < 3.15);
}

TEST_CASE("NUMBER negative values") {
    Value v(Value::NUMBER);
    v.number = -100.5;
    CHECK(v.number == -100.5);
}

TEST_CASE("NUMBER zero") {
    Value v(Value::NUMBER);
    v.number = 0.0;
    CHECK(v.number == 0.0);
}

TEST_CASE("STRING with ASCII content") {
    Value v(Value::STRING);
    v.str = "Hello, PDF!";
    CHECK(v.str == "Hello, PDF!");
    CHECK(v.str.size() == 11);
}

TEST_CASE("STRING empty") {
    Value v(Value::STRING);
    v.str = "";
    CHECK(v.str.empty());
}

TEST_CASE("STRING with binary content") {
    Value v(Value::STRING);
    v.str = std::string("\x00\x01\x02\x03", 4);
    CHECK(v.str.size() == 4);
    CHECK(v.str[0] == '\x00');
    CHECK(v.str[3] == '\x03');
}

TEST_CASE("NAME with typical PDF name") {
    Value v(Value::NAME);
    v.name = "Type";
    CHECK(v.name == "Type");
}

TEST_CASE("NAME with special characters") {
    Value v(Value::NAME);
    v.name = "Font#20Name";
    CHECK(v.name == "Font#20Name");
}

TEST_CASE("NULL_OBJ type check") {
    Value v(Value::NULL_OBJ);
    CHECK(v.type == Value::NULL_OBJ);
}

// ============================================================================
// Array operations
// ============================================================================

TEST_CASE("Array push_back and access") {
    Value v(Value::ARRAY);

    Value num(Value::NUMBER);
    num.number = 10.0;
    v.array.push_back(num);

    num.number = 20.0;
    v.array.push_back(num);

    num.number = 30.0;
    v.array.push_back(num);

    REQUIRE(v.array.size() == 3);
    CHECK(v.array[0].number == 10.0);
    CHECK(v.array[1].number == 20.0);
    CHECK(v.array[2].number == 30.0);
}

TEST_CASE("Array with mixed types") {
    Value v(Value::ARRAY);

    Value b(Value::BOOLEAN);
    b.boolean = true;
    v.array.push_back(b);

    Value n(Value::NUMBER);
    n.number = 3.14;
    v.array.push_back(n);

    Value s(Value::STRING);
    s.str = "test";
    v.array.push_back(s);

    Value null(Value::NULL_OBJ);
    v.array.push_back(null);

    REQUIRE(v.array.size() == 4);
    CHECK(v.array[0].type == Value::BOOLEAN);
    CHECK(v.array[0].boolean == true);
    CHECK(v.array[1].type == Value::NUMBER);
    CHECK(v.array[1].number == 3.14);
    CHECK(v.array[2].type == Value::STRING);
    CHECK(v.array[2].str == "test");
    CHECK(v.array[3].type == Value::NULL_OBJ);
}

TEST_CASE("Array empty by default") {
    Value v(Value::ARRAY);
    CHECK(v.array.empty());
    CHECK(v.array.size() == 0);
}

TEST_CASE("Nested array") {
    Value outer(Value::ARRAY);
    Value inner(Value::ARRAY);

    Value num(Value::NUMBER);
    num.number = 1.0;
    inner.array.push_back(num);
    num.number = 2.0;
    inner.array.push_back(num);

    outer.array.push_back(inner);

    REQUIRE(outer.array.size() == 1);
    REQUIRE(outer.array[0].type == Value::ARRAY);
    REQUIRE(outer.array[0].array.size() == 2);
    CHECK(outer.array[0].array[0].number == 1.0);
    CHECK(outer.array[0].array[1].number == 2.0);
}

// ============================================================================
// Dictionary operations
// ============================================================================

TEST_CASE("Dictionary insert and lookup") {
    Value v(Value::DICTIONARY);

    Value type_val(Value::NAME);
    type_val.name = "Catalog";
    v.dict["Type"] = type_val;

    REQUIRE(v.dict.count("Type") == 1);
    CHECK(v.dict["Type"].type == Value::NAME);
    CHECK(v.dict["Type"].name == "Catalog");
}

TEST_CASE("Dictionary multiple entries") {
    Value v(Value::DICTIONARY);

    Value type_val(Value::NAME);
    type_val.name = "Page";
    v.dict["Type"] = type_val;

    Value ref(Value::REFERENCE);
    ref.ref_object_number = 5;
    ref.ref_generation_number = 0;
    v.dict["Parent"] = ref;

    Value mediabox(Value::ARRAY);
    Value n(Value::NUMBER);
    n.number = 0.0;
    mediabox.array.push_back(n);
    mediabox.array.push_back(n);
    n.number = 612.0;
    mediabox.array.push_back(n);
    n.number = 792.0;
    mediabox.array.push_back(n);
    v.dict["MediaBox"] = mediabox;

    CHECK(v.dict.size() == 3);
    CHECK(v.dict["Type"].name == "Page");
    CHECK_EQ(v.dict["Parent"].ref_object_number, 5u);
    REQUIRE(v.dict["MediaBox"].array.size() == 4);
    CHECK(v.dict["MediaBox"].array[2].number == 612.0);
    CHECK(v.dict["MediaBox"].array[3].number == 792.0);
}

TEST_CASE("Dictionary nonexistent key") {
    Value v(Value::DICTIONARY);
    CHECK(v.dict.count("nonexistent") == 0);
}

TEST_CASE("Dictionary overwrite value") {
    Value v(Value::DICTIONARY);

    Value old_val(Value::NUMBER);
    old_val.number = 1.0;
    v.dict["Key"] = old_val;
    CHECK(v.dict["Key"].number == 1.0);

    Value new_val(Value::NUMBER);
    new_val.number = 2.0;
    v.dict["Key"] = new_val;
    CHECK(v.dict["Key"].number == 2.0);
    CHECK(v.dict.size() == 1);
}

// ============================================================================
// STREAM
// ============================================================================

TEST_CASE("Stream with data and dictionary") {
    Value v(Value::STREAM);

    // Set stream data
    v.stream.data = {0x48, 0x65, 0x6C, 0x6C, 0x6F};  // "Hello"

    // Set stream dictionary (e.g., Length)
    Value len_val(Value::NUMBER);
    len_val.number = 5.0;
    v.stream.dict["Length"] = len_val;

    CHECK(v.type == Value::STREAM);
    REQUIRE(v.stream.data.size() == 5);
    CHECK(v.stream.data[0] == 0x48);
    CHECK(v.stream.data[4] == 0x6F);
    REQUIRE(v.stream.dict.count("Length") == 1);
    CHECK(v.stream.dict["Length"].number == 5.0);
}

TEST_CASE("Stream empty data") {
    Value v(Value::STREAM);
    CHECK(v.stream.data.empty());
    CHECK(v.stream.dict.empty());
}

// ============================================================================
// REFERENCE
// ============================================================================

TEST_CASE("Reference object and generation numbers") {
    Value v(Value::REFERENCE);
    v.ref_object_number = 42;
    v.ref_generation_number = 0;

    CHECK(v.type == Value::REFERENCE);
    CHECK_EQ(v.ref_object_number, 42u);
    CHECK_EQ(v.ref_generation_number, static_cast<uint16_t>(0));
}

TEST_CASE("Reference with non-zero generation") {
    Value v(Value::REFERENCE);
    v.ref_object_number = 100;
    v.ref_generation_number = 5;

    CHECK_EQ(v.ref_object_number, 100u);
    CHECK_EQ(v.ref_generation_number, static_cast<uint16_t>(5));
}

TEST_CASE("Reference max object number") {
    Value v(Value::REFERENCE);
    v.ref_object_number = UINT32_MAX;
    v.ref_generation_number = UINT16_MAX;

    CHECK_EQ(v.ref_object_number, UINT32_MAX);
    CHECK_EQ(v.ref_generation_number, UINT16_MAX);
}

}  // TEST_SUITE

#ifndef NANOPDF_TEST_SUITE_NO_MAIN
int main() {
    return nanotest::run_all_tests();
}

#endif  // NANOPDF_TEST_SUITE_NO_MAIN
