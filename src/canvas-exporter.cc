#if defined(NANOPDF_USE_NANOSTL)
#include "nanocstring.h"
#include "nanostring.h"
#else
#include <cstring>
#include <sstream>
#include <algorithm>
#include <iomanip>
#endif

#include "canvas-exporter.hh"
#include "common-macros.inc"

namespace nanopdf {

namespace {

bool is_whitespace(char c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\0';
}

bool is_delimiter(char c) {
  return c == '(' || c == ')' || c == '<' || c == '>' || c == '[' || c == ']' ||
         c == '{' || c == '}' || c == '/' || c == '%';
}

std::vector<std::string> tokenize_content_stream(const std::vector<uint8_t>& data) {
  std::vector<std::string> tokens;
  std::string current_token;
  bool in_string = false;
  bool in_hex_string = false;
  
  for (size_t i = 0; i < data.size(); ++i) {
    char c = static_cast<char>(data[i]);
    
    if (in_string) {
      current_token += c;
      if (c == ')' && (current_token.length() == 1 || current_token[current_token.length()-2] != '\\')) {
        tokens.push_back(current_token);
        current_token.clear();
        in_string = false;
      }
    } else if (in_hex_string) {
      current_token += c;
      if (c == '>') {
        tokens.push_back(current_token);
        current_token.clear();
        in_hex_string = false;
      }
    } else if (c == '(') {
      if (!current_token.empty()) {
        tokens.push_back(current_token);
        current_token.clear();
      }
      current_token += c;
      in_string = true;
    } else if (c == '<') {
      if (!current_token.empty()) {
        tokens.push_back(current_token);
        current_token.clear();
      }
      current_token += c;
      in_hex_string = true;
    } else if (is_whitespace(c) || is_delimiter(c)) {
      if (!current_token.empty()) {
        tokens.push_back(current_token);
        current_token.clear();
      }
      if (is_delimiter(c)) {
        tokens.push_back(std::string(1, c));
      }
    } else {
      current_token += c;
    }
  }
  
  if (!current_token.empty()) {
    tokens.push_back(current_token);
  }
  
  return tokens;
}

}  // anonymous namespace

void CanvasExporter::reset_state() {
  canvas_commands_.clear();
  svg_elements_.clear();
  state_ = GraphicsState{};
}

CanvasExportResult CanvasExporter::export_page(const Pdf& pdf, const Page& page) {
  CanvasExportResult result;
  reset_state();
  
  if (page.media_box.size() >= 4) {
    page_width_ = page.media_box[2] - page.media_box[0];
    page_height_ = page.media_box[3] - page.media_box[1];
  } else {
    page_width_ = 612.0;  // Default letter size
    page_height_ = 792.0;
  }
  
  result.width = page_width_;
  result.height = page_height_;
  
  add_canvas_command("save");
  add_canvas_command("scale", {"1", double_to_string(page_height_ / page_height_)});
  add_canvas_command("translate", {"0", double_to_string(page_height_)});
  add_canvas_command("scale", {"1", "-1"});
  
  PageContent content = page.load_contents(pdf);
  if (!content.success) {
    result.error = "Failed to load page content: " + content.error;
    return result;
  }
  
  parse_content_stream(content.data);
  
  add_canvas_command("restore");
  
  result.commands = canvas_commands_;
  result.success = true;
  return result;
}

void CanvasExporter::parse_content_stream(const std::vector<uint8_t>& content_data) {
  std::vector<std::string> tokens = tokenize_content_stream(content_data);
  std::vector<std::string> operand_stack;
  
  for (const std::string& token : tokens) {
    if (token.empty()) continue;
    
    bool is_operator = false;
    
    if (token == "BT" || token == "ET" || token == "Tj" || token == "TJ" || 
        token == "Td" || token == "TD" || token == "Tm" || token == "T*" ||
        token == "Tf" || token == "TL" || token == "Tc" || token == "Tw" ||
        token == "Tz" || token == "TL" || token == "Ts" || token == "Tr") {
      handle_text_command(token, operand_stack);
      is_operator = true;
    } else if (token == "m" || token == "l" || token == "c" || token == "v" || 
               token == "y" || token == "h" || token == "re" || token == "S" ||
               token == "s" || token == "f" || token == "F" || token == "f*" ||
               token == "B" || token == "B*" || token == "b" || token == "b*" ||
               token == "n" || token == "W" || token == "W*") {
      handle_path_command(token, operand_stack);
      is_operator = true;
    } else if (token == "q" || token == "Q" || token == "cm" || token == "w" ||
               token == "J" || token == "j" || token == "M" || token == "d" ||
               token == "ri" || token == "i" || token == "gs") {
      handle_graphics_state_command(token, operand_stack);
      is_operator = true;
    } else if (token == "CS" || token == "cs" || token == "SC" || token == "SCN" ||
               token == "sc" || token == "scn" || token == "G" || token == "g" ||
               token == "RG" || token == "rg" || token == "K" || token == "k") {
      handle_color_command(token, operand_stack);
      is_operator = true;
    }
    
    if (is_operator) {
      operand_stack.clear();
    } else {
      operand_stack.push_back(token);
    }
  }
}

void CanvasExporter::process_pdf_command(const std::string& operator_name, 
                                       const std::vector<std::string>& operands) {
}

void CanvasExporter::handle_text_command(const std::string& op, 
                                        const std::vector<std::string>& operands) {
  if (op == "BT") {
    state_.in_text_block = true;
    state_.text_x = 0.0;
    state_.text_y = 0.0;
  } else if (op == "ET") {
    state_.in_text_block = false;
  } else if (op == "Tf" && operands.size() >= 2) {
    state_.current_font = operands[0];
    try {
      state_.font_size = std::stod(operands[1]);
    } catch (...) {
      state_.font_size = 12.0;
    }
    add_canvas_command("font", {double_to_string(state_.font_size) + "px Arial"});
  } else if (op == "Td" && operands.size() >= 2) {
    try {
      double dx = std::stod(operands[0]);
      double dy = std::stod(operands[1]);
      state_.text_x += dx;
      state_.text_y += dy;
    } catch (...) {}
  } else if (op == "Tm" && operands.size() >= 6) {
    try {
      state_.text_x = std::stod(operands[4]);
      state_.text_y = std::stod(operands[5]);
    } catch (...) {}
  } else if (op == "Tj" && operands.size() >= 1) {
    std::string text = operands[0];
    if (text.length() >= 2 && text[0] == '(' && text[text.length()-1] == ')') {
      text = text.substr(1, text.length()-2);
    }
    
    double canvas_x = state_.text_x;
    double canvas_y = state_.text_y;
    transform_coordinates(canvas_x, canvas_y);
    
    add_canvas_command("fillText", {
      "\"" + text + "\"",
      double_to_string(canvas_x),
      double_to_string(canvas_y)
    });
  }
}

void CanvasExporter::handle_path_command(const std::string& op, 
                                       const std::vector<std::string>& operands) {
  if (op == "m" && operands.size() >= 2) {
    try {
      double x = std::stod(operands[0]);
      double y = std::stod(operands[1]);
      transform_coordinates(x, y);
      add_canvas_command("moveTo", {double_to_string(x), double_to_string(y)});
      state_.current_x = x;
      state_.current_y = y;
      if (!state_.in_path) {
        add_canvas_command("beginPath");
        state_.in_path = true;
      }
    } catch (...) {}
  } else if (op == "l" && operands.size() >= 2) {
    try {
      double x = std::stod(operands[0]);
      double y = std::stod(operands[1]);
      transform_coordinates(x, y);
      add_canvas_command("lineTo", {double_to_string(x), double_to_string(y)});
      state_.current_x = x;
      state_.current_y = y;
    } catch (...) {}
  } else if (op == "c" && operands.size() >= 6) {
    try {
      double x1 = std::stod(operands[0]);
      double y1 = std::stod(operands[1]);
      double x2 = std::stod(operands[2]);
      double y2 = std::stod(operands[3]);
      double x3 = std::stod(operands[4]);
      double y3 = std::stod(operands[5]);
      transform_coordinates(x1, y1);
      transform_coordinates(x2, y2);
      transform_coordinates(x3, y3);
      add_canvas_command("bezierCurveTo", {
        double_to_string(x1), double_to_string(y1),
        double_to_string(x2), double_to_string(y2),
        double_to_string(x3), double_to_string(y3)
      });
      state_.current_x = x3;
      state_.current_y = y3;
    } catch (...) {}
  } else if (op == "re" && operands.size() >= 4) {
    try {
      double x = std::stod(operands[0]);
      double y = std::stod(operands[1]);
      double w = std::stod(operands[2]);
      double h = std::stod(operands[3]);
      transform_coordinates(x, y);
      add_canvas_command("rect", {
        double_to_string(x), double_to_string(y),
        double_to_string(w), double_to_string(h)
      });
      if (!state_.in_path) {
        add_canvas_command("beginPath");
        state_.in_path = true;
      }
    } catch (...) {}
  } else if (op == "h") {
    add_canvas_command("closePath");
  } else if (op == "S") {
    add_canvas_command("stroke");
    state_.in_path = false;
  } else if (op == "s") {
    add_canvas_command("closePath");
    add_canvas_command("stroke");
    state_.in_path = false;
  } else if (op == "f" || op == "F") {
    add_canvas_command("fill");
    state_.in_path = false;
  } else if (op == "B") {
    add_canvas_command("fill");
    add_canvas_command("stroke");
    state_.in_path = false;
  } else if (op == "n") {
    state_.in_path = false;
  }
}

void CanvasExporter::handle_graphics_state_command(const std::string& op, 
                                                  const std::vector<std::string>& operands) {
  if (op == "q") {
    add_canvas_command("save");
  } else if (op == "Q") {
    add_canvas_command("restore");
  } else if (op == "w" && operands.size() >= 1) {
    add_canvas_command("lineWidth", {operands[0]});
  } else if (op == "cm" && operands.size() >= 6) {
    add_canvas_command("transform", {
      operands[0], operands[1], operands[2],
      operands[3], operands[4], operands[5]
    });
  }
}

void CanvasExporter::handle_color_command(const std::string& op, 
                                        const std::vector<std::string>& operands) {
  if (op == "rg" && operands.size() >= 3) {
    try {
      double r = std::stod(operands[0]);
      double g = std::stod(operands[1]);
      double b = std::stod(operands[2]);
      int ir = static_cast<int>(r * 255);
      int ig = static_cast<int>(g * 255);
      int ib = static_cast<int>(b * 255);
      std::stringstream ss;
      ss << "rgb(" << ir << "," << ig << "," << ib << ")";
      add_canvas_command("fillStyle", {"\"" + ss.str() + "\""});
    } catch (...) {}
  } else if (op == "RG" && operands.size() >= 3) {
    try {
      double r = std::stod(operands[0]);
      double g = std::stod(operands[1]);
      double b = std::stod(operands[2]);
      int ir = static_cast<int>(r * 255);
      int ig = static_cast<int>(g * 255);
      int ib = static_cast<int>(b * 255);
      std::stringstream ss;
      ss << "rgb(" << ir << "," << ig << "," << ib << ")";
      add_canvas_command("strokeStyle", {"\"" + ss.str() + "\""});
    } catch (...) {}
  } else if (op == "g" && operands.size() >= 1) {
    try {
      double gray = std::stod(operands[0]);
      int igray = static_cast<int>(gray * 255);
      std::stringstream ss;
      ss << "rgb(" << igray << "," << igray << "," << igray << ")";
      add_canvas_command("fillStyle", {"\"" + ss.str() + "\""});
    } catch (...) {}
  } else if (op == "G" && operands.size() >= 1) {
    try {
      double gray = std::stod(operands[0]);
      int igray = static_cast<int>(gray * 255);
      std::stringstream ss;
      ss << "rgb(" << igray << "," << igray << "," << igray << ")";
      add_canvas_command("strokeStyle", {"\"" + ss.str() + "\""});
    } catch (...) {}
  }
}

void CanvasExporter::add_canvas_command(const std::string& command) {
  canvas_commands_.emplace_back(command);
}

void CanvasExporter::add_canvas_command(const std::string& command, 
                                      const std::vector<std::string>& args) {
  canvas_commands_.emplace_back(command, args);
}

void CanvasExporter::transform_coordinates(double& x, double& y) const {
}

std::string CanvasExporter::double_to_string(double value) const {
  std::stringstream ss;
  ss << std::fixed << std::setprecision(2) << value;
  return ss.str();
}

std::string CanvasExporter::commands_to_javascript(const std::vector<CanvasCommand>& commands, 
                                                  const std::string& canvas_id) const {
  std::stringstream js;
  js << "const canvas = document.getElementById('" << canvas_id << "');\n";
  js << "const ctx = canvas.getContext('2d');\n";
  js << "canvas.width = " << page_width_ << ";\n";
  js << "canvas.height = " << page_height_ << ";\n\n";
  
  for (const CanvasCommand& cmd : commands) {
    if (cmd.args.empty()) {
      js << "ctx." << cmd.command << "();\n";
    } else if (cmd.command == "fillStyle" || cmd.command == "strokeStyle" || 
               cmd.command == "font") {
      js << "ctx." << cmd.command << " = " << cmd.args[0] << ";\n";
    } else if (cmd.command == "lineWidth") {
      js << "ctx." << cmd.command << " = " << cmd.args[0] << ";\n";
    } else {
      js << "ctx." << cmd.command << "(";
      for (size_t i = 0; i < cmd.args.size(); ++i) {
        if (i > 0) js << ", ";
        js << cmd.args[i];
      }
      js << ");\n";
    }
  }
  
  return js.str();
}

SvgExportResult CanvasExporter::export_page_to_svg(const Pdf& pdf, const Page& page) {
  SvgExportResult result;
  reset_state();
  
  if (page.media_box.size() >= 4) {
    page_width_ = page.media_box[2] - page.media_box[0];
    page_height_ = page.media_box[3] - page.media_box[1];
  } else {
    page_width_ = 612.0;
    page_height_ = 792.0;
  }
  
  result.width = page_width_;
  result.height = page_height_;
  
  PageContent content = page.load_contents(pdf);
  if (!content.success) {
    result.error = "Failed to load page content: " + content.error;
    return result;
  }
  
  parse_content_stream_for_svg(content.data);
  
  result.elements = svg_elements_;
  result.success = true;
  return result;
}

void CanvasExporter::parse_content_stream_for_svg(const std::vector<uint8_t>& content_data) {
  std::vector<std::string> tokens = tokenize_content_stream(content_data);
  std::vector<std::string> operand_stack;
  
  for (const std::string& token : tokens) {
    if (token.empty()) continue;
    
    bool is_operator = false;
    
    if (token == "BT" || token == "ET" || token == "Tj" || token == "TJ" || 
        token == "Td" || token == "TD" || token == "Tm" || token == "T*" ||
        token == "Tf" || token == "TL" || token == "Tc" || token == "Tw" ||
        token == "Tz" || token == "TL" || token == "Ts" || token == "Tr") {
      handle_text_command_svg(token, operand_stack);
      is_operator = true;
    } else if (token == "m" || token == "l" || token == "c" || token == "v" || 
               token == "y" || token == "h" || token == "re" || token == "S" ||
               token == "s" || token == "f" || token == "F" || token == "f*" ||
               token == "B" || token == "B*" || token == "b" || token == "b*" ||
               token == "n" || token == "W" || token == "W*") {
      handle_path_command_svg(token, operand_stack);
      is_operator = true;
    } else if (token == "q" || token == "Q" || token == "cm" || token == "w" ||
               token == "J" || token == "j" || token == "M" || token == "d" ||
               token == "ri" || token == "i" || token == "gs") {
      handle_graphics_state_command_svg(token, operand_stack);
      is_operator = true;
    } else if (token == "CS" || token == "cs" || token == "SC" || token == "SCN" ||
               token == "sc" || token == "scn" || token == "G" || token == "g" ||
               token == "RG" || token == "rg" || token == "K" || token == "k") {
      handle_color_command_svg(token, operand_stack);
      is_operator = true;
    }
    
    if (is_operator) {
      operand_stack.clear();
    } else {
      operand_stack.push_back(token);
    }
  }
}

void CanvasExporter::handle_text_command_svg(const std::string& op, 
                                            const std::vector<std::string>& operands) {
  if (op == "BT") {
    state_.in_text_block = true;
    state_.text_x = 0.0;
    state_.text_y = 0.0;
  } else if (op == "ET") {
    state_.in_text_block = false;
  } else if (op == "Tf" && operands.size() >= 2) {
    state_.current_font = operands[0];
    try {
      state_.font_size = std::stod(operands[1]);
    } catch (...) {
      state_.font_size = 12.0;
    }
  } else if (op == "Td" && operands.size() >= 2) {
    try {
      double dx = std::stod(operands[0]);
      double dy = std::stod(operands[1]);
      state_.text_x += dx;
      state_.text_y += dy;
    } catch (...) {}
  } else if (op == "Tm" && operands.size() >= 6) {
    try {
      state_.text_x = std::stod(operands[4]);
      state_.text_y = std::stod(operands[5]);
    } catch (...) {}
  } else if (op == "Tj" && operands.size() >= 1) {
    std::string text = operands[0];
    if (text.length() >= 2 && text[0] == '(' && text[text.length()-1] == ')') {
      text = text.substr(1, text.length()-2);
    }
    
    double svg_x = state_.text_x;
    double svg_y = state_.text_y;
    transform_coordinates_svg(svg_x, svg_y);
    
    std::map<std::string, std::string> attrs;
    attrs["x"] = double_to_string(svg_x);
    attrs["y"] = double_to_string(svg_y);
    attrs["font-size"] = double_to_string(state_.font_size);
    attrs["fill"] = state_.fill_color;
    
    add_svg_element("text", attrs, text);
  }
}

void CanvasExporter::handle_path_command_svg(const std::string& op, 
                                            const std::vector<std::string>& operands) {
  if (op == "m" && operands.size() >= 2) {
    try {
      double x = std::stod(operands[0]);
      double y = std::stod(operands[1]);
      transform_coordinates_svg(x, y);
      state_.path_data += "M " + double_to_string(x) + " " + double_to_string(y) + " ";
      state_.current_x = x;
      state_.current_y = y;
      state_.in_path = true;
    } catch (...) {}
  } else if (op == "l" && operands.size() >= 2) {
    try {
      double x = std::stod(operands[0]);
      double y = std::stod(operands[1]);
      transform_coordinates_svg(x, y);
      state_.path_data += "L " + double_to_string(x) + " " + double_to_string(y) + " ";
      state_.current_x = x;
      state_.current_y = y;
    } catch (...) {}
  } else if (op == "c" && operands.size() >= 6) {
    try {
      double x1 = std::stod(operands[0]);
      double y1 = std::stod(operands[1]);
      double x2 = std::stod(operands[2]);
      double y2 = std::stod(operands[3]);
      double x3 = std::stod(operands[4]);
      double y3 = std::stod(operands[5]);
      transform_coordinates_svg(x1, y1);
      transform_coordinates_svg(x2, y2);
      transform_coordinates_svg(x3, y3);
      state_.path_data += "C " + double_to_string(x1) + " " + double_to_string(y1) + " " +
                         double_to_string(x2) + " " + double_to_string(y2) + " " +
                         double_to_string(x3) + " " + double_to_string(y3) + " ";
      state_.current_x = x3;
      state_.current_y = y3;
    } catch (...) {}
  } else if (op == "re" && operands.size() >= 4) {
    try {
      double x = std::stod(operands[0]);
      double y = std::stod(operands[1]);
      double w = std::stod(operands[2]);
      double h = std::stod(operands[3]);
      transform_coordinates_svg(x, y);
      
      std::map<std::string, std::string> attrs;
      attrs["x"] = double_to_string(x);
      attrs["y"] = double_to_string(y);
      attrs["width"] = double_to_string(w);
      attrs["height"] = double_to_string(h);
      attrs["fill"] = "none";
      attrs["stroke"] = state_.stroke_color;
      attrs["stroke-width"] = double_to_string(state_.stroke_width);
      
      add_svg_element("rect", attrs);
      state_.in_path = false;
    } catch (...) {}
  } else if (op == "h") {
    state_.path_data += "Z ";
  } else if (op == "S" || op == "s") {
    if (state_.in_path && !state_.path_data.empty()) {
      std::map<std::string, std::string> attrs;
      attrs["d"] = state_.path_data;
      attrs["fill"] = "none";
      attrs["stroke"] = state_.stroke_color;
      attrs["stroke-width"] = double_to_string(state_.stroke_width);
      
      add_svg_element("path", attrs);
      state_.path_data.clear();
      state_.in_path = false;
    }
  } else if (op == "f" || op == "F") {
    if (state_.in_path && !state_.path_data.empty()) {
      std::map<std::string, std::string> attrs;
      attrs["d"] = state_.path_data;
      attrs["fill"] = state_.fill_color;
      attrs["stroke"] = "none";
      
      add_svg_element("path", attrs);
      state_.path_data.clear();
      state_.in_path = false;
    }
  } else if (op == "B" || op == "b") {
    if (state_.in_path && !state_.path_data.empty()) {
      std::map<std::string, std::string> attrs;
      attrs["d"] = state_.path_data;
      attrs["fill"] = state_.fill_color;
      attrs["stroke"] = state_.stroke_color;
      attrs["stroke-width"] = double_to_string(state_.stroke_width);
      
      add_svg_element("path", attrs);
      state_.path_data.clear();
      state_.in_path = false;
    }
  } else if (op == "n") {
    state_.path_data.clear();
    state_.in_path = false;
  }
}

void CanvasExporter::handle_graphics_state_command_svg(const std::string& op, 
                                                      const std::vector<std::string>& operands) {
  if (op == "w" && operands.size() >= 1) {
    try {
      state_.stroke_width = std::stod(operands[0]);
    } catch (...) {}
  }
}

void CanvasExporter::handle_color_command_svg(const std::string& op, 
                                             const std::vector<std::string>& operands) {
  if (op == "rg" && operands.size() >= 3) {
    try {
      double r = std::stod(operands[0]);
      double g = std::stod(operands[1]);
      double b = std::stod(operands[2]);
      int ir = static_cast<int>(r * 255);
      int ig = static_cast<int>(g * 255);
      int ib = static_cast<int>(b * 255);
      std::stringstream ss;
      ss << "#" << std::hex << std::setw(2) << std::setfill('0') << ir
         << std::setw(2) << std::setfill('0') << ig
         << std::setw(2) << std::setfill('0') << ib;
      state_.fill_color = ss.str();
    } catch (...) {}
  } else if (op == "RG" && operands.size() >= 3) {
    try {
      double r = std::stod(operands[0]);
      double g = std::stod(operands[1]);
      double b = std::stod(operands[2]);
      int ir = static_cast<int>(r * 255);
      int ig = static_cast<int>(g * 255);
      int ib = static_cast<int>(b * 255);
      std::stringstream ss;
      ss << "#" << std::hex << std::setw(2) << std::setfill('0') << ir
         << std::setw(2) << std::setfill('0') << ig
         << std::setw(2) << std::setfill('0') << ib;
      state_.stroke_color = ss.str();
    } catch (...) {}
  } else if (op == "g" && operands.size() >= 1) {
    try {
      double gray = std::stod(operands[0]);
      int igray = static_cast<int>(gray * 255);
      std::stringstream ss;
      ss << "#" << std::hex << std::setw(2) << std::setfill('0') << igray
         << std::setw(2) << std::setfill('0') << igray
         << std::setw(2) << std::setfill('0') << igray;
      state_.fill_color = ss.str();
    } catch (...) {}
  } else if (op == "G" && operands.size() >= 1) {
    try {
      double gray = std::stod(operands[0]);
      int igray = static_cast<int>(gray * 255);
      std::stringstream ss;
      ss << "#" << std::hex << std::setw(2) << std::setfill('0') << igray
         << std::setw(2) << std::setfill('0') << igray
         << std::setw(2) << std::setfill('0') << igray;
      state_.stroke_color = ss.str();
    } catch (...) {}
  }
}

void CanvasExporter::add_svg_element(const std::string& element_type, 
                                    const std::map<std::string, std::string>& attributes) {
  svg_elements_.emplace_back(element_type, attributes);
}

void CanvasExporter::add_svg_element(const std::string& element_type, 
                                    const std::map<std::string, std::string>& attributes, 
                                    const std::string& text_content) {
  svg_elements_.emplace_back(element_type, attributes, text_content);
}

void CanvasExporter::transform_coordinates_svg(double& x, double& y) const {
  y = page_height_ - y;
}

std::string CanvasExporter::escape_json_string(const std::string& str) const {
  std::string escaped;
  escaped.reserve(str.length() + 10);
  
  for (char c : str) {
    switch (c) {
      case '"':
        escaped += "\\\"";
        break;
      case '\\':
        escaped += "\\\\";
        break;
      case '\b':
        escaped += "\\b";
        break;
      case '\f':
        escaped += "\\f";
        break;
      case '\n':
        escaped += "\\n";
        break;
      case '\r':
        escaped += "\\r";
        break;
      case '\t':
        escaped += "\\t";
        break;
      default:
        if (c >= 0 && c < 32) {
          std::stringstream ss;
          ss << "\\u" << std::hex << std::setw(4) << std::setfill('0') << static_cast<int>(c);
          escaped += ss.str();
        } else {
          escaped += c;
        }
        break;
    }
  }
  
  return escaped;
}

std::string CanvasExporter::commands_to_json(const std::vector<CanvasCommand>& commands) const {
  std::stringstream json;
  
  json << "{\n";
  json << "  \"canvas\": {\n";
  json << "    \"width\": " << page_width_ << ",\n";
  json << "    \"height\": " << page_height_ << "\n";
  json << "  },\n";
  json << "  \"commands\": [\n";
  
  for (size_t i = 0; i < commands.size(); ++i) {
    const CanvasCommand& cmd = commands[i];
    
    json << "    {\n";
    json << "      \"type\": \"" << escape_json_string(cmd.command) << "\"";
    
    if (!cmd.args.empty()) {
      json << ",\n      \"args\": [";
      for (size_t j = 0; j < cmd.args.size(); ++j) {
        const std::string& arg = cmd.args[j];
        
        if (arg.front() == '"' && arg.back() == '"') {
          json << "\"" << escape_json_string(arg.substr(1, arg.length() - 2)) << "\"";
        } else {
          try {
            std::stod(arg);
            json << arg;
          } catch (...) {
            json << "\"" << escape_json_string(arg) << "\"";
          }
        }
        
        if (j < cmd.args.size() - 1) {
          json << ", ";
        }
      }
      json << "]";
    }
    
    json << "\n    }";
    
    if (i < commands.size() - 1) {
      json << ",";
    }
    json << "\n";
  }
  
  json << "  ]\n";
  json << "}\n";
  
  return json.str();
}

std::string CanvasExporter::svg_to_json(const std::vector<SvgElement>& elements) const {
  std::stringstream json;
  
  json << "{\n";
  json << "  \"svg\": {\n";
  json << "    \"width\": " << page_width_ << ",\n";
  json << "    \"height\": " << page_height_ << ",\n";
  json << "    \"viewBox\": \"0 0 " << page_width_ << " " << page_height_ << "\"\n";
  json << "  },\n";
  json << "  \"elements\": [\n";
  
  for (size_t i = 0; i < elements.size(); ++i) {
    const SvgElement& elem = elements[i];
    
    json << "    {\n";
    json << "      \"type\": \"" << escape_json_string(elem.element_type) << "\"";
    
    if (!elem.attributes.empty()) {
      json << ",\n      \"attributes\": {\n";
      size_t attr_count = 0;
      for (const auto& attr : elem.attributes) {
        json << "        \"" << escape_json_string(attr.first) << "\": ";
        
        try {
          std::stod(attr.second);
          json << "\"" << escape_json_string(attr.second) << "\"";
        } catch (...) {
          json << "\"" << escape_json_string(attr.second) << "\"";
        }
        
        if (++attr_count < elem.attributes.size()) {
          json << ",";
        }
        json << "\n";
      }
      json << "      }";
    }
    
    if (!elem.text_content.empty()) {
      json << ",\n      \"text\": \"" << escape_json_string(elem.text_content) << "\"";
    }
    
    json << "\n    }";
    
    if (i < elements.size() - 1) {
      json << ",";
    }
    json << "\n";
  }
  
  json << "  ]\n";
  json << "}\n";
  
  return json.str();
}

}  // namespace nanopdf