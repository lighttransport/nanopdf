#if defined(NANOPDF_USE_NANOSTL)
#include "nanocstring.h"
#include "nanostring.h"
#else
#include <cstring>
#include <sstream>
#include <algorithm>
#include <iterator>
#include <iomanip>
#include <cctype>
#include <cmath>
#include <limits>
#include <memory>
#include <utility>
#endif

#include "canvas-exporter.hh"
#include "common-macros.inc"
#include "stb_image_write.h"

namespace nanopdf {

namespace {

bool is_whitespace(char c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\0';
}

bool is_delimiter(char c) {
  return c == '(' || c == ')' || c == '<' || c == '>' || c == '[' || c == ']' ||
         c == '{' || c == '}' || c == '/' || c == '%';
}

struct TokenizedStream {
  std::vector<std::string> tokens;
  std::vector<std::vector<uint8_t>> inline_images;
};

std::map<std::string, ExtendedGraphicsState> parse_extgstate_resources(const Pdf& pdf,
                                                                       const Dictionary& resources) {
  std::map<std::string, ExtendedGraphicsState> result;
  auto it = resources.find("ExtGState");
  if (it == resources.end()) {
    return result;
  }

  const Value& ext_value = it->second;
  Dictionary ext_dict;
  if (ext_value.type == Value::DICTIONARY) {
    ext_dict = ext_value.dict;
  } else if (ext_value.type == Value::REFERENCE) {
    ResolvedObject resolved = resolve_reference(pdf, ext_value.ref_object_number,
                                                ext_value.ref_generation_number);
    if (resolved.success && resolved.value.type == Value::DICTIONARY) {
      ext_dict = resolved.value.dict;
    }
  }

  for (const auto& entry : ext_dict) {
    const Value& value = entry.second;
    Dictionary gs_dict;
    if (value.type == Value::DICTIONARY) {
      gs_dict = value.dict;
    } else if (value.type == Value::REFERENCE) {
      ResolvedObject resolved = resolve_reference(pdf, value.ref_object_number,
                                                  value.ref_generation_number);
      if (resolved.success && resolved.value.type == Value::DICTIONARY) {
        gs_dict = resolved.value.dict;
      }
    }

    if (!gs_dict.empty()) {
      result[entry.first] = parse_ext_gstate(pdf, gs_dict);
    }
  }

  return result;
}

bool parse_pdf_bool(const std::string& token, bool* value) {
  if (!value) return false;
  if (token.empty()) return false;
  std::string lower = token;
  std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  if (lower == "true") {
    *value = true;
    return true;
  }
  if (lower == "false") {
    *value = false;
    return true;
  }
  try {
    double numeric = std::stod(token);
    *value = (numeric != 0.0);
    return true;
  } catch (...) {
    return false;
  }
}

bool parse_inline_decode_parms(const std::vector<std::string>& tokens,
                               filters::DecodeParams* params) {
  if (!params || tokens.empty()) {
    return false;
  }

  size_t idx = 0;
  if (tokens[idx] == "<<") {
    ++idx;
  }

  while (idx < tokens.size()) {
    std::string key = tokens[idx++];
    if (key == ">>") {
      break;
    }
    if (!key.empty() && key.front() == '/') {
      key = key.substr(1);
    }
    if (idx >= tokens.size()) {
      break;
    }

    std::string value = tokens[idx++];
    if (value == "<<") {
      int depth = 1;
      while (idx < tokens.size() && depth > 0) {
        if (tokens[idx] == "<<") depth++;
        else if (tokens[idx] == ">>") depth--;
        ++idx;
      }
      continue;
    }
    if (value == "[") {
      while (idx < tokens.size() && tokens[idx] != "]") {
        ++idx;
      }
      if (idx < tokens.size()) {
        ++idx;
      }
      continue;
    }
    if (value == ">>") {
      break;
    }
    if (!value.empty() && value.front() == '/') {
      value = value.substr(1);
    }

    auto parse_int = [&](const std::string& str, int fallback) -> int {
      try {
        return static_cast<int>(std::stod(str));
      } catch (...) {
        return fallback;
      }
    };

    auto parse_bool_token = [&](const std::string& str, bool fallback) -> bool {
      bool parsed = fallback;
      if (parse_pdf_bool(str, &parsed)) {
        return parsed;
      }
      return fallback;
    };

    if (key == "Predictor") {
      params->predictor = parse_int(value, params->predictor);
    } else if (key == "Colors") {
      params->colors = parse_int(value, params->colors);
    } else if (key == "BitsPerComponent") {
      params->bits_per_component = parse_int(value, params->bits_per_component);
    } else if (key == "Columns") {
      params->columns = parse_int(value, params->columns);
    } else if (key == "EarlyChange") {
      params->early_change = parse_bool_token(value, params->early_change);
    }
  }

  return true;
}

TokenizedStream tokenize_content_stream(const std::vector<uint8_t>& data) {
  TokenizedStream result;
  std::string current_token;
  bool in_string = false;
  bool in_hex_string = false;
  bool in_inline_image = false;
  std::vector<uint8_t> inline_buffer;

  auto flush_token = [&]() {
    if (!current_token.empty()) {
      result.tokens.push_back(current_token);
      current_token.clear();
    }
  };

  for (size_t i = 0; i < data.size(); ++i) {
    char c = static_cast<char>(data[i]);

    if (in_inline_image) {
      if (c == 'E' && i + 1 < data.size() && static_cast<char>(data[i + 1]) == 'I' &&
          (i == 0 || is_whitespace(static_cast<char>(data[i - 1])) || static_cast<char>(data[i - 1]) == '\r' || static_cast<char>(data[i - 1]) == '\n')) {
        result.inline_images.push_back(std::move(inline_buffer));
        inline_buffer.clear();
        in_inline_image = false;
        i += 1;
        result.tokens.push_back("EI");
        continue;
      }
      inline_buffer.push_back(static_cast<uint8_t>(c));
      continue;
    }

    if (in_string) {
      current_token += c;
      if (c == ')' && (current_token.length() == 1 || current_token[current_token.length() - 2] != '\\')) {
        result.tokens.push_back(current_token);
        current_token.clear();
        in_string = false;
      }
    } else if (in_hex_string) {
      current_token += c;
      if (c == '>') {
        result.tokens.push_back(current_token);
        current_token.clear();
        in_hex_string = false;
      }
    } else if (c == '(') {
      flush_token();
      current_token += c;
      in_string = true;
    } else if (c == '<') {
      flush_token();
      current_token += c;
      in_hex_string = true;
    } else if (is_whitespace(c) || is_delimiter(c)) {
      flush_token();
      if (is_delimiter(c)) {
        std::string delimiter(1, c);
        if (delimiter == "I" && !result.tokens.empty() && result.tokens.back() == "ID") {
          in_inline_image = true;
          inline_buffer.clear();
        } else {
          result.tokens.push_back(delimiter);
        }
      }
    } else {
      current_token += c;
      if (current_token == "ID" && i + 1 < data.size() && is_whitespace(static_cast<char>(data[i + 1]))) {
        flush_token();
        i += 1;
        while (i < data.size() && is_whitespace(static_cast<char>(data[i]))) {
          ++i;
        }
        --i;
        in_inline_image = true;
        inline_buffer.clear();
      }
    }
  }

  flush_token();
  return result;
}

}  // anonymous namespace

void CanvasExporter::reset_state() {
  canvas_commands_.clear();
  svg_elements_.clear();
  image_xobjects_.clear();
  inline_images_.clear();
  pending_inline_mask_.reset();
  inline_image_cursor_ = 0;
  ext_gstates_.clear();
  current_pdf_ = nullptr;
  graphics_state_stack_.clear();
  state_ = GraphicsState{};
  canvas_mode_ = false;
  svg_mode_ = false;
}

CanvasExportResult CanvasExporter::export_page(const Pdf& pdf, const Page& page) {
  CanvasExportResult result;
  reset_state();
  current_pdf_ = &pdf;
  canvas_mode_ = true;
  
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
  
  image_xobjects_ = parse_xobject_resources(pdf, page.resources);
  ext_gstates_ = parse_extgstate_resources(pdf, page.resources);
  inline_image_cursor_ = 0;
  TokenizedStream stream = tokenize_content_stream(content.data);
  inline_images_ = stream.inline_images;
  parse_content_stream_from_tokens(stream.tokens);

  add_canvas_command("restore");

  result.commands = canvas_commands_;
  result.success = true;
  current_pdf_ = nullptr;
  pending_inline_mask_.reset();
  canvas_mode_ = false;
  return result;
}

void CanvasExporter::parse_content_stream_from_tokens(const std::vector<std::string>& tokens) {
  std::vector<std::string> operand_stack;

  for (size_t i = 0; i < tokens.size(); ++i) {
    const std::string& token = tokens[i];
    if (token.empty()) continue;

    if (token == "BI") {
      process_inline_image(tokens, i, false);
      continue;
    }
    if (token == "ID" || token == "EI") {
      continue;
    }

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
    graphics_state_stack_.push_back(state_);
  } else if (op == "Q") {
    add_canvas_command("restore");
    if (!graphics_state_stack_.empty()) {
      state_ = graphics_state_stack_.back();
      graphics_state_stack_.pop_back();
    }
  } else if (op == "w" && operands.size() >= 1) {
    try {
      state_.stroke_width = std::stod(operands[0]);
      update_canvas_line_width();
    } catch (...) {}
  } else if (op == "gs" && !operands.empty()) {
    std::string name;
    for (auto it = operands.rbegin(); it != operands.rend(); ++it) {
      if (it->empty() || *it == "/") {
        continue;
      }
      name = *it;
      break;
    }
    if (!name.empty() && name.front() == '/') {
      name.erase(0, 1);
    }
    if (!name.empty()) {
      apply_extended_graphics_state(name);
    }
  } else if (op == "cm" && operands.size() >= 6) {
    add_canvas_command("transform", {
      operands[0], operands[1], operands[2],
      operands[3], operands[4], operands[5]
    });
  } else if (op == "J" && operands.size() >= 1) {
    try {
      int cap = static_cast<int>(std::stod(operands[0]));
      switch (cap) {
        case 0:
          state_.line_cap = "butt";
          break;
        case 1:
          state_.line_cap = "round";
          break;
        case 2:
          state_.line_cap = "square";
          break;
        default:
          return;
      }
      update_canvas_line_cap();
    } catch (...) {}
  } else if (op == "j" && operands.size() >= 1) {
    try {
      int join = static_cast<int>(std::stod(operands[0]));
      switch (join) {
        case 0:
          state_.line_join = "miter";
          break;
        case 1:
          state_.line_join = "round";
          break;
        case 2:
          state_.line_join = "bevel";
          break;
        default:
          return;
      }
      update_canvas_line_join();
    } catch (...) {}
  } else if (op == "M" && operands.size() >= 1) {
    try {
      state_.miter_limit = std::stod(operands[0]);
      update_canvas_miter_limit();
    } catch (...) {}
  } else if (op == "d" && operands.size() >= 2) {
    std::vector<double> pattern;
    double phase = 0.0;
    parse_dash_pattern(operands, &pattern, &phase);
    state_.dash_pattern = pattern;
    state_.dash_phase = phase;
    update_canvas_line_dash();
  }
}

void CanvasExporter::handle_color_command(const std::string& op, 
                                        const std::vector<std::string>& operands) {
  if (op == "rg" && operands.size() >= 3) {
    try {
      double r = std::stod(operands[0]);
      double g = std::stod(operands[1]);
      double b = std::stod(operands[2]);
      state_.fill_color = rgb_to_hex(r, g, b);
      update_canvas_fill_style();
    } catch (...) {}
  } else if (op == "RG" && operands.size() >= 3) {
    try {
      double r = std::stod(operands[0]);
      double g = std::stod(operands[1]);
      double b = std::stod(operands[2]);
      state_.stroke_color = rgb_to_hex(r, g, b);
      update_canvas_stroke_style();
    } catch (...) {}
  } else if (op == "g" && operands.size() >= 1) {
    try {
      double gray = std::stod(operands[0]);
      state_.fill_color = rgb_to_hex(gray, gray, gray);
      update_canvas_fill_style();
    } catch (...) {}
  } else if (op == "G" && operands.size() >= 1) {
    try {
      double gray = std::stod(operands[0]);
      state_.stroke_color = rgb_to_hex(gray, gray, gray);
      update_canvas_stroke_style();
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
  ss << std::fixed << std::setprecision(3) << value;
  return ss.str();
}

std::string CanvasExporter::rgb_to_hex(double r, double g, double b) const {
  auto clamp_unit = [](double value) {
    if (value < 0.0) return 0.0;
    if (value > 1.0) return 1.0;
    return value;
  };

  auto to_byte = [](double value) {
    return static_cast<int>(std::round(value));
  };

  double cr = clamp_unit(r) * 255.0;
  double cg = clamp_unit(g) * 255.0;
  double cb = clamp_unit(b) * 255.0;

  std::stringstream ss;
  ss << "#" << std::hex << std::setw(2) << std::setfill('0') << to_byte(cr)
     << std::setw(2) << std::setfill('0') << to_byte(cg)
     << std::setw(2) << std::setfill('0') << to_byte(cb);
  return ss.str();
}

std::string CanvasExporter::canvas_color_string(const std::string& hex, double alpha) const {
  auto clamp_unit = [](double value) {
    if (value < 0.0) return 0.0;
    if (value > 1.0) return 1.0;
    return value;
  };

  double a = clamp_unit(alpha);
  int r = 0, g = 0, b = 0;
  if (hex.size() == 7 && hex[0] == '#') {
    try {
      r = std::stoi(hex.substr(1, 2), nullptr, 16);
      g = std::stoi(hex.substr(3, 2), nullptr, 16);
      b = std::stoi(hex.substr(5, 2), nullptr, 16);
    } catch (...) {
      r = g = b = 0;
    }
  }

  std::ostringstream ss;
  ss << '"';
  if (std::abs(a - 1.0) < 1e-6) {
    ss << "rgb(" << r << ',' << g << ',' << b << ')';
  } else {
    ss << std::fixed << std::setprecision(3);
    ss << "rgba(" << r << ',' << g << ',' << b << ',' << a << ')';
  }
  ss << '"';
  return ss.str();
}

void CanvasExporter::update_canvas_fill_style() {
  if (!canvas_mode_) {
    return;
  }
  add_canvas_command("fillStyle", {canvas_color_string(state_.fill_color, state_.fill_alpha)});
}

void CanvasExporter::update_canvas_stroke_style() {
  if (!canvas_mode_) {
    return;
  }
  add_canvas_command("strokeStyle", {canvas_color_string(state_.stroke_color, state_.stroke_alpha)});
}

void CanvasExporter::update_canvas_line_width() {
  if (!canvas_mode_) {
    return;
  }
  add_canvas_command("lineWidth", {double_to_string(state_.stroke_width)});
}

void CanvasExporter::update_canvas_line_cap() {
  if (!canvas_mode_) {
    return;
  }
  if (state_.line_cap.empty()) {
    return;
  }
  add_canvas_command("lineCap", {"\"" + state_.line_cap + "\""});
}

void CanvasExporter::update_canvas_line_join() {
  if (!canvas_mode_) {
    return;
  }
  if (state_.line_join.empty()) {
    return;
  }
  add_canvas_command("lineJoin", {"\"" + state_.line_join + "\""});
}

void CanvasExporter::update_canvas_miter_limit() {
  if (!canvas_mode_) {
    return;
  }
  add_canvas_command("miterLimit", {double_to_string(state_.miter_limit)});
}

void CanvasExporter::update_canvas_line_dash() {
  if (!canvas_mode_) {
    return;
  }
  add_canvas_command("setLineDash", {dash_pattern_to_canvas_array(state_.dash_pattern)});
  add_canvas_command("lineDashOffset", {double_to_string(state_.dash_phase)});
}

void CanvasExporter::update_canvas_blend_mode() {
  if (!canvas_mode_) {
    return;
  }
  add_canvas_command("globalCompositeOperation", {"\"" + state_.blend_mode + "\""});
}

std::string CanvasExporter::blend_mode_to_canvas(BlendMode mode) const {
  switch (mode) {
    case BlendMode::Normal:
      return "source-over";
    case BlendMode::Multiply:
      return "multiply";
    case BlendMode::Screen:
      return "screen";
    case BlendMode::Overlay:
      return "overlay";
    case BlendMode::Darken:
      return "darken";
    case BlendMode::Lighten:
      return "lighten";
    case BlendMode::ColorDodge:
      return "color-dodge";
    case BlendMode::ColorBurn:
      return "color-burn";
    case BlendMode::HardLight:
      return "hard-light";
    case BlendMode::SoftLight:
      return "soft-light";
    case BlendMode::Difference:
      return "difference";
    case BlendMode::Exclusion:
      return "exclusion";
    case BlendMode::Hue:
      return "hue";
    case BlendMode::Saturation:
      return "saturation";
    case BlendMode::Color:
      return "color";
    case BlendMode::Luminosity:
      return "luminosity";
  }
  return "source-over";
}

std::string CanvasExporter::blend_mode_to_css(const std::string& canvas_mode) const {
  if (canvas_mode == "source-over") {
    return "";
  }
  return canvas_mode;
}

CanvasExporter::Matrix2D CanvasExporter::multiply_matrices(const Matrix2D& lhs,
                                                          const Matrix2D& rhs) const {
  Matrix2D result;
  result.a = lhs.a * rhs.a + lhs.c * rhs.b;
  result.b = lhs.b * rhs.a + lhs.d * rhs.b;
  result.c = lhs.a * rhs.c + lhs.c * rhs.d;
  result.d = lhs.b * rhs.c + lhs.d * rhs.d;
  result.e = lhs.a * rhs.e + lhs.c * rhs.f + lhs.e;
  result.f = lhs.b * rhs.e + lhs.d * rhs.f + lhs.f;
  return result;
}

void CanvasExporter::apply_matrix_to_point(const Matrix2D& matrix, double& x, double& y) const {
  double new_x = matrix.a * x + matrix.c * y + matrix.e;
  double new_y = matrix.b * x + matrix.d * y + matrix.f;
  x = new_x;
  y = new_y;
}

std::string CanvasExporter::dash_pattern_to_string(const std::vector<double>& pattern) const {
  if (pattern.empty()) {
    return "";
  }

  std::stringstream ss;
  for (size_t i = 0; i < pattern.size(); ++i) {
    if (i > 0) {
      ss << ' ';
    }
    ss << double_to_string(pattern[i]);
  }
  return ss.str();
}

std::string CanvasExporter::dash_pattern_to_canvas_array(const std::vector<double>& pattern) const {
  std::stringstream ss;
  ss << '[';
  for (size_t i = 0; i < pattern.size(); ++i) {
    if (i > 0) {
      ss << ", ";
    }
    ss << double_to_string(pattern[i]);
  }
  ss << ']';
  return ss.str();
}

void CanvasExporter::parse_dash_pattern(const std::vector<std::string>& operands,
                                        std::vector<double>* pattern,
                                        double* phase) const {
  if (!pattern || !phase) {
    return;
  }

  pattern->clear();
  *phase = 0.0;

  bool in_array = false;
  for (size_t i = 0; i < operands.size(); ++i) {
    const std::string& token = operands[i];
    if (token == "[") {
      in_array = true;
      continue;
    }
    if (token == "]") {
      in_array = false;
      continue;
    }

    try {
      double value = std::stod(token);
      if (in_array) {
        pattern->push_back(value);
      } else {
        *phase = value;
        break;
      }
    } catch (...) {}
  }
}

void CanvasExporter::emit_svg_image(const std::string& name, const ImageXObject& image) {
  (void)name;

  if (image.width <= 0 || image.height <= 0) {
    return;
  }

  if (image.image_mask) {
    return;
  }

  std::vector<uint8_t> encoded_bytes;
  std::string mime_type;

  auto encode_png = [&](const std::vector<uint8_t>& buffer, int components) -> bool {
    if (buffer.empty()) {
      return false;
    }
    int stride = image.width * components;
    if (static_cast<int>(buffer.size()) < stride * image.height) {
      return false;
    }
    encoded_bytes.clear();
    auto write_cb = [](void* context, void* data, int size) {
      auto* out = static_cast<std::vector<uint8_t>*>(context);
      const uint8_t* bytes = static_cast<const uint8_t*>(data);
      out->insert(out->end(), bytes, bytes + size);
    };
    if (stbi_write_png_to_func(write_cb, &encoded_bytes,
                               image.width, image.height,
                               components, buffer.data(), stride) == 0) {
      encoded_bytes.clear();
      return false;
    }
    mime_type = "image/png";
    return !encoded_bytes.empty();
  };

  bool has_value_mask = image.soft_mask.type != Value::UNDEFINED &&
                        image.soft_mask.type != Value::NULL_OBJ;
  bool has_state_mask = state_.soft_mask_type != SoftMaskType::None &&
                        state_.soft_mask_value.type != Value::UNDEFINED &&
                        state_.soft_mask_value.type != Value::NULL_OBJ;

  bool has_mask = static_cast<bool>(pending_inline_mask_) || has_value_mask || has_state_mask;

  std::vector<uint8_t> prepared;
  int components = 0;
  bool direct_copy = !has_mask && (image.filter == "DCTDecode" || image.filter == "JPXDecode") &&
                     !image.raw_data.empty();

  if (direct_copy) {
    mime_type = (image.filter == "DCTDecode") ? "image/jpeg" : "image/jp2";
    encoded_bytes = image.raw_data;
  } else {
    if (!prepare_image_pixels(image, prepared, components)) {
      pending_inline_mask_.reset();
      return;
    }

    bool mask_applied = false;
    if (pending_inline_mask_) {
      mask_applied = combine_with_pending_inline_mask(image, prepared, components);
    }

    if (!mask_applied && has_value_mask) {
      ImageXObject mask_image;
      if (load_soft_mask_from_value(image.soft_mask, &mask_image)) {
        if (apply_soft_mask_to_pixels(image, mask_image, prepared, components, SoftMaskType::Alpha)) {
          mask_applied = true;
        }
      }
    }

    if (!mask_applied && has_state_mask) {
      ImageXObject mask_image;
      if (load_soft_mask_from_value(state_.soft_mask_value, &mask_image)) {
        if (apply_soft_mask_to_pixels(image, mask_image, prepared, components, state_.soft_mask_type)) {
          mask_applied = true;
        }
      }
    }

    if (!mask_applied && !has_mask && (image.filter == "DCTDecode" || image.filter == "JPXDecode") &&
        !image.raw_data.empty()) {
      mime_type = (image.filter == "DCTDecode") ? "image/jpeg" : "image/jp2";
      encoded_bytes = image.raw_data;
    } else {
      if (!encode_png(prepared, components)) {
        pending_inline_mask_.reset();
        return;
      }
    }
  }

  pending_inline_mask_.reset();

  if (encoded_bytes.empty()) {
    if (!direct_copy) {
      if (!encode_png(prepared, components)) {
        return;
      }
    } else {
      return;
    }
  }

  if (mime_type.empty()) {
    mime_type = "image/png";
  }

  std::string encoded = base64_encode(encoded_bytes);
  if (encoded.empty()) {
    return;
  }

  std::string data_url = "data:" + mime_type + ";base64," + encoded;

  double x0 = 0.0;
  double y0 = 0.0;
  double x1 = 1.0;
  double y1 = 0.0;
  double x2 = 0.0;
  double y2 = 1.0;

  transform_coordinates_svg(x0, y0);
  transform_coordinates_svg(x1, y1);
  transform_coordinates_svg(x2, y2);

  double a = x1 - x0;
  double b = y1 - y0;
  double c = x2 - x0;
  double d = y2 - y0;
  double e = x0;
  double f = y0;

  queue_canvas_image(a, b, c, d, e, f, data_url);

  std::map<std::string, std::string> attrs;
  attrs["xlink:href"] = data_url;
  attrs["width"] = double_to_string(1.0);
  attrs["height"] = double_to_string(1.0);

  std::stringstream transform;
  transform << "matrix(" << double_to_string(a) << " " << double_to_string(b) << " "
            << double_to_string(c) << " " << double_to_string(d) << " "
            << double_to_string(e) << " " << double_to_string(f) << ")";
  attrs["transform"] = transform.str();
  attrs["preserveAspectRatio"] = "none";

  add_svg_element("image", attrs);
}

std::string CanvasExporter::base64_encode(const std::vector<uint8_t>& data) const {
  if (data.empty()) {
    return std::string();
  }

  static const char kTable[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

  std::string encoded;
  encoded.reserve(((data.size() + 2) / 3) * 4);

  size_t i = 0;
  while (i + 2 < data.size()) {
    uint32_t triple = (static_cast<uint32_t>(data[i]) << 16) |
                      (static_cast<uint32_t>(data[i + 1]) << 8) |
                      static_cast<uint32_t>(data[i + 2]);
    encoded.push_back(kTable[(triple >> 18) & 0x3F]);
    encoded.push_back(kTable[(triple >> 12) & 0x3F]);
    encoded.push_back(kTable[(triple >> 6) & 0x3F]);
    encoded.push_back(kTable[triple & 0x3F]);
    i += 3;
  }

  size_t remaining = data.size() - i;
  if (remaining == 1) {
    uint32_t triple = static_cast<uint32_t>(data[i]) << 16;
    encoded.push_back(kTable[(triple >> 18) & 0x3F]);
    encoded.push_back(kTable[(triple >> 12) & 0x3F]);
    encoded.push_back('=');
    encoded.push_back('=');
  } else if (remaining == 2) {
    uint32_t triple = (static_cast<uint32_t>(data[i]) << 16) |
                      (static_cast<uint32_t>(data[i + 1]) << 8);
    encoded.push_back(kTable[(triple >> 18) & 0x3F]);
    encoded.push_back(kTable[(triple >> 12) & 0x3F]);
    encoded.push_back(kTable[(triple >> 6) & 0x3F]);
    encoded.push_back('=');
  }

  return encoded;
}

bool CanvasExporter::prepare_image_pixels(const ImageXObject& image,
                                          std::vector<uint8_t>& out,
                                          int& components) const {
  out.clear();
  components = 0;

  if (image.width <= 0 || image.height <= 0 || image.image_mask) {
    return false;
  }

  switch (image.color_space.type) {
    case ColorSpaceType::DeviceGray:
    case ColorSpaceType::CalGray: {
      if (image.bits_per_component == 8) {
        size_t expected = static_cast<size_t>(image.width) * image.height;
        if (image.data.size() < expected) {
          return false;
        }
      out.assign(image.data.begin(), image.data.begin() + expected);
      components = 1;
      return apply_decode_to_buffer(image, out, components);
    }
      if (image.bits_per_component == 1 || image.bits_per_component == 2 ||
          image.bits_per_component == 4) {
        if (!expand_low_bpc_gray(image, out)) {
          return false;
        }
        components = 1;
        return apply_decode_to_buffer(image, out, components);
      }
      return false;
    }
    case ColorSpaceType::DeviceRGB:
    case ColorSpaceType::CalRGB: {
      if (image.bits_per_component != 8) {
        return false;
      }
      size_t expected = static_cast<size_t>(image.width) * image.height * 3;
      if (image.data.size() < expected) {
        return false;
      }
      out.assign(image.data.begin(), image.data.begin() + expected);
      components = 3;
      return apply_decode_to_buffer(image, out, components);
    }
    case ColorSpaceType::DeviceCMYK: {
      if (image.bits_per_component != 8) {
        return false;
      }
      if (!convert_cmyk_to_rgb(image, out)) {
        return false;
      }
      components = 3;
      return true;
    }
    case ColorSpaceType::Indexed: {
      return expand_indexed_pixels(image, out, components);
    }
    default:
      break;
  }

  return false;
}

bool CanvasExporter::expand_low_bpc_gray(const ImageXObject& image,
                                         std::vector<uint8_t>& out) const {
  int bits = image.bits_per_component;
  if (!(bits == 1 || bits == 2 || bits == 4)) {
    return false;
  }

  size_t bytes_per_row = static_cast<size_t>((image.width * bits + 7) / 8);
  size_t expected = bytes_per_row * image.height;
  if (image.data.size() < expected) {
    return false;
  }

  out.resize(static_cast<size_t>(image.width) * image.height);
  const uint8_t* src = image.data.data();
  size_t src_index = 0;
  size_t dst_index = 0;
  int mask = (1 << bits) - 1;

  for (int y = 0; y < image.height; ++y) {
    int bit_offset = 0;
    uint8_t current_byte = 0;
    for (int x = 0; x < image.width; ++x) {
      if (bit_offset == 0) {
        if (src_index >= image.data.size()) {
          return false;
        }
        current_byte = src[src_index++];
      }
      int shift = 8 - bits - bit_offset;
      if (shift < 0) {
        shift = 0;
      }
      uint8_t value = static_cast<uint8_t>((current_byte >> shift) & mask);
      double scaled = static_cast<double>(value) / static_cast<double>(mask);
      out[dst_index++] = static_cast<uint8_t>(std::round(scaled * 255.0));
      bit_offset += bits;
      if (bit_offset >= 8) {
        bit_offset = 0;
      }
    }

    if (bit_offset != 0) {
      bit_offset = 0;
    }
  }

  return true;
}

bool CanvasExporter::expand_index_buffer(const std::vector<uint8_t>& data,
                                         int width,
                                         int height,
                                         int bits,
                                         std::vector<uint8_t>& indices) const {
  size_t bytes_per_row = static_cast<size_t>((width * bits + 7) / 8);
  size_t expected = bytes_per_row * height;
  if (data.size() < expected) {
    return false;
  }

  indices.resize(static_cast<size_t>(width) * height);
  size_t src_index = 0;
  size_t dst_index = 0;
  int mask = (1 << bits) - 1;

  for (int y = 0; y < height; ++y) {
    int bit_offset = 0;
    uint8_t current_byte = 0;
    for (int x = 0; x < width; ++x) {
      if (bit_offset == 0) {
        if (src_index >= data.size()) {
          return false;
        }
        current_byte = data[src_index++];
      }
      int shift = 8 - bits - bit_offset;
      if (shift < 0) {
        shift = 0;
      }
      uint8_t value = static_cast<uint8_t>((current_byte >> shift) & mask);
      indices[dst_index++] = value;
      bit_offset += bits;
      if (bit_offset >= 8) {
        bit_offset = 0;
      }
    }
    if (bit_offset != 0) {
      bit_offset = 0;
    }
  }

  return true;
}

bool CanvasExporter::apply_decode_to_buffer(const ImageXObject& image,
                                            std::vector<uint8_t>& buffer,
                                            int components) const {
  if (buffer.empty()) {
    return false;
  }

  size_t expected = static_cast<size_t>(components) * 2;
  if (image.decode.empty() || image.decode.size() < expected) {
    return true;
  }

  for (size_t i = 0; i < buffer.size(); i += components) {
    for (int c = 0; c < components && (i + c) < buffer.size(); ++c) {
      double normalized = static_cast<double>(buffer[i + c]) / 255.0;
      double mapped = apply_decode_component(image, c, normalized);
      buffer[i + c] = static_cast<uint8_t>(std::round(mapped * 255.0));
    }
  }

  return true;
}

bool CanvasExporter::expand_indexed_pixels(const ImageXObject& image,
                                           std::vector<uint8_t>& out,
                                           int& components) const {
  if (!image.color_space.base_color_space) {
    return false;
  }

  int bits = image.bits_per_component;
  if (bits <= 0 || bits > 8) {
    return false;
  }

  int base_components = 0;
  bool base_is_gray = false;
  switch (image.color_space.base_color_space->type) {
    case ColorSpaceType::DeviceRGB:
    case ColorSpaceType::CalRGB:
      base_components = 3;
      break;
    case ColorSpaceType::DeviceGray:
    case ColorSpaceType::CalGray:
      base_components = 1;
      base_is_gray = true;
      break;
    default:
      return false;
  }

  if (image.color_space.lookup_table.empty()) {
    return false;
  }

  std::vector<uint8_t> indices;
  if (bits == 8) {
    size_t expected = static_cast<size_t>(image.width) * image.height;
    if (image.data.size() < expected) {
      return false;
    }
    indices.assign(image.data.begin(), image.data.begin() + expected);
  } else {
    if (!expand_index_buffer(image.data, image.width, image.height, bits, indices)) {
      return false;
    }
  }

  size_t palette_stride = static_cast<size_t>(base_components);
  size_t palette_entries = image.color_space.lookup_table.size() / palette_stride;
  if (palette_entries == 0) {
    return false;
  }

  components = 3;
  out.resize(static_cast<size_t>(image.width) * image.height * components);
  size_t dst_index = 0;
  size_t max_index = image.color_space.hival;
  for (uint8_t idx : indices) {
    size_t clamped = std::min<size_t>(idx, std::min(max_index, static_cast<size_t>(palette_entries - 1)));
    size_t palette_index = clamped * palette_stride;
    if (palette_index + palette_stride > image.color_space.lookup_table.size()) {
      palette_index = (palette_entries - 1) * palette_stride;
    }
    if (base_is_gray) {
      double normalized = static_cast<double>(image.color_space.lookup_table[palette_index]) / 255.0;
      double mapped = apply_decode_component(image, 0, normalized);
      uint8_t gray = static_cast<uint8_t>(std::round(mapped * 255.0));
      out[dst_index++] = gray;
      out[dst_index++] = gray;
      out[dst_index++] = gray;
    } else {
      double nr = static_cast<double>(image.color_space.lookup_table[palette_index]) / 255.0;
      double ng = static_cast<double>(image.color_space.lookup_table[palette_index + 1]) / 255.0;
      double nb = static_cast<double>(image.color_space.lookup_table[palette_index + 2]) / 255.0;
      double mapped_r = apply_decode_component(image, 0, nr);
      double mapped_g = apply_decode_component(image, 1, ng);
      double mapped_b = apply_decode_component(image, 2, nb);
      out[dst_index++] = static_cast<uint8_t>(std::round(mapped_r * 255.0));
      out[dst_index++] = static_cast<uint8_t>(std::round(mapped_g * 255.0));
      out[dst_index++] = static_cast<uint8_t>(std::round(mapped_b * 255.0));
    }
  }

  return true;
}

double CanvasExporter::apply_decode_component(const ImageXObject& image,
                                              int component_index,
                                              double normalized_value) const {
  if (normalized_value < 0.0) normalized_value = 0.0;
  if (normalized_value > 1.0) normalized_value = 1.0;

  size_t expected = static_cast<size_t>(component_index + 1) * 2;
  if (image.decode.empty() || image.decode.size() < expected) {
    return normalized_value;
  }

  double d_min = image.decode[component_index * 2];
  double d_max = image.decode[component_index * 2 + 1];

  if (d_min > d_max) {
    std::swap(d_min, d_max);
  }

  if (d_min < 0.0) d_min = 0.0;
  if (d_min > 1.0) d_min = 1.0;
  if (d_max < 0.0) d_max = 0.0;
  if (d_max > 1.0) d_max = 1.0;

  double mapped = d_min + normalized_value * (d_max - d_min);
  if (mapped < 0.0) mapped = 0.0;
  if (mapped > 1.0) mapped = 1.0;
  return mapped;
}

bool CanvasExporter::prepare_mask_pixels(const ImageXObject& image,
                                         std::vector<uint8_t>& out) const {
  if (image.width <= 0 || image.height <= 0) {
    return false;
  }

  int bits = image.bits_per_component;
  size_t pixel_count = static_cast<size_t>(image.width) * image.height;

  auto convert_rgb_to_gray = [&](const std::vector<uint8_t>& rgb) {
    out.resize(pixel_count);
    for (size_t i = 0; i < pixel_count; ++i) {
      double r = static_cast<double>(rgb[i * 3 + 0]) / 255.0;
      double g = static_cast<double>(rgb[i * 3 + 1]) / 255.0;
      double b = static_cast<double>(rgb[i * 3 + 2]) / 255.0;
      double gray = 0.299 * r + 0.587 * g + 0.114 * b;
      if (gray < 0.0) gray = 0.0;
      if (gray > 1.0) gray = 1.0;
      out[i] = static_cast<uint8_t>(std::round(gray * 255.0));
    }
  };

  if (image.image_mask ||
      image.color_space.type == ColorSpaceType::DeviceGray ||
      image.color_space.type == ColorSpaceType::CalGray) {
    if (bits == 8) {
      size_t expected = pixel_count;
      if (image.data.size() < expected) {
        return false;
      }
      out.assign(image.data.begin(), image.data.begin() + expected);
    } else if (bits == 1 || bits == 2 || bits == 4) {
      if (!expand_low_bpc_gray(image, out)) {
        return false;
      }
    } else {
      return false;
    }

    for (size_t i = 0; i < out.size(); ++i) {
      double normalized = static_cast<double>(out[i]) / 255.0;
      double mapped = apply_decode_component(image, 0, normalized);
      out[i] = static_cast<uint8_t>(std::round(mapped * 255.0));
    }
    return true;
  }

  if (image.color_space.type == ColorSpaceType::DeviceRGB ||
      image.color_space.type == ColorSpaceType::CalRGB) {
    if (bits != 8) {
      return false;
    }
    size_t expected = pixel_count * 3;
    if (image.data.size() < expected) {
      return false;
    }

    std::vector<uint8_t> rgb(pixel_count * 3);
    for (size_t i = 0; i < pixel_count; ++i) {
      double r = apply_decode_component(image, 0, static_cast<double>(image.data[i * 3 + 0]) / 255.0);
      double g = apply_decode_component(image, 1, static_cast<double>(image.data[i * 3 + 1]) / 255.0);
      double b = apply_decode_component(image, 2, static_cast<double>(image.data[i * 3 + 2]) / 255.0);
      if (r < 0.0) r = 0.0;
      if (r > 1.0) r = 1.0;
      if (g < 0.0) g = 0.0;
      if (g > 1.0) g = 1.0;
      if (b < 0.0) b = 0.0;
      if (b > 1.0) b = 1.0;
      rgb[i * 3 + 0] = static_cast<uint8_t>(std::round(r * 255.0));
      rgb[i * 3 + 1] = static_cast<uint8_t>(std::round(g * 255.0));
      rgb[i * 3 + 2] = static_cast<uint8_t>(std::round(b * 255.0));
    }
    convert_rgb_to_gray(rgb);
    return true;
  }

  if (image.color_space.type == ColorSpaceType::Indexed) {
    std::vector<uint8_t> rgb;
    int components = 0;
    if (!expand_indexed_pixels(image, rgb, components)) {
      return false;
    }
    if (components != 3) {
      return false;
    }
    convert_rgb_to_gray(rgb);
    return true;
  }

  if (image.color_space.type == ColorSpaceType::DeviceCMYK) {
    std::vector<uint8_t> rgb;
    if (!convert_cmyk_to_rgb(image, rgb)) {
      return false;
    }
    if (rgb.size() < pixel_count * 3) {
      return false;
    }
    convert_rgb_to_gray(rgb);
    return true;
  }

  return false;
}

bool CanvasExporter::combine_with_pending_inline_mask(const ImageXObject& image,
                                                      std::vector<uint8_t>& buffer,
                                                      int& components) {
  if (!pending_inline_mask_) {
    return false;
  }

  ImageXObject mask = *pending_inline_mask_;
  pending_inline_mask_.reset();
  return apply_soft_mask_to_pixels(image, mask, buffer, components, SoftMaskType::Alpha);
}

bool CanvasExporter::prepare_soft_mask_object(const ImageXObject& mask_image,
                                              std::vector<uint8_t>& mask_pixels) const {
  if (mask_image.width <= 0 || mask_image.height <= 0) {
    return false;
  }

  if (!prepare_mask_pixels(mask_image, mask_pixels)) {
    return false;
  }

  return true;
}

bool CanvasExporter::prepare_image_by_id(const std::string& name,
                                         ImageXObject* out_image) const {
  std::string key = name;
  if (!key.empty() && key.front() == '/') {
    key.erase(0, 1);
  }
  auto it = image_xobjects_.find(key);
  if (it == image_xobjects_.end()) {
    return false;
  }
  if (!out_image) {
    return false;
  }
  *out_image = it->second;
  return true;
}

bool CanvasExporter::apply_soft_mask_to_pixels(const ImageXObject& image,
                                               const ImageXObject& mask,
                                               std::vector<uint8_t>& buffer,
                                               int& components,
                                               SoftMaskType mask_type) const {
  std::vector<uint8_t> mask_pixels;
  if (!prepare_soft_mask_object(mask, mask_pixels)) {
    return false;
  }

  size_t pixel_count = static_cast<size_t>(image.width) * image.height;
  if (mask_pixels.size() < pixel_count) {
    return false;
  }

  if (mask_type == SoftMaskType::Luminosity) {
    if (mask.color_space.type == ColorSpaceType::DeviceRGB ||
        mask.color_space.type == ColorSpaceType::CalRGB ||
        mask.color_space.type == ColorSpaceType::Indexed ||
        mask.color_space.type == ColorSpaceType::DeviceCMYK ||
        mask.color_space.type == ColorSpaceType::DeviceGray ||
        mask.color_space.type == ColorSpaceType::CalGray) {
      std::vector<uint8_t> gray;
      if (!prepare_mask_pixels(mask, gray)) {
        return false;
      }
      mask_pixels.swap(gray);
    }
  }

  if (mask_type == SoftMaskType::Alpha) {
    // mask_pixels already represents alpha values.
  }

  if (components == 1) {
    std::vector<uint8_t> rgb(pixel_count * 3);
    for (size_t i = 0; i < pixel_count; ++i) {
      uint8_t gray = buffer[i];
      rgb[i * 3 + 0] = gray;
      rgb[i * 3 + 1] = gray;
      rgb[i * 3 + 2] = gray;
    }
    buffer.swap(rgb);
    components = 3;
  }

  if (components != 3) {
    return false;
  }

  if (buffer.size() < pixel_count * 3) {
    return false;
  }

  std::vector<uint8_t> rgba(pixel_count * 4);
  for (size_t i = 0; i < pixel_count; ++i) {
    rgba[i * 4 + 0] = buffer[i * 3 + 0];
    rgba[i * 4 + 1] = buffer[i * 3 + 1];
    rgba[i * 4 + 2] = buffer[i * 3 + 2];
    rgba[i * 4 + 3] = mask_pixels[i];
  }

  buffer.swap(rgba);
  components = 4;
  return true;
}

bool CanvasExporter::load_soft_mask_from_value(const Value& value,
                                               ImageXObject* out_image) const {
  if (!current_pdf_ || !out_image) {
    return false;
  }

  Value resolved = value;
  if (value.type == Value::REFERENCE) {
    ResolvedObject resolved_obj = resolve_reference(*current_pdf_, value.ref_object_number,
                                                    value.ref_generation_number);
    if (!resolved_obj.success) {
      return false;
    }
    resolved = resolved_obj.value;
  }

  if (resolved.type == Value::STREAM) {
    *out_image = parse_image_xobject(*current_pdf_, resolved);
    return out_image->width > 0 && out_image->height > 0;
  }

  if (resolved.type == Value::NAME) {
    std::string mask_name = resolved.name;
    if (!mask_name.empty() && mask_name.front() == '/') {
      mask_name.erase(0, 1);
    }
    return prepare_image_by_id(mask_name, out_image);
  }

  if (resolved.type == Value::DICTIONARY) {
    auto subtype_it = resolved.dict.find("Subtype");
    if (subtype_it != resolved.dict.end() && subtype_it->second.type == Value::NAME &&
        subtype_it->second.name == "Image") {
      Value stream_value = resolved;
      stream_value.type = Value::STREAM;
      stream_value.stream.dict = resolved.dict;
      // Without actual stream data we cannot decode; return false.
    }
  }

  return false;
}

bool CanvasExporter::process_inline_image(const std::vector<std::string>& tokens,
                                          size_t& index,
                                          bool emit_svg) {
  size_t dict_start = index + 1;
  size_t id_pos = dict_start;
  while (id_pos < tokens.size() && tokens[id_pos] != "ID") {
    ++id_pos;
  }
  if (id_pos >= tokens.size()) {
    index = tokens.size();
    return false;
  }

  std::map<std::string, std::vector<std::string>> dict_entries;
  size_t cursor = dict_start;
  while (cursor < id_pos) {
    std::string key = tokens[cursor++];
    if (key.empty()) continue;
    if (!key.empty() && key[0] == '/') {
      key = key.substr(1);
    }
    if (key.empty()) continue;

    std::vector<std::string> values;
    if (cursor < id_pos) {
      if (tokens[cursor] == "[") {
        ++cursor;
        while (cursor < id_pos && tokens[cursor] != "]") {
          values.push_back(tokens[cursor++]);
        }
        if (cursor < id_pos && tokens[cursor] == "]") {
          ++cursor;
        }
      } else if (tokens[cursor] == "<<") {
        int depth = 1;
        ++cursor;
        while (cursor < id_pos && depth > 0) {
          if (tokens[cursor] == "<<") depth++;
          else if (tokens[cursor] == ">>") depth--;
          values.push_back(tokens[cursor]);
          ++cursor;
        }
      } else {
        values.push_back(tokens[cursor++]);
      }
    }
    if (!values.empty()) {
      dict_entries[key] = values;
    }
  }

  size_t ei_pos = id_pos + 1;
  while (ei_pos < tokens.size() && tokens[ei_pos] != "EI") {
    ++ei_pos;
  }
  if (ei_pos >= tokens.size()) {
    index = tokens.size();
    return false;
  }

  if (inline_image_cursor_ >= inline_images_.size()) {
    index = ei_pos;
    return false;
  }

  const std::vector<uint8_t>& raw_data = inline_images_[inline_image_cursor_++];
  ImageXObject image;
  if (!build_inline_image(dict_entries, raw_data, &image)) {
    index = ei_pos;
    return false;
  }

  if (image.image_mask) {
    pending_inline_mask_.reset(new ImageXObject(std::move(image)));
    index = ei_pos;
    return true;
  }

  if (emit_svg) {
    emit_svg_image("inline" + std::to_string(inline_image_cursor_), image);
  }

  index = ei_pos;
  return true;
}

bool CanvasExporter::build_inline_image(const std::map<std::string, std::vector<std::string>>& dict,
                                        const std::vector<uint8_t>& data,
                                        ImageXObject* out_image) const {
  if (!out_image) {
    return false;
  }

  auto get_string_value = [&](const std::string& key) -> std::string {
    auto it = dict.find(key);
    if (it == dict.end() || it->second.empty()) {
      return std::string();
    }
    return it->second.front();
  };

  auto get_bool_value = [&](const std::string& key, bool fallback) -> bool {
    auto it = dict.find(key);
    if (it == dict.end() || it->second.empty()) {
      return fallback;
    }
    bool parsed = fallback;
    if (parse_pdf_bool(it->second.front(), &parsed)) {
      return parsed;
    }
    return fallback;
  };

  auto get_int_value = [&](const std::string& key, int fallback) -> int {
    auto it = dict.find(key);
    if (it == dict.end() || it->second.empty()) {
      return fallback;
    }
    try {
      return static_cast<int>(std::stod(it->second.front()));
    } catch (...) {
      return fallback;
    }
  };

  ImageXObject image;
  image.width = get_int_value("W", 0);
  image.height = get_int_value("H", 0);
  image.bits_per_component = get_int_value("BPC", 8);

  if (image.width <= 0 || image.height <= 0) {
    return false;
  }

  std::string cs = get_string_value("CS");
  if (cs.empty()) {
    cs = get_string_value("ColorSpace");
  }
  if (!cs.empty() && cs.front() == '/') {
    cs = cs.substr(1);
  }

  if (cs == "DeviceRGB") {
    image.color_space = ColorSpace(ColorSpaceType::DeviceRGB);
  } else if (cs == "DeviceGray") {
    image.color_space = ColorSpace(ColorSpaceType::DeviceGray);
  } else {
    if (!cs.empty()) {
      return false;
    }
  }

  std::string filter = get_string_value("F");
  if (filter.empty()) {
    filter = get_string_value("Filter");
  }
  if (!filter.empty() && filter.front() == '/') {
    filter = filter.substr(1);
  }
  image.filter = filter;

  bool is_mask = get_bool_value("IM", false) || get_bool_value("ImageMask", false);
  image.image_mask = is_mask;
  if (is_mask) {
    image.color_space = ColorSpace(ColorSpaceType::DeviceGray);
    if (image.bits_per_component == 0) {
      image.bits_per_component = 1;
    }
  }

  auto decode_it = dict.find("D");
  if (decode_it == dict.end()) {
    decode_it = dict.find("Decode");
  }
  if (decode_it != dict.end()) {
    for (const std::string& token : decode_it->second) {
      try {
        image.decode.push_back(std::stod(token));
      } catch (...) {}
    }
  }

  image.raw_data = data;

  std::string filter_upper;
  filter_upper.reserve(filter.size());
  std::transform(filter.begin(), filter.end(), std::back_inserter(filter_upper), [](char ch) {
    return static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
  });

  filters::DecodeParams decode_params;
  decode_params.colors = (image.color_space.type == ColorSpaceType::DeviceRGB) ? 3 : 1;
  decode_params.bits_per_component = image.bits_per_component;
  decode_params.columns = image.width;

  auto decode_parms_it = dict.find("DP");
  if (decode_parms_it == dict.end()) {
    decode_parms_it = dict.find("DecodeParms");
  }
  if (decode_parms_it != dict.end() && !decode_parms_it->second.empty()) {
    std::vector<std::string> parms_tokens = decode_parms_it->second;
    if (!parms_tokens.empty() && parms_tokens.front() == "[") {
      size_t idx = 1;
      while (idx < parms_tokens.size()) {
        if (parms_tokens[idx] == "<<") {
          size_t start = idx;
          int depth = 1;
          ++idx;
          while (idx < parms_tokens.size() && depth > 0) {
            if (parms_tokens[idx] == "<<") depth++;
            else if (parms_tokens[idx] == ">>") depth--;
            ++idx;
          }
          std::vector<std::string> slice(parms_tokens.begin() + start, parms_tokens.begin() + idx);
          parse_inline_decode_parms(slice, &decode_params);
          break;
        }
        ++idx;
      }
    } else {
      parse_inline_decode_parms(parms_tokens, &decode_params);
    }
  }

  if (filter_upper.empty()) {
    image.data = data;
  } else if (filter_upper == "FLATEDECODE") {
    DecodedStream decoded = filters::decode_flate(data.data(), data.size(), decode_params);
    if (!decoded.success) {
      return false;
    }
    image.data = std::move(decoded.data);
  } else if (filter_upper == "DCTDECODE" || filter_upper == "JPXDECODE") {
    // Keep encoded data in raw_data; pixels will be derived on demand.
  } else {
    return false;
  }

  *out_image = std::move(image);
  return true;
}

bool CanvasExporter::convert_cmyk_to_rgb(const ImageXObject& image,
                                         std::vector<uint8_t>& out) const {
  size_t expected = static_cast<size_t>(image.width) * image.height * 4;
  if (image.data.size() < expected) {
    return false;
  }

  out.resize(static_cast<size_t>(image.width) * image.height * 3);
  for (size_t i = 0, j = 0; i + 3 < image.data.size(); i += 4) {
    double c = apply_decode_component(image, 0, static_cast<double>(image.data[i]) / 255.0);
    double m = apply_decode_component(image, 1, static_cast<double>(image.data[i + 1]) / 255.0);
    double y = apply_decode_component(image, 2, static_cast<double>(image.data[i + 2]) / 255.0);
    double k = apply_decode_component(image, 3, static_cast<double>(image.data[i + 3]) / 255.0);

    double r = 1.0 - std::min(1.0, c + k);
    double g = 1.0 - std::min(1.0, m + k);
    double b = 1.0 - std::min(1.0, y + k);

    out[j++] = static_cast<uint8_t>(std::round(r * 255.0));
    out[j++] = static_cast<uint8_t>(std::round(g * 255.0));
    out[j++] = static_cast<uint8_t>(std::round(b * 255.0));
  }

  return true;
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
    } else if (cmd.command == "lineWidth" || cmd.command == "lineCap" ||
               cmd.command == "lineJoin" || cmd.command == "miterLimit" ||
               cmd.command == "lineDashOffset") {
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
  current_pdf_ = &pdf;
  svg_mode_ = true;
  
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
  
  image_xobjects_ = parse_xobject_resources(pdf, page.resources);
  ext_gstates_ = parse_extgstate_resources(pdf, page.resources);
  inline_image_cursor_ = 0;
  TokenizedStream stream = tokenize_content_stream(content.data);
  inline_images_ = stream.inline_images;
  parse_content_stream_for_svg_from_tokens(stream.tokens);

  result.elements = svg_elements_;
  result.success = true;
  current_pdf_ = nullptr;
  pending_inline_mask_.reset();
  svg_mode_ = false;
  return result;
}

void CanvasExporter::parse_content_stream_for_svg_from_tokens(const std::vector<std::string>& tokens) {
  std::vector<std::string> operand_stack;

  for (size_t i = 0; i < tokens.size(); ++i) {
    const std::string& token = tokens[i];
    if (token.empty()) continue;

    if (token == "BI") {
      process_inline_image(tokens, i, true);
      continue;
    }
    if (token == "ID" || token == "EI") {
      continue;
    }

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
    } else if (token == "Do") {
      handle_xobject_command_svg(operand_stack);
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
    state_.text_matrix = Matrix2D{};
    state_.text_line_matrix = Matrix2D{};
    state_.leading = 0.0;
  } else if (op == "ET") {
    state_.in_text_block = false;
  } else if (op == "Tf" && operands.size() >= 2) {
    state_.current_font = operands[0];
    try {
      state_.font_size = std::stod(operands[1]);
    } catch (...) {
      state_.font_size = 12.0;
    }
  } else if (op == "TL" && operands.size() >= 1) {
    try {
      state_.leading = std::stod(operands[0]);
    } catch (...) {}
  } else if (op == "Td" && operands.size() >= 2) {
    try {
      double dx = std::stod(operands[0]);
      double dy = std::stod(operands[1]);
      state_.text_x += dx;
      state_.text_y += dy;
      Matrix2D translation;
      translation.e = dx;
      translation.f = dy;
      state_.text_line_matrix = multiply_matrices(state_.text_line_matrix, translation);
      state_.text_matrix = state_.text_line_matrix;
    } catch (...) {}
    state_.text_x = state_.text_matrix.e;
    state_.text_y = state_.text_matrix.f;
  } else if (op == "TD" && operands.size() >= 2) {
    try {
      double dx = std::stod(operands[0]);
      double dy = std::stod(operands[1]);
      state_.text_x += dx;
      state_.text_y += dy;
      state_.leading = -dy;
      Matrix2D translation;
      translation.e = dx;
      translation.f = dy;
      state_.text_line_matrix = multiply_matrices(state_.text_line_matrix, translation);
      state_.text_matrix = state_.text_line_matrix;
    } catch (...) {}
    state_.text_x = state_.text_matrix.e;
    state_.text_y = state_.text_matrix.f;
  } else if (op == "Tm" && operands.size() >= 6) {
    try {
      state_.text_matrix.a = std::stod(operands[0]);
      state_.text_matrix.b = std::stod(operands[1]);
      state_.text_matrix.c = std::stod(operands[2]);
      state_.text_matrix.d = std::stod(operands[3]);
      state_.text_matrix.e = std::stod(operands[4]);
      state_.text_matrix.f = std::stod(operands[5]);
      state_.text_line_matrix = state_.text_matrix;
      state_.text_x = state_.text_matrix.e;
      state_.text_y = state_.text_matrix.f;
    } catch (...) {}
  } else if (op == "T*") {
    double move_y = state_.leading;
    if (std::abs(move_y) < 1e-6) {
      move_y = state_.font_size;
    }
    Matrix2D translation;
    translation.e = 0.0;
    translation.f = -move_y;
    state_.text_line_matrix = multiply_matrices(state_.text_line_matrix, translation);
    state_.text_matrix = state_.text_line_matrix;
    state_.text_x = state_.text_matrix.e;
    state_.text_y = state_.text_matrix.f;
  } else if (op == "Tj" && operands.size() >= 1) {
    std::string text = operands[0];
    if (text.length() >= 2 && text[0] == '(' && text[text.length()-1] == ')') {
      text = text.substr(1, text.length()-2);
    }

    double svg_x = state_.text_matrix.e;
    double svg_y = state_.text_matrix.f;
    transform_coordinates_svg(svg_x, svg_y);
    
    std::map<std::string, std::string> attrs;
    attrs["x"] = double_to_string(svg_x);
    attrs["y"] = double_to_string(svg_y);
    attrs["font-size"] = double_to_string(state_.font_size);
    attrs["fill"] = state_.fill_color;
    if (std::abs(state_.fill_alpha - 1.0) > 1e-6) {
      attrs["fill-opacity"] = double_to_string(state_.fill_alpha);
    }
    std::string css_mode = blend_mode_to_css(state_.blend_mode);
    if (!css_mode.empty()) {
      attrs["style"] = "mix-blend-mode:" + css_mode + ';';
    }
    
    add_svg_element("text", attrs, text);
  } else if (op == "TJ" && !operands.empty()) {
    std::string combined;
    for (const std::string& token : operands) {
      if (token.length() >= 2 && token.front() == '(' && token.back() == ')') {
        combined += token.substr(1, token.length() - 2);
      }
    }

    if (!combined.empty()) {
      double svg_x = state_.text_matrix.e;
      double svg_y = state_.text_matrix.f;
      transform_coordinates_svg(svg_x, svg_y);

      std::map<std::string, std::string> attrs;
      attrs["x"] = double_to_string(svg_x);
      attrs["y"] = double_to_string(svg_y);
      attrs["font-size"] = double_to_string(state_.font_size);
      attrs["fill"] = state_.fill_color;
      if (std::abs(state_.fill_alpha - 1.0) > 1e-6) {
        attrs["fill-opacity"] = double_to_string(state_.fill_alpha);
      }
      std::string css_mode2 = blend_mode_to_css(state_.blend_mode);
      if (!css_mode2.empty()) {
        attrs["style"] = "mix-blend-mode:" + css_mode2 + ';';
      }

      add_svg_element("text", attrs, combined);
    }
  }
}

void CanvasExporter::handle_path_command_svg(const std::string& op, 
                                            const std::vector<std::string>& operands) {
  if (op == "m" && operands.size() >= 2) {
    try {
      double x = std::stod(operands[0]);
      double y = std::stod(operands[1]);
      transform_coordinates_svg(x, y);
      if (!state_.in_path) {
        state_.path_data.clear();
        state_.in_path = true;
      }
      state_.path_data += "M " + double_to_string(x) + " " + double_to_string(y) + " ";
      state_.current_x = x;
      state_.current_y = y;
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
  } else if (op == "v" && operands.size() >= 4) {
    try {
      double x2 = std::stod(operands[0]);
      double y2 = std::stod(operands[1]);
      double x3 = std::stod(operands[2]);
      double y3 = std::stod(operands[3]);
      transform_coordinates_svg(x2, y2);
      transform_coordinates_svg(x3, y3);
      double x1 = state_.current_x;
      double y1 = state_.current_y;
      state_.path_data += "C " + double_to_string(x1) + " " + double_to_string(y1) + " " +
                         double_to_string(x2) + " " + double_to_string(y2) + " " +
                         double_to_string(x3) + " " + double_to_string(y3) + " ";
      state_.current_x = x3;
      state_.current_y = y3;
    } catch (...) {}
  } else if (op == "y" && operands.size() >= 4) {
    try {
      double x1 = std::stod(operands[0]);
      double y1 = std::stod(operands[1]);
      double x3 = std::stod(operands[2]);
      double y3 = std::stod(operands[3]);
      transform_coordinates_svg(x1, y1);
      transform_coordinates_svg(x3, y3);
      state_.path_data += "C " + double_to_string(x1) + " " + double_to_string(y1) + " " +
                         double_to_string(x3) + " " + double_to_string(y3) + " " +
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

      double x0 = x;
      double y0 = y;
      double x1 = x + w;
      double y1 = y + h;

      transform_coordinates_svg(x0, y0);
      transform_coordinates_svg(x1, y1);

      double x2 = x1;
      double y2 = y0;
      double x3 = x0;
      double y3 = y1;

      if (!state_.in_path) {
        state_.path_data.clear();
        state_.in_path = true;
      }

      state_.path_data += "M " + double_to_string(x0) + " " + double_to_string(y0) + " ";
      state_.path_data += "L " + double_to_string(x2) + " " + double_to_string(y2) + " ";
      state_.path_data += "L " + double_to_string(x1) + " " + double_to_string(y1) + " ";
      state_.path_data += "L " + double_to_string(x3) + " " + double_to_string(y3) + " ";
      state_.path_data += "Z ";

      state_.current_x = x0;
      state_.current_y = y0;
    } catch (...) {}
  } else if (op == "h") {
    state_.path_data += "Z ";
  } else if (op == "S") {
    emit_svg_path(false, false, true, false);
  } else if (op == "s") {
    emit_svg_path(true, false, true, false);
  } else if (op == "f" || op == "F") {
    emit_svg_path(false, true, false, false);
  } else if (op == "f*") {
    emit_svg_path(false, true, false, true);
  } else if (op == "B") {
    emit_svg_path(false, true, true, false);
  } else if (op == "B*") {
    emit_svg_path(false, true, true, true);
  } else if (op == "b") {
    emit_svg_path(true, true, true, false);
  } else if (op == "b*") {
    emit_svg_path(true, true, true, true);
  } else if (op == "n") {
    state_.path_data.clear();
    state_.in_path = false;
  }
}

void CanvasExporter::emit_svg_path(bool close_path, bool fill, bool stroke, bool use_evenodd) {
  if (!state_.in_path || state_.path_data.empty()) {
    return;
  }

  std::string path_data = state_.path_data;
  while (!path_data.empty() && std::isspace(static_cast<unsigned char>(path_data.back()))) {
    path_data.pop_back();
  }

  if (close_path) {
    if (path_data.empty() || (path_data.back() != 'Z' && path_data.back() != 'z')) {
      if (!path_data.empty()) {
        path_data += ' ';
      }
      path_data += 'Z';
    }
  }

  if (path_data.empty()) {
    state_.path_data.clear();
    state_.in_path = false;
    return;
  }

  std::map<std::string, std::string> attrs;
  attrs["d"] = path_data;

  if (fill) {
    attrs["fill"] = state_.fill_color;
    if (use_evenodd) {
      attrs["fill-rule"] = "evenodd";
    }
    if (std::abs(state_.fill_alpha - 1.0) > 1e-6) {
      attrs["fill-opacity"] = double_to_string(state_.fill_alpha);
    }
  } else {
    attrs["fill"] = "none";
  }

  if (stroke) {
    attrs["stroke"] = state_.stroke_color;
    attrs["stroke-width"] = double_to_string(state_.stroke_width);
    if (!state_.line_cap.empty()) {
      attrs["stroke-linecap"] = state_.line_cap;
    }
    if (!state_.line_join.empty()) {
      attrs["stroke-linejoin"] = state_.line_join;
    }
    if (std::abs(state_.miter_limit - 10.0) > 1e-6) {
      attrs["stroke-miterlimit"] = double_to_string(state_.miter_limit);
    }
    if (!state_.dash_pattern.empty()) {
      std::string dash_array = dash_pattern_to_string(state_.dash_pattern);
      if (!dash_array.empty()) {
        attrs["stroke-dasharray"] = dash_array;
      }
      if (std::abs(state_.dash_phase) > 1e-6) {
        attrs["stroke-dashoffset"] = double_to_string(state_.dash_phase);
      }
    }
    if (std::abs(state_.stroke_alpha - 1.0) > 1e-6) {
      attrs["stroke-opacity"] = double_to_string(state_.stroke_alpha);
    }
  } else {
    attrs["stroke"] = "none";
  }

  std::string css_mode = blend_mode_to_css(state_.blend_mode);
  if (!css_mode.empty()) {
    attrs["style"] = "mix-blend-mode:" + css_mode + ';';
  }

  add_svg_element("path", attrs);
  state_.path_data.clear();
  state_.in_path = false;
}

void CanvasExporter::handle_graphics_state_command_svg(const std::string& op, 
                                                      const std::vector<std::string>& operands) {
  if (op == "q") {
    graphics_state_stack_.push_back(state_);
  } else if (op == "Q") {
    if (!graphics_state_stack_.empty()) {
      state_ = graphics_state_stack_.back();
      graphics_state_stack_.pop_back();
    }
  } else if (op == "cm" && operands.size() >= 6) {
    try {
      Matrix2D matrix;
      matrix.a = std::stod(operands[0]);
      matrix.b = std::stod(operands[1]);
      matrix.c = std::stod(operands[2]);
      matrix.d = std::stod(operands[3]);
      matrix.e = std::stod(operands[4]);
      matrix.f = std::stod(operands[5]);
      state_.ctm = multiply_matrices(state_.ctm, matrix);
    } catch (...) {}
  } else if (op == "w" && operands.size() >= 1) {
    try {
      state_.stroke_width = std::stod(operands[0]);
    } catch (...) {}
  } else if (op == "gs" && !operands.empty()) {
    std::string name;
    for (auto it = operands.rbegin(); it != operands.rend(); ++it) {
      if (it->empty() || *it == "/") continue;
      name = *it;
      break;
    }
    if (!name.empty() && name.front() == '/') {
      name.erase(0, 1);
    }
    if (!name.empty()) {
      apply_extended_graphics_state(name);
    }
  } else if (op == "J" && operands.size() >= 1) {
    try {
      int cap = static_cast<int>(std::stod(operands[0]));
      switch (cap) {
        case 0:
          state_.line_cap = "butt";
          break;
        case 1:
          state_.line_cap = "round";
          break;
        case 2:
          state_.line_cap = "square";
          break;
        default:
          break;
      }
    } catch (...) {}
  } else if (op == "j" && operands.size() >= 1) {
    try {
      int join = static_cast<int>(std::stod(operands[0]));
      switch (join) {
        case 0:
          state_.line_join = "miter";
          break;
        case 1:
          state_.line_join = "round";
          break;
        case 2:
          state_.line_join = "bevel";
          break;
        default:
          break;
      }
    } catch (...) {}
  } else if (op == "M" && operands.size() >= 1) {
    try {
      state_.miter_limit = std::stod(operands[0]);
    } catch (...) {}
  } else if (op == "d" && operands.size() >= 2) {
    std::vector<double> pattern;
    double phase = 0.0;
    parse_dash_pattern(operands, &pattern, &phase);
    state_.dash_pattern = pattern;
    state_.dash_phase = phase;
  }
}

void CanvasExporter::handle_color_command_svg(const std::string& op, 
                                             const std::vector<std::string>& operands) {
  if (op == "rg" && operands.size() >= 3) {
    try {
      double r = std::stod(operands[0]);
      double g = std::stod(operands[1]);
      double b = std::stod(operands[2]);
      state_.fill_color = rgb_to_hex(r, g, b);
    } catch (...) {}
  } else if (op == "RG" && operands.size() >= 3) {
    try {
      double r = std::stod(operands[0]);
      double g = std::stod(operands[1]);
      double b = std::stod(operands[2]);
      state_.stroke_color = rgb_to_hex(r, g, b);
    } catch (...) {}
  } else if (op == "g" && operands.size() >= 1) {
    try {
      double gray = std::stod(operands[0]);
      state_.fill_color = rgb_to_hex(gray, gray, gray);
    } catch (...) {}
  } else if (op == "G" && operands.size() >= 1) {
    try {
      double gray = std::stod(operands[0]);
      state_.stroke_color = rgb_to_hex(gray, gray, gray);
    } catch (...) {}
  } else if (op == "k" && operands.size() >= 4) {
    try {
      double c = std::stod(operands[0]);
      double m = std::stod(operands[1]);
      double y = std::stod(operands[2]);
      double k_val = std::stod(operands[3]);
      c = std::max(0.0, std::min(1.0, c));
      m = std::max(0.0, std::min(1.0, m));
      y = std::max(0.0, std::min(1.0, y));
      k_val = std::max(0.0, std::min(1.0, k_val));
      double r = 1.0 - std::min(1.0, c + k_val);
      double g = 1.0 - std::min(1.0, m + k_val);
      double b = 1.0 - std::min(1.0, y + k_val);
      state_.fill_color = rgb_to_hex(r, g, b);
      update_canvas_fill_style();
    } catch (...) {}
  } else if (op == "K" && operands.size() >= 4) {
    try {
      double c = std::stod(operands[0]);
      double m = std::stod(operands[1]);
      double y = std::stod(operands[2]);
      double k_val = std::stod(operands[3]);
      c = std::max(0.0, std::min(1.0, c));
      m = std::max(0.0, std::min(1.0, m));
      y = std::max(0.0, std::min(1.0, y));
      k_val = std::max(0.0, std::min(1.0, k_val));
      double r = 1.0 - std::min(1.0, c + k_val);
      double g = 1.0 - std::min(1.0, m + k_val);
      double b = 1.0 - std::min(1.0, y + k_val);
      state_.stroke_color = rgb_to_hex(r, g, b);
      update_canvas_stroke_style();
    } catch (...) {}
  }
}

void CanvasExporter::apply_extended_graphics_state(const std::string& name) {
  auto it = ext_gstates_.find(name);
  if (it != ext_gstates_.end()) {
    apply_extended_graphics_state(it->second);
  }
}

void CanvasExporter::apply_extended_graphics_state(const ExtendedGraphicsState& ext_state) {
  if (ext_state.line_width > 0.0) {
    state_.stroke_width = ext_state.line_width;
    update_canvas_line_width();
  }

  switch (ext_state.line_cap) {
    case 0:
      state_.line_cap = "butt";
      break;
    case 1:
      state_.line_cap = "round";
      break;
    case 2:
      state_.line_cap = "square";
      break;
    default:
      break;
  }
  update_canvas_line_cap();

  switch (ext_state.line_join) {
    case 0:
      state_.line_join = "miter";
      break;
    case 1:
      state_.line_join = "round";
      break;
    case 2:
      state_.line_join = "bevel";
      break;
    default:
      break;
  }
  update_canvas_line_join();

  if (ext_state.miter_limit > 0.0) {
    state_.miter_limit = ext_state.miter_limit;
    update_canvas_miter_limit();
  }

  state_.dash_pattern = ext_state.dash_pattern;
  state_.dash_phase = ext_state.dash_phase;
  update_canvas_line_dash();

  if (ext_state.soft_mask_type != SoftMaskType::None) {
    auto g_it = ext_state.soft_mask_dict.find("G");
    if (g_it != ext_state.soft_mask_dict.end()) {
      state_.soft_mask_type = ext_state.soft_mask_type;
      state_.soft_mask_value = g_it->second;
    } else {
      state_.soft_mask_type = SoftMaskType::None;
      state_.soft_mask_value = Value();
    }
  } else {
    state_.soft_mask_type = SoftMaskType::None;
    state_.soft_mask_value = Value();
  }

  auto clamp_unit = [](double value) {
    if (value < 0.0) return 0.0;
    if (value > 1.0) return 1.0;
    return value;
  };

  if (ext_state.ca >= 0.0) {
    state_.fill_alpha = clamp_unit(ext_state.ca);
    update_canvas_fill_style();
  }
  if (ext_state.CA >= 0.0) {
    state_.stroke_alpha = clamp_unit(ext_state.CA);
    update_canvas_stroke_style();
  }

  std::string new_mode = blend_mode_to_canvas(ext_state.blend_mode);
  if (new_mode != state_.blend_mode) {
    state_.blend_mode = new_mode;
    update_canvas_blend_mode();
  }
}

void CanvasExporter::handle_xobject_command_svg(const std::vector<std::string>& operands) {
  if (operands.empty()) {
    return;
  }

  std::string name = operands.back();
  if (!name.empty() && name.front() == '/') {
    name = name.substr(1);
  }

  auto it = image_xobjects_.find(name);
  if (it == image_xobjects_.end()) {
    return;
  }

  emit_svg_image(name, it->second);
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
  apply_matrix_to_point(state_.ctm, x, y);
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

std::string CanvasExporter::escape_xml_string(const std::string& str) const {
  std::string escaped;
  escaped.reserve(str.length());

  for (char c : str) {
    switch (c) {
      case '&':
        escaped += "&amp;";
        break;
      case '<':
        escaped += "&lt;";
        break;
      case '>':
        escaped += "&gt;";
        break;
      case '"':
        escaped += "&quot;";
        break;
      case '\'':
        escaped += "&apos;";
        break;
      default:
        escaped += c;
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

std::string CanvasExporter::svg_to_markup(const std::vector<SvgElement>& elements) const {
  std::stringstream svg;

  svg << "<svg xmlns=\"http://www.w3.org/2000/svg\" xmlns:xlink=\"http://www.w3.org/1999/xlink\" width=\""
      << double_to_string(page_width_) << "\" height=\"" << double_to_string(page_height_)
      << "\" viewBox=\"0 0 " << double_to_string(page_width_) << " "
      << double_to_string(page_height_) << "\">\n";

  for (const SvgElement& elem : elements) {
    svg << "  <" << elem.element_type;
    for (const auto& attr : elem.attributes) {
      svg << " " << attr.first << "=\"" << escape_xml_string(attr.second) << "\"";
    }

    if (elem.text_content.empty()) {
      svg << " />\n";
    } else {
      svg << ">" << escape_xml_string(elem.text_content)
          << "</" << elem.element_type << ">\n";
    }
  }

  svg << "</svg>\n";
  return svg.str();
}

void CanvasExporter::queue_canvas_image(double a, double b, double c, double d,
                                        double e, double f,
                                        const std::string& data_url) {
  CanvasCommand cmd("drawImageDataURL");
  cmd.args.push_back("\"" + data_url + "\"");
  cmd.args.push_back(double_to_string(a));
  cmd.args.push_back(double_to_string(b));
  cmd.args.push_back(double_to_string(c));
  cmd.args.push_back(double_to_string(d));
  cmd.args.push_back(double_to_string(e));
  cmd.args.push_back(double_to_string(f));
  canvas_commands_.push_back(std::move(cmd));
}

}  // namespace nanopdf
