# Integration tests - end-to-end PDF processing with real PDF files

# Integration tests: parse real PDFs from the project data/ directory
add_nanopdf_integration_test(test_parse_real_pdfs
    ${CMAKE_CURRENT_SOURCE_DIR}/test_parse_real_pdfs.cc)

add_nanopdf_integration_test(test_text_extraction
    ${CMAKE_CURRENT_SOURCE_DIR}/test_text_extraction.cc)

# Provide the project root so tests can locate data/ PDFs
target_compile_definitions(test_parse_real_pdfs PRIVATE
    NANOPDF_PROJECT_DIR="${PROJECT_SOURCE_DIR}")
target_compile_definitions(test_text_extraction PRIVATE
    NANOPDF_PROJECT_DIR="${PROJECT_SOURCE_DIR}")
