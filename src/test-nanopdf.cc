#include "nanopdf.hh"
#include "canvas-exporter.hh"

#include <cstdio>
#include <vector>
#include <string>
#include <fstream>
#include <iostream>
#include <limits>

static bool ReadWholeFile(std::vector<uint8_t> *out, std::string *err,
                   const std::string &filepath, size_t filesize_max,
                   void *userdata) {
  (void)userdata;

#ifdef _WIN32
#if defined(__GLIBCXX__)  // mingw
  int file_descriptor =
      _wopen(UTF8ToWchar(filepath).c_str(), _O_RDONLY | _O_BINARY);
  __gnu_cxx::stdio_filebuf<char> wfile_buf(file_descriptor, std::ios_base::in);
  std::istream f(&wfile_buf);
#elif defined(_MSC_VER) || defined(_LIBCPP_VERSION)
  // For libcxx, assume _LIBCPP_HAS_OPEN_WITH_WCHAR is defined to accept
  // `wchar_t *`
  std::ifstream f(UTF8ToWchar(filepath).c_str(), std::ifstream::binary);
#else
  // Unknown compiler/runtime
  std::ifstream f(filepath.c_str(), std::ifstream::binary);
#endif
#else
  std::ifstream f(filepath.c_str(), std::ifstream::binary);
#endif
  if (!f) {
    if (err) {
      (*err) += "File open error : " + filepath + "\n";
    }
    return false;
  }

  // For directory(and pipe?), peek() will fail(Posix gnustl/libc++ only)
  int buf = f.peek();
  (void)buf;
  if (!f) {
    if (err) {
      (*err) += "File read error. Maybe empty file or invalid file : " + filepath + "\n";
    }
    return false;
  }

  f.seekg(0, f.end);
  size_t sz = static_cast<size_t>(f.tellg());
  f.seekg(0, f.beg);

  if (int64_t(sz) < 0) {
    if (err) {
      (*err) += "Invalid file size : " + filepath +
                " (does the path point to a directory?)";
    }
    return false;
  } else if (sz == 0) {
    if (err) {
      (*err) += "File is empty : " + filepath + "\n";
    }
    return false;
  } else if (uint64_t(sz) >= uint64_t(std::numeric_limits<int64_t>::max())) {
    // Posixish environment.
    if (err) {
      (*err) += "Invalid File(Pipe or special device?) : " + filepath + "\n";
    }
    return false;
  }

  if ((filesize_max > 0) && (sz > filesize_max)) {
    if (err) {
      (*err) += "File size is too large : " + filepath +
                " sz = " + std::to_string(sz) +
                ", allowed max filesize = " + std::to_string(filesize_max) +
                "\n";
    }
    return false;
  }

  out->resize(sz);
  f.read(reinterpret_cast<char *>(&out->at(0)),
         static_cast<std::streamsize>(sz));

  if (!f) {
    // read failure.
    if (err) {
      (*err) += "Failed to read file: " + filepath + "\n";
    }
    return false;
  }

  return true;
}

int main(int argc, char **argv) {

  if (argc < 2) {
    printf("Needs input.pdf\n");
    return -1;
  }

  std::vector<uint8_t> buf;
  std::string err;
  std::string filepath = argv[1];
  size_t size_max = 1024*1024*1024; // Up to 1GB

  if (!ReadWholeFile(&buf, &err, filepath, size_max, nullptr)) {
    std::cerr << err << "\n";
    return -2;
  }

  nanopdf::Pdf pdf;
  if (!nanopdf::parse_from_memory(buf.data(), buf.size(), &pdf)) {
    std::cerr << "Failed to parse PDF\n";
    return -3;
  }

  if (!pdf.load_document_structure()) {
    std::cerr << "Failed to load document structure\n";
    return -4;
  }

  std::cout << "PDF parsed successfully. Pages: " << pdf.catalog.pages_count << "\n";

  // Parse signature fields
  if (pdf.parse_signature_fields()) {
    std::cout << "Signature fields parsed. Found " << pdf.catalog.signature_fields.size() << " signature field(s)\n";
    
    for (size_t i = 0; i < pdf.catalog.signature_fields.size(); ++i) {
      const auto& sig_field = pdf.catalog.signature_fields[i];
      std::cout << "Signature Field " << (i + 1) << ":\n";
      std::cout << "  Name: " << sig_field.name << "\n";
      std::cout << "  Signed: " << (sig_field.is_signed ? "Yes" : "No") << "\n";
      
      if (sig_field.is_signed) {
        if (!sig_field.signing_reason.empty()) {
          std::cout << "  Reason: " << sig_field.signing_reason << "\n";
        }
        if (!sig_field.signing_location.empty()) {
          std::cout << "  Location: " << sig_field.signing_location << "\n";
        }
        if (!sig_field.signing_contact_info.empty()) {
          std::cout << "  Contact: " << sig_field.signing_contact_info << "\n";
        }
        if (!sig_field.signing_date.empty()) {
          std::cout << "  Date: " << sig_field.signing_date << "\n";
        }
      }
      
      if (sig_field.rect.size() == 4) {
        std::cout << "  Rectangle: [" << sig_field.rect[0] << ", " << sig_field.rect[1] 
                  << ", " << sig_field.rect[2] << ", " << sig_field.rect[3] << "]\n";
      }
    }
  } else {
    std::cerr << "Failed to parse signature fields\n";
  }

  if (pdf.catalog.pages_count > 0) {
    const nanopdf::Page* first_page = pdf.get_page(0);
    if (first_page) {
      std::cout << "Exporting first page to HTML5 Canvas commands...\n";
      
      nanopdf::CanvasExporter exporter;
      nanopdf::CanvasExportResult result = exporter.export_page(pdf, *first_page);
      
      if (result.success) {
        std::cout << "Canvas export successful!\n";
        std::cout << "Page dimensions: " << result.width << "x" << result.height << "\n";
        std::cout << "Generated " << result.commands.size() << " canvas commands\n\n";
        
        std::string js_code = exporter.commands_to_javascript(result.commands, "pdfCanvas");
        std::cout << "JavaScript code:\n";
        std::cout << js_code << std::endl;
        
        std::string json_code = exporter.commands_to_json(result.commands);
        std::cout << "\nCanvas JSON export:\n";
        std::cout << json_code << std::endl;
        
        std::cout << "\nExporting to SVG...\n";
        nanopdf::SvgExportResult svg_result = exporter.export_page_to_svg(pdf, *first_page);
        
        if (svg_result.success) {
          std::cout << "SVG export successful!\n";
          std::cout << "Generated " << svg_result.elements.size() << " SVG elements\n";
          
          std::string svg_json_code = exporter.svg_to_json(svg_result.elements);
          std::cout << "\nSVG JSON export:\n";
          std::cout << svg_json_code << std::endl;
        } else {
          std::cerr << "SVG export failed: " << svg_result.error << "\n";
        }
        
        std::ofstream js_outfile("canvas_output.js");
        if (js_outfile.is_open()) {
          js_outfile << js_code;
          js_outfile.close();
          std::cout << "\nJavaScript code written to canvas_output.js\n";
        }
        
        std::ofstream json_outfile("canvas_output.json");
        if (json_outfile.is_open()) {
          json_outfile << json_code;
          json_outfile.close();
          std::cout << "Canvas JSON export written to canvas_output.json\n";
        }
        
        if (svg_result.success) {
          std::ofstream svg_json_outfile("svg_output.json");
          if (svg_json_outfile.is_open()) {
            svg_json_outfile << exporter.svg_to_json(svg_result.elements);
            svg_json_outfile.close();
            std::cout << "SVG JSON export written to svg_output.json\n";
          }
        }
      } else {
        std::cerr << "Canvas export failed: " << result.error << "\n";
        return -5;
      }
    } else {
      std::cerr << "Failed to get first page\n";
      return -6;
    }
  }

  return 0;
}
