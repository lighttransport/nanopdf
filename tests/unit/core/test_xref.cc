/**
 * XRef structure unit tests
 *
 * Tests the XRef and XRefSection structures that track the byte offsets and
 * generation numbers of indirect objects in a PDF file.
 */

#include "nanotest.hh"
#include "nanopdf.hh"
#include "test_helpers.hh"

using namespace nanopdf;

TEST_SUITE("XRef") {

// ============================================================================
// XRef default values
// ============================================================================

TEST_CASE("XRef default construction") {
    XRef xref;
    CHECK_EQ(xref.offset, static_cast<uint64_t>(0));
    CHECK_EQ(xref.generation, static_cast<uint16_t>(65535));
    CHECK(xref.use == false);
}

TEST_CASE("XRef set offset") {
    XRef xref;
    xref.offset = 12345;
    CHECK_EQ(xref.offset, static_cast<uint64_t>(12345));
}

TEST_CASE("XRef set generation") {
    XRef xref;
    xref.generation = 0;
    CHECK_EQ(xref.generation, static_cast<uint16_t>(0));
}

TEST_CASE("XRef mark as used") {
    XRef xref;
    CHECK(xref.use == false);
    xref.use = true;
    CHECK(xref.use == true);
}

TEST_CASE("XRef typical in-use entry") {
    // A typical in-use xref entry: known offset, generation 0, in use
    XRef xref;
    xref.offset = 9;
    xref.generation = 0;
    xref.use = true;

    CHECK_EQ(xref.offset, static_cast<uint64_t>(9));
    CHECK_EQ(xref.generation, static_cast<uint16_t>(0));
    CHECK(xref.use == true);
}

TEST_CASE("XRef free entry (default)") {
    // The default XRef matches the first xref entry (object 0):
    // generation 65535, not in use
    XRef xref;
    CHECK(xref.use == false);
    CHECK_EQ(xref.generation, static_cast<uint16_t>(65535));
}

TEST_CASE("XRef large offset") {
    XRef xref;
    xref.offset = 0xFFFFFFFFULL;  // 4 GB offset
    CHECK_EQ(xref.offset, static_cast<uint64_t>(0xFFFFFFFFULL));
}

TEST_CASE("XRef very large offset (> 4GB)") {
    // PDF files can exceed 4 GB, offset is uint64_t
    XRef xref;
    xref.offset = 0x1FFFFFFFFULL;  // 8 GB offset
    CHECK_EQ(xref.offset, static_cast<uint64_t>(0x1FFFFFFFFULL));
}

// ============================================================================
// XRefSection default values
// ============================================================================

TEST_CASE("XRefSection default construction") {
    XRefSection section;
    CHECK(section.xrefs.empty());
    CHECK_EQ(section.start_object_id, 0u);
    CHECK_EQ(section.num_objectsid, 0u);
}

TEST_CASE("XRefSection with start object id") {
    XRefSection section;
    section.start_object_id = 10;
    CHECK_EQ(section.start_object_id, 10u);
}

TEST_CASE("XRefSection with num objects") {
    XRefSection section;
    section.num_objectsid = 5;
    CHECK_EQ(section.num_objectsid, 5u);
}

// ============================================================================
// XRefSection with xrefs populated
// ============================================================================

TEST_CASE("XRefSection single entry") {
    XRefSection section;
    section.start_object_id = 0;
    section.num_objectsid = 1;

    XRef entry;
    entry.offset = 0;
    entry.generation = 65535;
    entry.use = false;
    section.xrefs.push_back(entry);

    REQUIRE(section.xrefs.size() == 1);
    CHECK(section.xrefs[0].use == false);
    CHECK_EQ(section.xrefs[0].generation, static_cast<uint16_t>(65535));
}

TEST_CASE("XRefSection typical PDF xref table") {
    // Simulate the xref table from a minimal PDF:
    // 0 3
    // 0000000000 65535 f   (object 0, free)
    // 0000000009 00000 n   (object 1, in use)
    // 0000000058 00000 n   (object 2, in use)
    XRefSection section;
    section.start_object_id = 0;
    section.num_objectsid = 3;

    // Object 0: free entry
    XRef obj0;
    obj0.offset = 0;
    obj0.generation = 65535;
    obj0.use = false;
    section.xrefs.push_back(obj0);

    // Object 1: catalog at offset 9
    XRef obj1;
    obj1.offset = 9;
    obj1.generation = 0;
    obj1.use = true;
    section.xrefs.push_back(obj1);

    // Object 2: pages at offset 58
    XRef obj2;
    obj2.offset = 58;
    obj2.generation = 0;
    obj2.use = true;
    section.xrefs.push_back(obj2);

    REQUIRE(section.xrefs.size() == 3);

    // Verify free entry
    CHECK(section.xrefs[0].use == false);
    CHECK_EQ(section.xrefs[0].offset, static_cast<uint64_t>(0));
    CHECK_EQ(section.xrefs[0].generation, static_cast<uint16_t>(65535));

    // Verify in-use entries
    CHECK(section.xrefs[1].use == true);
    CHECK_EQ(section.xrefs[1].offset, static_cast<uint64_t>(9));
    CHECK_EQ(section.xrefs[1].generation, static_cast<uint16_t>(0));

    CHECK(section.xrefs[2].use == true);
    CHECK_EQ(section.xrefs[2].offset, static_cast<uint64_t>(58));
    CHECK_EQ(section.xrefs[2].generation, static_cast<uint16_t>(0));
}

TEST_CASE("XRefSection multiple sections") {
    // PDF incremental updates produce multiple xref sections
    XRefSection section1;
    section1.start_object_id = 0;
    section1.num_objectsid = 3;
    section1.xrefs.resize(3);
    section1.xrefs[0] = {0, 65535, false};
    section1.xrefs[1] = {9, 0, true};
    section1.xrefs[2] = {58, 0, true};

    XRefSection section2;
    section2.start_object_id = 3;
    section2.num_objectsid = 2;
    section2.xrefs.resize(2);
    section2.xrefs[0] = {200, 0, true};  // object 3
    section2.xrefs[1] = {300, 0, true};  // object 4

    CHECK_EQ(section1.xrefs.size(), static_cast<size_t>(3));
    CHECK_EQ(section2.xrefs.size(), static_cast<size_t>(2));
    CHECK_EQ(section2.start_object_id, 3u);
    CHECK(section2.xrefs[0].use == true);
    CHECK_EQ(section2.xrefs[0].offset, static_cast<uint64_t>(200));
}

TEST_CASE("XRef entry marking as used then free") {
    XRef xref;
    CHECK(xref.use == false);

    xref.use = true;
    xref.generation = 0;
    xref.offset = 1000;
    CHECK(xref.use == true);

    // Mark as free (e.g., object deletion in incremental update)
    xref.use = false;
    xref.generation = 1;  // generation incremented on free
    CHECK(xref.use == false);
    CHECK_EQ(xref.generation, static_cast<uint16_t>(1));
}

TEST_CASE("XRefSection empty xrefs vector") {
    XRefSection section;
    section.start_object_id = 0;
    section.num_objectsid = 0;

    CHECK(section.xrefs.empty());
    CHECK_EQ(section.xrefs.size(), static_cast<size_t>(0));
}

TEST_CASE("XRefSection large number of objects") {
    XRefSection section;
    section.start_object_id = 0;
    section.num_objectsid = 1000;
    section.xrefs.resize(1000);

    // Fill with sequential offsets
    for (uint32_t i = 0; i < 1000; i++) {
        section.xrefs[i].offset = i * 100;
        section.xrefs[i].generation = 0;
        section.xrefs[i].use = (i > 0);  // object 0 is always free
    }

    CHECK_EQ(section.xrefs.size(), static_cast<size_t>(1000));
    CHECK(section.xrefs[0].use == false);
    CHECK(section.xrefs[1].use == true);
    CHECK(section.xrefs[999].use == true);
    CHECK_EQ(section.xrefs[999].offset, static_cast<uint64_t>(99900));
}

}  // TEST_SUITE

#ifndef NANOPDF_TEST_SUITE_NO_MAIN
int main() {
    return nanotest::run_all_tests();
}

#endif  // NANOPDF_TEST_SUITE_NO_MAIN
