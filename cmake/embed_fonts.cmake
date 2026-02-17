# embed_fonts.cmake
# Generates a C++ header file with embedded font data from binary files
# Usage: cmake -DFONTS_DIR=<dir> -DOUTPUT_FILE=<file> -P embed_fonts.cmake

if(NOT FONTS_DIR)
  message(FATAL_ERROR "FONTS_DIR not specified")
endif()

if(NOT OUTPUT_FILE)
  message(FATAL_ERROR "OUTPUT_FILE not specified")
endif()

# Find Python3
find_package(Python3 COMPONENTS Interpreter)

if(Python3_FOUND)
  # Use Python script for fast processing
  # Exclude CJK font directories — those are handled by embed_cjk_fonts.cmake
  execute_process(
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_LIST_DIR}/embed_fonts.py
      ${FONTS_DIR} ${OUTPUT_FILE}
      --exclude-dirs=noto-sans-jp,noto-serif-jp
    RESULT_VARIABLE RESULT
    OUTPUT_VARIABLE OUTPUT
    ERROR_VARIABLE ERROR
  )

  if(NOT RESULT EQUAL 0)
    message(FATAL_ERROR "Failed to generate embedded fonts header:\n${ERROR}")
  endif()

  message(STATUS "${OUTPUT}")
else()
  message(FATAL_ERROR "Python3 not found. Python3 is required to generate embedded fonts header.")
endif()
