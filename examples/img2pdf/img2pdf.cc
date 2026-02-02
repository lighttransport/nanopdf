// SPDX-License-Identifier: Apache-2.0
// Copyright 2024 Light Transport Entertainment Inc.
//
// img2pdf - Convert images to PDF using nanopdf
//
// Usage:
//   img2pdf image1.jpg image2.png -o output.pdf
//   img2pdf *.jpg -o album.pdf -s a4 -m 36 -t "My Album"

#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#include "pdf-writer.hh"

struct Options {
  std::vector<std::string> input_files;
  std::string output_file;
  std::string page_size = "a4";  // a4, letter, fit
  std::string compression = "auto";  // auto, flate, ccitt
  double margin = 0;             // in points
  std::string title;
  std::string author;
  bool help = false;
};

void print_usage(const char* program) {
  std::cerr << "Usage: " << program << " [options] image1 [image2 ...]\n"
            << "\n"
            << "Convert images to PDF using nanopdf.\n"
            << "\n"
            << "Options:\n"
            << "  -o, --output <file>   Output PDF file (required)\n"
            << "  -s, --size <size>     Page size: a4, letter, fit (default: a4)\n"
            << "                        'fit' uses image dimensions\n"
            << "  -c, --compression <type>\n"
            << "                        Compression: auto, flate, ccitt (default: auto)\n"
            << "                        'auto' uses JPEG passthrough or FlateDecode\n"
            << "                        'ccitt' uses CCITT Group 4 (for B/W documents)\n"
            << "  -m, --margin <pts>    Margin in points (default: 0)\n"
            << "  -t, --title <text>    Document title\n"
            << "  -a, --author <text>   Document author\n"
            << "  -h, --help            Show this help message\n"
            << "\n"
            << "Examples:\n"
            << "  " << program << " photo.jpg -o photo.pdf\n"
            << "  " << program << " img1.jpg img2.png -o album.pdf -s a4 -m 36\n"
            << "  " << program << " scan.png -o scan.pdf -s fit\n"
            << "  " << program << " document.png -o doc.pdf -c ccitt\n"
            << std::endl;
}

bool parse_args(int argc, char* argv[], Options& opts) {
  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];

    if (arg == "-h" || arg == "--help") {
      opts.help = true;
      return true;
    } else if (arg == "-o" || arg == "--output") {
      if (i + 1 >= argc) {
        std::cerr << "Error: " << arg << " requires an argument\n";
        return false;
      }
      opts.output_file = argv[++i];
    } else if (arg == "-s" || arg == "--size") {
      if (i + 1 >= argc) {
        std::cerr << "Error: " << arg << " requires an argument\n";
        return false;
      }
      opts.page_size = argv[++i];
      // Validate
      if (opts.page_size != "a4" && opts.page_size != "letter" &&
          opts.page_size != "fit") {
        std::cerr << "Error: Invalid page size '" << opts.page_size
                  << "'. Use: a4, letter, or fit\n";
        return false;
      }
    } else if (arg == "-c" || arg == "--compression") {
      if (i + 1 >= argc) {
        std::cerr << "Error: " << arg << " requires an argument\n";
        return false;
      }
      opts.compression = argv[++i];
      if (opts.compression != "auto" && opts.compression != "flate" &&
          opts.compression != "ccitt") {
        std::cerr << "Error: Invalid compression '" << opts.compression
                  << "'. Use: auto, flate, or ccitt\n";
        return false;
      }
    } else if (arg == "-m" || arg == "--margin") {
      if (i + 1 >= argc) {
        std::cerr << "Error: " << arg << " requires an argument\n";
        return false;
      }
      try {
        opts.margin = std::stod(argv[++i]);
      } catch (...) {
        std::cerr << "Error: Invalid margin value\n";
        return false;
      }
    } else if (arg == "-t" || arg == "--title") {
      if (i + 1 >= argc) {
        std::cerr << "Error: " << arg << " requires an argument\n";
        return false;
      }
      opts.title = argv[++i];
    } else if (arg == "-a" || arg == "--author") {
      if (i + 1 >= argc) {
        std::cerr << "Error: " << arg << " requires an argument\n";
        return false;
      }
      opts.author = argv[++i];
    } else if (arg[0] == '-') {
      std::cerr << "Error: Unknown option '" << arg << "'\n";
      return false;
    } else {
      opts.input_files.push_back(arg);
    }
  }

  if (opts.help) {
    return true;
  }

  if (opts.input_files.empty()) {
    std::cerr << "Error: No input files specified\n";
    return false;
  }

  if (opts.output_file.empty()) {
    std::cerr << "Error: Output file not specified (use -o)\n";
    return false;
  }

  return true;
}

int main(int argc, char* argv[]) {
  Options opts;

  if (!parse_args(argc, argv, opts)) {
    std::cerr << "Use --help for usage information.\n";
    return 1;
  }

  if (opts.help) {
    print_usage(argv[0]);
    return 0;
  }

  nanopdf::PdfWriter writer;

  // Set metadata
  if (!opts.title.empty()) {
    writer.set_title(opts.title);
  }
  if (!opts.author.empty()) {
    writer.set_author(opts.author);
  }

  // Determine compression type
  nanopdf::ImageCompression compression = nanopdf::ImageCompression::Auto;
  if (opts.compression == "flate") {
    compression = nanopdf::ImageCompression::Flate;
  } else if (opts.compression == "ccitt") {
    compression = nanopdf::ImageCompression::CCITTFax;
  }

  // Process each input file
  int success_count = 0;
  for (const auto& input_file : opts.input_files) {
    std::cout << "Loading: " << input_file << std::endl;

    nanopdf::ImageData img = nanopdf::ImageData::FromFile(input_file);
    if (!img.valid()) {
      std::cerr << "Warning: Failed to load image: " << input_file << std::endl;
      continue;
    }

    std::cout << "  Size: " << img.width << "x" << img.height
              << ", Channels: " << img.channels;
    if (compression == nanopdf::ImageCompression::CCITTFax) {
      std::cout << " (CCITT Group 4)";
    } else if (img.is_jpeg && compression == nanopdf::ImageCompression::Auto) {
      std::cout << " (JPEG passthrough)";
    }
    std::cout << std::endl;

    // Add image with specified compression
    std::string img_name = writer.add_image(img, compression);

    // Add page based on page size option
    nanopdf::PageSize size;
    if (opts.page_size == "fit") {
      size = nanopdf::PageSize::FromPixels(
          img.width + static_cast<int>(2 * opts.margin),
          img.height + static_cast<int>(2 * opts.margin));
    } else if (opts.page_size == "letter") {
      size = nanopdf::PageSize::Letter();
    } else {
      size = nanopdf::PageSize::A4();
    }

    double page_w = size.width - 2 * opts.margin;
    double page_h = size.height - 2 * opts.margin;

    double img_w, img_h;
    if (opts.page_size == "fit") {
      img_w = static_cast<double>(img.width);
      img_h = static_cast<double>(img.height);
    } else {
      // Scale to fit
      double scale_x = page_w / img.width;
      double scale_y = page_h / img.height;
      double scale = std::min(scale_x, scale_y);
      img_w = img.width * scale;
      img_h = img.height * scale;
    }

    double x = opts.margin + (page_w - img_w) / 2;
    double y = opts.margin + (page_h - img_h) / 2;

    writer.add_page(size, [&](nanopdf::PageBuilder& builder) {
      builder.draw_image(img_name, x, y, img_w, img_h);
    });

    success_count++;
  }

  if (success_count == 0) {
    std::cerr << "Error: No images were successfully loaded\n";
    return 1;
  }

  // Write PDF
  std::cout << "Writing PDF: " << opts.output_file << std::endl;

  nanopdf::WriteResult result = writer.write_to_file(opts.output_file);
  if (!result.success) {
    std::cerr << "Error: Failed to write PDF: " << result.error << std::endl;
    return 1;
  }

  std::cout << "Successfully created PDF with " << success_count
            << " page(s)." << std::endl;

  return 0;
}
