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

# Helper function for creating unit tests
# Creates one executable per test file for fast incremental builds
function(add_nanopdf_unit_test test_name test_source)
    add_executable(${test_name} ${test_source})
    target_link_libraries(${test_name} PRIVATE nanopdf nanotest test_fixtures)
    target_include_directories(${test_name} PRIVATE
        ${PROJECT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/fixtures
        ${CMAKE_CURRENT_SOURCE_DIR}
    )

    # Define test data directory path for accessing test PDFs and resources
    target_compile_definitions(${test_name} PRIVATE
        NANOPDF_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data"
    )

    # Add sanitizers if enabled
    if(COMMAND add_sanitizers)
        add_sanitizers(${test_name})
    endif()

    # Register with CTest
    add_test(NAME ${test_name} COMMAND ${test_name})
    set_tests_properties(${test_name} PROPERTIES LABELS "unit")
endfunction()

# Helper function for creating integration tests
function(add_nanopdf_integration_test test_name test_source)
    add_executable(${test_name} ${test_source})
    target_link_libraries(${test_name} PRIVATE nanopdf nanotest test_fixtures)
    target_include_directories(${test_name} PRIVATE
        ${PROJECT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/fixtures
        ${CMAKE_CURRENT_SOURCE_DIR}
    )

    target_compile_definitions(${test_name} PRIVATE
        NANOPDF_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data"
    )

    if(COMMAND add_sanitizers)
        add_sanitizers(${test_name})
    endif()

    add_test(NAME ${test_name} COMMAND ${test_name})
    set_tests_properties(${test_name} PROPERTIES
        LABELS "integration"
        TIMEOUT 300  # Integration tests may take longer
    )
endfunction()

# Add subdirectories for each test category
add_subdirectory(unit/core)
add_subdirectory(unit/filters)
add_subdirectory(unit/fonts)
add_subdirectory(unit/graphics)
add_subdirectory(unit/text)
add_subdirectory(unit/annotations)
add_subdirectory(unit/forms)
add_subdirectory(unit/security)
add_subdirectory(unit/document)

# Conditional backend tests
if(NANOPDF_USE_THORVG)
    add_subdirectory(unit/backends)
endif()

# Integration tests
add_subdirectory(integration)

# Validation tests (PDF specification compliance)
option(NANOPDF_BUILD_VALIDATION_TESTS "Build validation test harness" ON)
if(NANOPDF_BUILD_VALIDATION_TESTS)
    add_subdirectory(validation)
endif()

# Custom targets for selective testing
add_custom_target(test_unit
    COMMAND ${CMAKE_CTEST_COMMAND} -L unit --output-on-failure
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
