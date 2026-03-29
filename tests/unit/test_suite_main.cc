#include "acutest.h"
#include "nanotest.hh"

// MCP JSON tests implemented in examples/mcp/test-mcp-json.cc
extern void test_parse_primitives();
extern void test_parse_array();
extern void test_parse_object();
extern void test_serialize();
extern void test_round_trip();
extern void test_unicode();

static void unit_nanotest_suite(void) {
  const int rc = nanotest::run_all_tests();
  TEST_CHECK_(rc == 0, "nanotest suite returned %d", rc);
}

static void unit_mcp_json_suite(void) {
  test_parse_primitives();
  test_parse_array();
  test_parse_object();
  test_serialize();
  test_round_trip();
  test_unicode();
  TEST_CHECK(1);
}

struct TEST_ENTRY TEST_LIST[] = {
    {"unit_nanotest_suite", unit_nanotest_suite},
    {"unit_mcp_json_suite", unit_mcp_json_suite},
    {NULL, NULL},
};
