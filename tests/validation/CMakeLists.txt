# Validation test infrastructure for PDF specification compliance testing

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

# Helper function for validation tests
function(add_nanopdf_validation_test test_name test_source)
    add_executable(${test_name} ${test_source})
    target_link_libraries(${test_name} PRIVATE
        nanopdf nanopdf_validation nanotest test_fixtures)
    target_include_directories(${test_name} PRIVATE
        ${PROJECT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_SOURCE_DIR}/tests/fixtures
        ${CMAKE_SOURCE_DIR}/tests
    )

    target_compile_definitions(${test_name} PRIVATE
        NANOPDF_TEST_DATA_DIR="${CMAKE_SOURCE_DIR}/tests/data"
        NANOPDF_PROJECT_DIR="${PROJECT_SOURCE_DIR}"
    )

    if(COMMAND add_sanitizers)
        add_sanitizers(${test_name})
    endif()

    add_test(NAME ${test_name} COMMAND ${test_name})
    set_tests_properties(${test_name} PROPERTIES
        LABELS "validation"
        TIMEOUT ${ARGV2}
    )
endfunction()

# Test executables
add_nanopdf_validation_test(test_arlington_parser
    test_arlington_parser.cc 600)

add_nanopdf_validation_test(test_arlington_validate
    test_arlington_validate.cc 600)

add_nanopdf_validation_test(test_safedocs
    test_safedocs.cc 3600)

add_nanopdf_validation_test(test_pdf_differences
    test_pdf_differences.cc 3600)

add_nanopdf_validation_test(test_corpus_parsing
    test_corpus_parsing.cc 3600)
