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

set(PHASE_EXECUTABLES
  test_phase1
  test_phase1_simple
  test_phase2
  test_phase2_standardencoding
  test_phase2_rendering
  test_phase2_realpdf
  test_phase2_text
  test_phase3
  test_phase4
  test_phase5
  test_phase6
)

file(GLOB_RECURSE SAMPLE_PDFS LIST_DIRECTORIES false "${PROJECT_ROOT}/data/*.pdf")
list(SORT SAMPLE_PDFS)

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

  set(EXEC_SUFFIX "")
  if(WIN32)
    set(EXEC_SUFFIX ".exe")
  endif()

  foreach(PHASE_EXE IN LISTS PHASE_EXECUTABLES)
    set(EXEC_PATH "${BINARY_DIR}/${PHASE_EXE}${EXEC_SUFFIX}")
    if(NOT EXISTS "${EXEC_PATH}")
      message(FATAL_ERROR "Expected phase executable ${EXEC_PATH} was not generated")
    endif()

    set(PHASE_ARGS)
    set(PHASE_DISPLAY "")
    if(SAMPLE_PDFS)
      list(GET SAMPLE_PDFS 0 SAMPLE_PDF)
      list(APPEND PHASE_ARGS "${SAMPLE_PDF}")
      file(RELATIVE_PATH PHASE_DISPLAY "${PROJECT_ROOT}" "${SAMPLE_PDF}")
    endif()

    if(PHASE_DISPLAY)
      message(STATUS "[nanopdf-ci] Run ${PHASE_EXE} (${PHASE_DISPLAY})")
    else()
      message(STATUS "[nanopdf-ci] Run ${PHASE_EXE}")
    endif()

    execute_process(
      COMMAND "${EXEC_PATH}" ${PHASE_ARGS}
      WORKING_DIRECTORY "${BINARY_DIR}"
      RESULT_VARIABLE PHASE_RESULT
      COMMAND_ECHO STDOUT
    )
    if(PHASE_RESULT)
      message(FATAL_ERROR "Phase executable ${PHASE_EXE} failed with exit code ${PHASE_RESULT}")
    endif()
  endforeach()
endforeach()

message(STATUS "[nanopdf-ci] All configurations passed")
