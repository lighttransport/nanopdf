# nanopdf Test Infrastructure
# Uses nanotest (custom lightweight testing library) for unit and integration testing

# Include nanotest as interface library
add_library(nanotest INTERFACE)
target_include_directories(nanotest INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR})

# Test fixtures library (compiled once, reused by all tests)
add_library(test_fixtures STATIC
    fixtures/test_helpers.cc
)
target_link_libraries(test_fixtures PUBLIC nanopdf nanotest)
target_include_directories(test_fixtures PUBLIC
    ${PROJECT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/fixtures
    ${CMAKE_CURRENT_SOURCE_DIR}
)
target_compile_definitions(test_fixtures PRIVATE
    NANOPDF_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data"
)

# Integration and validation tests are each aggregated into a single binary in
# their respective subdirectories (see integration/ and validation/ CMakeLists).

# Unit test suite: single binary target.
set(NANOPDF_UNIT_TEST_SOURCES
    unit/core/test_value.cc
    unit/core/test_xref.cc
    unit/core/test_pdf_parsing.cc
    unit/core/test_pdf_function.cc
    unit/annotations/test_annotation_types.cc
    unit/annotations/test_freetext_annotation.cc
    unit/annotations/test_link_annotation.cc
    unit/annotations/test_markup_annotation.cc
    unit/annotations/test_text_annotation.cc
    unit/document/test_document_catalog.cc
    unit/document/test_document_info.cc
    unit/document/test_named_destinations.cc
    unit/document/test_outline.cc
    unit/document/test_page_labels.cc
    unit/filters/test_ascii85_decode.cc
    unit/filters/test_asciihex_decode.cc
    unit/filters/test_ccitt_decode.cc
    unit/filters/test_dct_decode.cc
    unit/filters/test_filter_chains.cc
    unit/filters/test_flate_decode.cc
    unit/filters/test_lzw_decode.cc
    unit/filters/test_png_predictors.cc
    unit/filters/test_runlength_decode.cc
    unit/fonts/test_cmap.cc
    unit/fonts/test_cff_wrapper.cc
    unit/fonts/test_font_substitution.cc
    unit/fonts/test_type0_font.cc
    unit/fonts/test_type3_font.cc
    unit/forms/test_button_field.cc
    unit/forms/test_choice_field.cc
    unit/forms/test_form_field_types.cc
    unit/forms/test_text_field.cc
    unit/security/test_aes.cc
    unit/security/test_hash.cc
    unit/security/test_rc4.cc
    unit/security/test_rsa_pk.cc
    unit/security/test_ecc.cc
    unit/security/test_x509.cc
    unit/security/test_asn1_cms.cc
    unit/security/test_tls_crypto.cc
    unit/text/test_text_extraction.cc
    unit/text/test_text_state.cc
    unit/text/test_text_layout.cc
    unit/text/test_table_extraction.cc
    unit/document/test_attachments.cc
    unit/document/test_document_structure.cc
    unit/document/test_pdfa_validation.cc
    unit/document/test_revision_history.cc
    unit/document/test_signature_validation.cc
    unit/graphics/test_color_transform.cc
    unit/backends/test_render_progress.cc
    unit/backends/test_focal_radial_gradient.cc
    unit/filters/test_tiff_decode.cc
    unit/forms/test_form_manipulation.cc
    unit/forms/test_form_writeback.cc
)

add_executable(nanopdf_unit_suite
    ${NANOPDF_UNIT_TEST_SOURCES}
    unit/test_suite_main.cc
    ${PROJECT_SOURCE_DIR}/examples/mcp/test-mcp-json.cc
)
target_link_libraries(nanopdf_unit_suite PRIVATE nanopdf nanopdf_mcp nanotest test_fixtures)
target_include_directories(nanopdf_unit_suite PRIVATE
    ${PROJECT_SOURCE_DIR}
    ${PROJECT_SOURCE_DIR}/src
    ${PROJECT_SOURCE_DIR}/src/third_party
    ${CMAKE_CURRENT_SOURCE_DIR}/fixtures
    ${CMAKE_CURRENT_SOURCE_DIR}
)
target_compile_definitions(nanopdf_unit_suite PRIVATE
    NANOPDF_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data"
    NANOPDF_TEST_SUITE_NO_MAIN=1
)
if(COMMAND add_sanitizers)
    add_sanitizers(nanopdf_unit_suite)
endif()
add_test(NAME nanopdf_unit_suite COMMAND nanopdf_unit_suite)
set_tests_properties(nanopdf_unit_suite PROPERTIES LABELS "unit")

# Integration tests
add_subdirectory(integration)

# Validation tests (PDF specification compliance)
option(NANOPDF_BUILD_VALIDATION_TESTS "Build validation test harness" ON)
if(NANOPDF_BUILD_VALIDATION_TESTS)
    add_subdirectory(validation)
endif()

# Custom targets for selective testing
add_custom_target(test_unit
    COMMAND ${CMAKE_CTEST_COMMAND} -R nanopdf_unit_suite --output-on-failure
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    COMMENT "Running unit tests"
)

add_custom_target(test_integration
    COMMAND ${CMAKE_CTEST_COMMAND} -L integration --output-on-failure
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    COMMENT "Running integration tests"
)

# Target to run all tests
add_custom_target(test_all
    COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    COMMENT "Running all tests"
)

# Target to run validation tests only
add_custom_target(test_validation
    COMMAND ${CMAKE_CTEST_COMMAND} -L validation --output-on-failure
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    COMMENT "Running validation tests"
)
