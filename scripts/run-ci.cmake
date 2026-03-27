set(PROJECT_ROOT "${CMAKE_CURRENT_LIST_DIR}/..")
get_filename_component(PROJECT_ROOT "${PROJECT_ROOT}" ABSOLUTE)

set(DEFAULT_CONFIGS Release Debug)
if(DEFINED ENV{NANOPDF_CI_CONFIGS} AND NOT "$ENV{NANOPDF_CI_CONFIGS}" STREQUAL "")
  string(REPLACE "," ";" DEFAULT_CONFIGS "$ENV{NANOPDF_CI_CONFIGS}")
endif()

if(DEFINED ENV{NANOPDF_CI_BUILD_ROOT} AND NOT "$ENV{NANOPDF_CI_BUILD_ROOT}" STREQUAL "")
  set(BUILD_ROOT "$ENV{NANOPDF_CI_BUILD_ROOT}")
  get_filename_component(BUILD_ROOT "${BUILD_ROOT}" ABSOLUTE)
else()
  set(BUILD_ROOT "${PROJECT_ROOT}/build")
endif()

set(EXTRA_CONFIG_ARGS)
if(DEFINED ENV{NANOPDF_CI_CMAKE_ARGS} AND NOT "$ENV{NANOPDF_CI_CMAKE_ARGS}" STREQUAL "")
  separate_arguments(EXTRA_CONFIG_ARGS NATIVE_COMMAND "$ENV{NANOPDF_CI_CMAKE_ARGS}")
endif()

find_program(CTEST_COMMAND ctest)
if(NOT CTEST_COMMAND)
  message(FATAL_ERROR "ctest executable not found in PATH")
endif()


foreach(CONFIG IN LISTS DEFAULT_CONFIGS)
  string(TOLOWER "${CONFIG}" CONFIG_LOWER)
  set(BINARY_DIR "${BUILD_ROOT}/ci-${CONFIG_LOWER}")

  message(STATUS "[nanopdf-ci] Configure ${CONFIG} in ${BINARY_DIR}")
  execute_process(
    COMMAND "${CMAKE_COMMAND}" -S "${PROJECT_ROOT}" -B "${BINARY_DIR}" -DCMAKE_BUILD_TYPE=${CONFIG} -DNANOPDF_BUILD_TESTS=ON -DNANOPDF_USE_CCACHE=OFF ${EXTRA_CONFIG_ARGS}
    RESULT_VARIABLE CONFIGURE_RESULT
    OUTPUT_VARIABLE CONFIGURE_OUT
    ERROR_VARIABLE CONFIGURE_ERR
  )
  if(CONFIGURE_RESULT)
    message("${CONFIGURE_OUT}")
    message(FATAL_ERROR "Configuration failed for ${CONFIG}: ${CONFIGURE_ERR}")
  endif()

  message(STATUS "[nanopdf-ci] Build ${CONFIG}")
  execute_process(
    COMMAND "${CMAKE_COMMAND}" --build "${BINARY_DIR}"
    RESULT_VARIABLE BUILD_RESULT
    OUTPUT_VARIABLE BUILD_OUT
    ERROR_VARIABLE BUILD_ERR
  )
  if(BUILD_RESULT)
    message("${BUILD_OUT}")
    message(FATAL_ERROR "Build failed for ${CONFIG}: ${BUILD_ERR}")
  endif()

  message(STATUS "[nanopdf-ci] Test ${CONFIG}")
  execute_process(
    COMMAND "${CTEST_COMMAND}" --output-on-failure -C "${CONFIG}"
    WORKING_DIRECTORY "${BINARY_DIR}"
    RESULT_VARIABLE TEST_RESULT
    OUTPUT_VARIABLE TEST_OUT
    ERROR_VARIABLE TEST_ERR
  )
  if(TEST_RESULT)
    message("${TEST_OUT}")
    message(FATAL_ERROR "Tests failed for ${CONFIG}: ${TEST_ERR}")
  endif()

endforeach()

message(STATUS "[nanopdf-ci] All configurations passed")
