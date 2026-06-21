# Integration tests - end-to-end PDF processing with real PDF files.
#
# All integration test .cc files are aggregated into a single binary
# (nanopdf_integration_suite). Each .cc auto-registers its TEST_CASEs into the
# nanotest global registry; their per-file main() is compiled out via
# NANOPDF_TEST_SUITE_NO_MAIN, and integration_suite_main.cc provides the one main().

set(NANOPDF_INTEGRATION_TEST_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/test_parse_real_pdfs.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/test_text_extraction.cc
)

add_executable(nanopdf_integration_suite
    ${NANOPDF_INTEGRATION_TEST_SOURCES}
    ${CMAKE_CURRENT_SOURCE_DIR}/integration_suite_main.cc
)
target_link_libraries(nanopdf_integration_suite PRIVATE
    nanopdf nanotest test_fixtures)
target_include_directories(nanopdf_integration_suite PRIVATE
    ${PROJECT_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/tests/fixtures
    ${CMAKE_SOURCE_DIR}/tests
)
target_compile_definitions(nanopdf_integration_suite PRIVATE
    NANOPDF_TEST_DATA_DIR="${CMAKE_SOURCE_DIR}/tests/data"
    NANOPDF_PROJECT_DIR="${PROJECT_SOURCE_DIR}"
    NANOPDF_TEST_SUITE_NO_MAIN=1
)
if(COMMAND add_sanitizers)
    add_sanitizers(nanopdf_integration_suite)
endif()

add_test(NAME nanopdf_integration_suite COMMAND nanopdf_integration_suite)
set_tests_properties(nanopdf_integration_suite PROPERTIES
    LABELS "integration"
    TIMEOUT 600
)
