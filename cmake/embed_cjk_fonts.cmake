# embed_cjk_fonts.cmake
# Generates a C++ header file with embedded CJK font data (Noto Sans/Serif JP)
# Usage: cmake -DFONTS_DIR=<dir> -DOUTPUT_FILE=<file> -P embed_cjk_fonts.cmake

if(NOT FONTS_DIR)
  message(FATAL_ERROR "FONTS_DIR not specified")
endif()

if(NOT OUTPUT_FILE)
  message(FATAL_ERROR "OUTPUT_FILE not specified")
endif()

# Find Python3
find_package(Python3 COMPONENTS Interpreter)

if(Python3_FOUND)
  # Use Python script — only include CJK font directories
  execute_process(
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_LIST_DIR}/embed_fonts.py
      ${FONTS_DIR} ${OUTPUT_FILE}
      --include-only-dirs=noto-sans-jp,noto-serif-jp
      --header-guard=NANOPDF_EMBEDDED_CJK_FONTS_HH
      --namespace=embedded_cjk_fonts
      --skip-pdf-mapping
    RESULT_VARIABLE RESULT
    OUTPUT_VARIABLE OUTPUT
    ERROR_VARIABLE ERROR
  )

  if(NOT RESULT EQUAL 0)
    message(FATAL_ERROR "Failed to generate embedded CJK fonts header:\n${ERROR}")
  endif()

  message(STATUS "${OUTPUT}")
else()
  message(FATAL_ERROR "Python3 not found. Python3 is required to generate embedded CJK fonts header.")
endif()
