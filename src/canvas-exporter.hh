// SPDX-License-Identifier: Apache 2.0
// Copyright 2024 - Present, Light Transport Entertainment Inc.

#pragma once

#if defined(NANOPDF_USE_NANOSTL)
#include "nanocstring.h"
#include "nanostring.h"
using namespace nanostl;
#else
#include <string>
#include <vector>
#include <sstream>
#endif

#include "nanopdf.hh"

namespace nanopdf {

struct CanvasCommand {
  std::string command;
  std::vector<std::string> args;
  
  CanvasCommand(const std::string& cmd) : command(cmd) {}
  CanvasCommand(const std::string& cmd, const std::vector<std::string>& arguments) 
    : command(cmd), args(arguments) {}
};

struct SvgElement {
  std::string element_type;
  std::map<std::string, std::string> attributes;
  std::string text_content;
  
  SvgElement(const std::string& type) : element_type(type) {}
  SvgElement(const std::string& type, const std::map<std::string, std::string>& attrs) 
    : element_type(type), attributes(attrs) {}
  SvgElement(const std::string& type, const std::map<std::string, std::string>& attrs, const std::string& content)
    : element_type(type), attributes(attrs), text_content(content) {}
};

struct CanvasExportResult {
  std::vector<CanvasCommand> commands;
  bool success{false};
  std::string error;
  double width{0.0};
  double height{0.0};
};

struct SvgExportResult {
  std::vector<SvgElement> elements;
  bool success{false};
  std::string error;
  double width{0.0};
  double height{0.0};
};

class CanvasExporter {
public:
  CanvasExporter() = default;
  ~CanvasExporter() = default;

  CanvasExportResult export_page(const Pdf& pdf, const Page& page);
  SvgExportResult export_page_to_svg(const Pdf& pdf, const Page& page);
  
  std::string commands_to_javascript(const std::vector<CanvasCommand>& commands, 
                                   const std::string& canvas_id = "canvas") const;
  
  std::string commands_to_json(const std::vector<CanvasCommand>& commands) const;
  std::string svg_to_json(const std::vector<SvgElement>& elements) const;

private:
  void reset_state();
  void parse_content_stream(const std::vector<uint8_t>& content_data);
  void parse_content_stream_for_svg(const std::vector<uint8_t>& content_data);
  void process_pdf_command(const std::string& operator_name, 
                         const std::vector<std::string>& operands);
  
  void handle_text_command(const std::string& op, const std::vector<std::string>& operands);
  void handle_path_command(const std::string& op, const std::vector<std::string>& operands);
  void handle_graphics_state_command(const std::string& op, const std::vector<std::string>& operands);
  void handle_color_command(const std::string& op, const std::vector<std::string>& operands);
  
  void handle_text_command_svg(const std::string& op, const std::vector<std::string>& operands);
  void handle_path_command_svg(const std::string& op, const std::vector<std::string>& operands);
  void handle_graphics_state_command_svg(const std::string& op, const std::vector<std::string>& operands);
  void handle_color_command_svg(const std::string& op, const std::vector<std::string>& operands);
  
  void add_canvas_command(const std::string& command);
  void add_canvas_command(const std::string& command, const std::vector<std::string>& args);
  void add_svg_element(const std::string& element_type, const std::map<std::string, std::string>& attributes = {});
  void add_svg_element(const std::string& element_type, const std::map<std::string, std::string>& attributes, const std::string& text_content);
  
  void transform_coordinates(double& x, double& y) const;
  void transform_coordinates_svg(double& x, double& y) const;
  std::string double_to_string(double value) const;
  std::string escape_json_string(const std::string& str) const;

  std::vector<CanvasCommand> canvas_commands_;
  std::vector<SvgElement> svg_elements_;
  double page_width_{0.0};
  double page_height_{0.0};
  
  struct GraphicsState {
    double current_x{0.0};
    double current_y{0.0};
    double text_x{0.0};
    double text_y{0.0};
    std::string current_font;
    double font_size{12.0};
    std::string fill_color{"#000000"};
    std::string stroke_color{"#000000"};
    double stroke_width{1.0};
    bool in_text_block{false};
    bool in_path{false};
    std::string path_data;
  } state_;
};

}  // namespace nanopdf