# Validation test infrastructure for PDF specification compliance testing.
#
# All validation test .cc files are aggregated into a single binary
# (nanopdf_validation_suite). Each .cc auto-registers its TEST_CASEs into the
# nanotest global registry; their per-file main() is compiled out via
# NANOPDF_TEST_SUITE_NO_MAIN, and validation_suite_main.cc provides the one main().

# Arlington parser + validator library
add_library(nanopdf_validation STATIC
    arlington_parser.cc
    arlington_validator.cc
)
target_link_libraries(nanopdf_validation PUBLIC nanopdf)
target_include_directories(nanopdf_validation PUBLIC
    ${PROJECT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}
)

set(NANOPDF_VALIDATION_TEST_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/test_arlington_parser.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/test_arlington_validate.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/test_safedocs.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/test_pdf_differences.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/test_corpus_parsing.cc
)

add_executable(nanopdf_validation_suite
    ${NANOPDF_VALIDATION_TEST_SOURCES}
    ${CMAKE_CURRENT_SOURCE_DIR}/validation_suite_main.cc
)
target_link_libraries(nanopdf_validation_suite PRIVATE
    nanopdf nanopdf_validation nanotest test_fixtures)
target_include_directories(nanopdf_validation_suite PRIVATE
    ${PROJECT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/tests/fixtures
    ${CMAKE_SOURCE_DIR}/tests
)
target_compile_definitions(nanopdf_validation_suite PRIVATE
    NANOPDF_TEST_DATA_DIR="${CMAKE_SOURCE_DIR}/tests/data"
    NANOPDF_PROJECT_DIR="${PROJECT_SOURCE_DIR}"
    NANOPDF_TEST_SUITE_NO_MAIN=1
)
if(COMMAND add_sanitizers)
    add_sanitizers(nanopdf_validation_suite)
endif()

# Single registration; the longest individual budget (corpus/safedocs/diffs) governs.
add_test(NAME nanopdf_validation_suite COMMAND nanopdf_validation_suite)
set_tests_properties(nanopdf_validation_suite PROPERTIES
    LABELS "validation"
    TIMEOUT 3600
)
