#include "nanotest.hh"
#include "arlington_parser.hh"
#include "test_helpers.hh"

using namespace nanopdf::arlington;
using namespace nanopdf::test;

TEST_SUITE("ArlingtonParser") {

TEST_CASE("Parse type strings") {
    CHECK(parse_type("array") == ArlingtonType::Array);
    CHECK(parse_type("boolean") == ArlingtonType::Boolean);
    CHECK(parse_type("dictionary") == ArlingtonType::Dictionary);
    CHECK(parse_type("integer") == ArlingtonType::Integer);
    CHECK(parse_type("name") == ArlingtonType::Name);
    CHECK(parse_type("null") == ArlingtonType::Null);
    CHECK(parse_type("number") == ArlingtonType::Number);
    CHECK(parse_type("stream") == ArlingtonType::Stream);
    CHECK(parse_type("string") == ArlingtonType::String);
    CHECK(parse_type("name-tree") == ArlingtonType::NameTree);
    CHECK(parse_type("number-tree") == ArlingtonType::NumberTree);
    CHECK(parse_type("rectangle") == ArlingtonType::Rectangle);
    CHECK(parse_type("matrix") == ArlingtonType::Matrix);
    CHECK(parse_type("date") == ArlingtonType::Date);
    CHECK(parse_type("string-byte") == ArlingtonType::String);
    CHECK(parse_type("string-ascii") == ArlingtonType::String);
    CHECK(parse_type("string-text") == ArlingtonType::String);
    CHECK(parse_type("invalid") == ArlingtonType::Unknown);
}

TEST_CASE("Type to string round-trip") {
    CHECK_EQ(std::string(type_to_string(ArlingtonType::Array)), std::string("array"));
    CHECK_EQ(std::string(type_to_string(ArlingtonType::Dictionary)),
             std::string("dictionary"));
    CHECK_EQ(std::string(type_to_string(ArlingtonType::Stream)),
             std::string("stream"));
}

TEST_CASE("PdfVersion parsing") {
    auto v1 = PdfVersion::parse("1.4");
    CHECK_EQ(v1.major, 1);
    CHECK_EQ(v1.minor, 4);

    auto v2 = PdfVersion::parse("2.0");
    CHECK_EQ(v2.major, 2);
    CHECK_EQ(v2.minor, 0);

    auto v3 = PdfVersion::parse("");
    CHECK_EQ(v3.major, 1);
    CHECK_EQ(v3.minor, 0);
}

TEST_CASE("PdfVersion comparison") {
    PdfVersion v14(1, 4);
    PdfVersion v15(1, 5);
    PdfVersion v17(1, 7);
    PdfVersion v20(2, 0);

    CHECK(v14 < v15);
    CHECK(v15 < v17);
    CHECK(v17 < v20);
    CHECK(v14 <= v14);
    CHECK(v20 > v17);
    CHECK(v14 == PdfVersion(1, 4));
}

TEST_CASE("Load Catalog.tsv from Arlington data") {
    std::string arlington_dir = get_corpus_path("arlington/tsv/latest");
    SKIP_IF(!corpus_available("arlington"), "Arlington data not downloaded");

    ArlingtonModel model;
    // Try to load just Catalog.tsv to verify basic parsing
    std::string catalog_path = arlington_dir + "/Catalog.tsv";
    SKIP_IF(!file_exists(catalog_path),
            "Catalog.tsv not found in Arlington data");

    REQUIRE(model.load(arlington_dir));

    const ObjectDefinition* catalog = model.get_object_def("Catalog");
    REQUIRE(catalog != nullptr);
    CHECK_EQ(catalog->name, std::string("Catalog"));

    // Verify known keys
    const KeyDefinition* type_key = catalog->get_key("Type");
    CHECK(type_key != nullptr);
    if (type_key) {
        CHECK_FALSE(type_key->types.empty());
    }

    const KeyDefinition* pages_key = catalog->get_key("Pages");
    CHECK(pages_key != nullptr);
    if (pages_key) {
        CHECK(pages_key->is_unconditionally_required());
    }
}

TEST_CASE("Load full Arlington model") {
    std::string arlington_dir = get_corpus_path("arlington/tsv/latest");
    SKIP_IF(!corpus_available("arlington"), "Arlington data not downloaded");

    ArlingtonModel model;
    REQUIRE(model.load(arlington_dir));

    // Should have many definitions (Arlington has 600+ TSV files)
    CHECK(model.definition_count() > 100);

    // Verify some well-known definitions exist
    CHECK(model.get_object_def("Catalog") != nullptr);
    CHECK(model.get_object_def("FileTrailer") != nullptr);

    auto names = model.definition_names();
    CHECK(names.size() == model.definition_count());
}

}  // TEST_SUITE

#ifndef NANOPDF_TEST_SUITE_NO_MAIN
int main() {
    return nanotest::run_all_tests();
}
#endif
