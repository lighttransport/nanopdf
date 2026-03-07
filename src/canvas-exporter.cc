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
  current_page_ = nullptr;
  graphics_state_stack_.clear();
  state_ = GraphicsState{};
  canvas_mode_ = false;
  svg_mode_ = false;

  // Clear gradient and pattern definitions
  svg_gradients_.clear();
  svg_patterns_.clear();
  svg_gradient_counter_ = 0;
  svg_pattern_counter_ = 0;
  svg_clip_counter_ = 0;
  shading_resources_.clear();
  pattern_resources_.clear();
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
  
  current_page_ = &page;
  image_xobjects_ = parse_xobject_resources(pdf, page.resources);
  ext_gstates_ = parse_extgstate_resources(pdf, page.resources);
  parse_shading_resources(pdf, page.resources);
  parse_pattern_resources(pdf, page.resources);
  inline_image_cursor_ = 0;
  TokenizedStream stream = tokenize_content_stream(content.data);
  inline_images_ = stream.inline_images;
  parse_content_stream_from_tokens(stream.tokens);

  add_canvas_command("restore");

  result.commands = canvas_commands_;
  result.success = true;
  current_page_ = nullptr;
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
    } else if (token == "Do") {
      handle_xobject_command(operand_stack);
      is_operator = true;
    } else if (token == "sh") {
      handle_shading_command(operand_stack);
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
  } else if (op == "v" && operands.size() >= 4) {
    // Curve with first control point = current point
    try {
      double x2 = std::stod(operands[0]);
      double y2 = std::stod(operands[1]);
      double x3 = std::stod(operands[2]);
      double y3 = std::stod(operands[3]);
      transform_coordinates(x2, y2);
      transform_coordinates(x3, y3);
      add_canvas_command("bezierCurveTo", {
        double_to_string(state_.current_x), double_to_string(state_.current_y),
        double_to_string(x2), double_to_string(y2),
        double_to_string(x3), double_to_string(y3)
      });
      state_.current_x = x3;
      state_.current_y = y3;
    } catch (...) {}
  } else if (op == "y" && operands.size() >= 4) {
    // Curve with last control point = end point
    try {
      double x1 = std::stod(operands[0]);
      double y1 = std::stod(operands[1]);
      double x3 = std::stod(operands[2]);
      double y3 = std::stod(operands[3]);
      transform_coordinates(x1, y1);
      transform_coordinates(x3, y3);
      add_canvas_command("bezierCurveTo", {
        double_to_string(x1), double_to_string(y1),
        double_to_string(x3), double_to_string(y3),
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
      if (!state_.in_path) {
        add_canvas_command("beginPath");
        state_.in_path = true;
      }
      add_canvas_command("rect", {
        double_to_string(x), double_to_string(y),
        double_to_string(w), double_to_string(h)
      });
    } catch (...) {}
  } else if (op == "h") {
    add_canvas_command("closePath");
  } else if (op == "W") {
    state_.pending_clip = true;
    state_.pending_clip_evenodd = false;
  } else if (op == "W*") {
    state_.pending_clip = true;
    state_.pending_clip_evenodd = true;
  } else if (op == "S") {
    apply_pending_clip();
    add_canvas_command("stroke");
    state_.in_path = false;
  } else if (op == "s") {
    add_canvas_command("closePath");
    apply_pending_clip();
    add_canvas_command("stroke");
    state_.in_path = false;
  } else if (op == "f" || op == "F") {
    apply_pending_clip();
    add_canvas_command("fill");
    state_.in_path = false;
  } else if (op == "f*") {
    apply_pending_clip();
    add_canvas_command("fill", {"\"evenodd\""});
    state_.in_path = false;
  } else if (op == "B") {
    apply_pending_clip();
    add_canvas_command("fill");
    add_canvas_command("stroke");
    state_.in_path = false;
  } else if (op == "B*") {
    apply_pending_clip();
    add_canvas_command("fill", {"\"evenodd\""});
    add_canvas_command("stroke");
    state_.in_path = false;
  } else if (op == "b") {
    add_canvas_command("closePath");
    apply_pending_clip();
    add_canvas_command("fill");
    add_canvas_command("stroke");
    state_.in_path = false;
  } else if (op == "b*") {
    add_canvas_command("closePath");
    apply_pending_clip();
    add_canvas_command("fill", {"\"evenodd\""});
    add_canvas_command("stroke");
    state_.in_path = false;
  } else if (op == "n") {
    apply_pending_clip();
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
    try {
      Matrix2D m;
      m.a = std::stod(operands[0]);
      m.b = std::stod(operands[1]);
      m.c = std::stod(operands[2]);
      m.d = std::stod(operands[3]);
      m.e = std::stod(operands[4]);
      m.f = std::stod(operands[5]);
      state_.ctm = multiply_matrices(state_.ctm, m);
    } catch (...) {}
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
      state_.fill_color_space = "DeviceRGB";
      state_.fill_color = rgb_to_hex(std::stod(operands[0]), std::stod(operands[1]), std::stod(operands[2]));
      update_canvas_fill_style();
    } catch (...) {}
  } else if (op == "RG" && operands.size() >= 3) {
    try {
      state_.stroke_color_space = "DeviceRGB";
      state_.stroke_color = rgb_to_hex(std::stod(operands[0]), std::stod(operands[1]), std::stod(operands[2]));
      update_canvas_stroke_style();
    } catch (...) {}
  } else if (op == "g" && operands.size() >= 1) {
    try {
      state_.fill_color_space = "DeviceGray";
      double gray = std::stod(operands[0]);
      state_.fill_color = rgb_to_hex(gray, gray, gray);
      update_canvas_fill_style();
    } catch (...) {}
  } else if (op == "G" && operands.size() >= 1) {
    try {
      state_.stroke_color_space = "DeviceGray";
      double gray = std::stod(operands[0]);
      state_.stroke_color = rgb_to_hex(gray, gray, gray);
      update_canvas_stroke_style();
    } catch (...) {}
  } else if (op == "k" && operands.size() >= 4) {
    try {
      state_.fill_color_space = "DeviceCMYK";
      state_.fill_color = cmyk_to_hex(std::stod(operands[0]), std::stod(operands[1]),
                                       std::stod(operands[2]), std::stod(operands[3]));
      update_canvas_fill_style();
    } catch (...) {}
  } else if (op == "K" && operands.size() >= 4) {
    try {
      state_.stroke_color_space = "DeviceCMYK";
      state_.stroke_color = cmyk_to_hex(std::stod(operands[0]), std::stod(operands[1]),
                                         std::stod(operands[2]), std::stod(operands[3]));
      update_canvas_stroke_style();
    } catch (...) {}
  } else if (op == "cs" && !operands.empty()) {
    std::string cs = operands.back();
    if (!cs.empty() && cs.front() == '/') cs.erase(0, 1);
    state_.fill_color_space = cs;
  } else if (op == "CS" && !operands.empty()) {
    std::string cs = operands.back();
    if (!cs.empty() && cs.front() == '/') cs.erase(0, 1);
    state_.stroke_color_space = cs;
  } else if (op == "sc" || op == "scn") {
    std::string color = resolve_scn_color(operands, false);
    if (!color.empty()) {
      state_.fill_color = color;
      update_canvas_fill_style();
    }
  } else if (op == "SC" || op == "SCN") {
    std::string color = resolve_scn_color(operands, true);
    if (!color.empty()) {
      state_.stroke_color = color;
      update_canvas_stroke_style();
    }
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

  // Apply transparency: fill/stroke alpha → opacity on image
  double opacity = std::min(state_.fill_alpha, state_.stroke_alpha);
  if (std::abs(opacity - 1.0) > 1e-6) {
    attrs["opacity"] = double_to_string(opacity);
  }

  // Apply blend mode
  std::string css_mode = blend_mode_to_css(state_.blend_mode);
  if (!css_mode.empty()) {
    std::string style;
    auto style_it = attrs.find("style");
    if (style_it != attrs.end()) {
      style = style_it->second;
    }
    if (!style.empty() && style.back() != ';') style += ";";
    style += "mix-blend-mode:" + css_mode;
    attrs["style"] = style;
  }

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
  
  int gradient_counter = 0;
  for (const CanvasCommand& cmd : commands) {
    if (cmd.args.empty()) {
      js << "ctx." << cmd.command << "();\n";
    } else if (cmd.command == "fillStyle" || cmd.command == "strokeStyle" ||
               cmd.command == "font") {
      js << "ctx." << cmd.command << " = " << cmd.args[0] << ";\n";
    } else if (cmd.command == "lineWidth" || cmd.command == "lineCap" ||
               cmd.command == "lineJoin" || cmd.command == "miterLimit" ||
               cmd.command == "lineDashOffset" || cmd.command == "globalAlpha" ||
               cmd.command == "globalCompositeOperation") {
      js << "ctx." << cmd.command << " = " << cmd.args[0] << ";\n";
    } else if (cmd.command == "createLinearGradient" || cmd.command == "createRadialGradient") {
      ++gradient_counter;
      js << "var _grad" << gradient_counter << " = ctx." << cmd.command << "(";
      for (size_t i = 0; i < cmd.args.size(); ++i) {
        if (i > 0) js << ", ";
        js << cmd.args[i];
      }
      js << ");\n";
    } else if (cmd.command == "addColorStop") {
      if (gradient_counter > 0 && cmd.args.size() >= 2) {
        js << "_grad" << gradient_counter << ".addColorStop(" << cmd.args[0] << ", " << cmd.args[1] << ");\n";
      }
    } else if (cmd.command == "fillGradient") {
      if (gradient_counter > 0) {
        js << "ctx.fillStyle = _grad" << gradient_counter << ";\n";
      }
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
  
  current_page_ = &page;
  image_xobjects_ = parse_xobject_resources(pdf, page.resources);
  ext_gstates_ = parse_extgstate_resources(pdf, page.resources);
  parse_shading_resources(pdf, page.resources);
  parse_pattern_resources(pdf, page.resources);
  inline_image_cursor_ = 0;
  TokenizedStream stream = tokenize_content_stream(content.data);
  inline_images_ = stream.inline_images;
  parse_content_stream_for_svg_from_tokens(stream.tokens);

  result.elements = svg_elements_;
  result.success = true;
  current_page_ = nullptr;
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
    } else if (token == "sh") {
      handle_shading_command_svg(operand_stack);
      is_operator = true;
    } else if (token == "BDC") {
      // Begin Marked Content with properties - check for OCG reference
      bool is_visible = true;
      if (operand_stack.size() >= 2 && operand_stack[0] == "/OC" && current_pdf_) {
        // Look up the OCG properties dict name in page resources
        std::string props_name = operand_stack[1];
        if (props_name.size() > 0 && props_name[0] == '/')
          props_name = props_name.substr(1);
        // Check OCG visibility
        const_cast<Pdf*>(current_pdf_)->ensure_optional_content_loaded();
        const auto& ocg_props = current_pdf_->catalog.ocg_properties;
        for (const auto& ocg : ocg_props.ocgs) {
          if (ocg.name == props_name && !ocg.visible) {
            is_visible = false;
            break;
          }
        }
      }
      marked_content_visibility_stack_.push_back(is_visible);
      if (!is_visible)
        ocg_skip_depth_++;
      is_operator = true;
    } else if (token == "BMC") {
      // Begin Marked Content (simple tag) - always visible
      marked_content_visibility_stack_.push_back(true);
      is_operator = true;
    } else if (token == "EMC") {
      // End Marked Content
      if (!marked_content_visibility_stack_.empty()) {
        bool was_visible = marked_content_visibility_stack_.back();
        marked_content_visibility_stack_.pop_back();
        if (!was_visible)
          ocg_skip_depth_--;
      }
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
  // Skip rendering when inside a hidden OCG layer
  if (ocg_skip_depth_ > 0) return;

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
  // Skip rendering when inside a hidden OCG layer
  if (ocg_skip_depth_ > 0) return;

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
  } else if (op == "W") {
    state_.pending_clip = true;
    state_.pending_clip_evenodd = false;
  } else if (op == "W*") {
    state_.pending_clip = true;
    state_.pending_clip_evenodd = true;
  } else if (op == "n") {
    apply_pending_clip_svg();
    state_.path_data.clear();
    state_.in_path = false;
  }
}

void CanvasExporter::emit_svg_path(bool close_path, bool fill, bool stroke, bool use_evenodd) {
  if (!state_.in_path || state_.path_data.empty()) {
    return;
  }

  // Apply pending clipping path before painting
  if (state_.pending_clip) {
    apply_pending_clip_svg();
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
    // Close any open clip groups from this save level
    for (int i = 0; i < state_.svg_clip_group_depth; ++i) {
      add_svg_element("/g");
    }
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
      state_.fill_color_space = "DeviceRGB";
      state_.fill_color = rgb_to_hex(std::stod(operands[0]), std::stod(operands[1]), std::stod(operands[2]));
    } catch (...) {}
  } else if (op == "RG" && operands.size() >= 3) {
    try {
      state_.stroke_color_space = "DeviceRGB";
      state_.stroke_color = rgb_to_hex(std::stod(operands[0]), std::stod(operands[1]), std::stod(operands[2]));
    } catch (...) {}
  } else if (op == "g" && operands.size() >= 1) {
    try {
      state_.fill_color_space = "DeviceGray";
      double gray = std::stod(operands[0]);
      state_.fill_color = rgb_to_hex(gray, gray, gray);
    } catch (...) {}
  } else if (op == "G" && operands.size() >= 1) {
    try {
      state_.stroke_color_space = "DeviceGray";
      double gray = std::stod(operands[0]);
      state_.stroke_color = rgb_to_hex(gray, gray, gray);
    } catch (...) {}
  } else if (op == "k" && operands.size() >= 4) {
    try {
      state_.fill_color_space = "DeviceCMYK";
      state_.fill_color = cmyk_to_hex(std::stod(operands[0]), std::stod(operands[1]),
                                       std::stod(operands[2]), std::stod(operands[3]));
    } catch (...) {}
  } else if (op == "K" && operands.size() >= 4) {
    try {
      state_.stroke_color_space = "DeviceCMYK";
      state_.stroke_color = cmyk_to_hex(std::stod(operands[0]), std::stod(operands[1]),
                                         std::stod(operands[2]), std::stod(operands[3]));
    } catch (...) {}
  } else if (op == "cs" && !operands.empty()) {
    std::string cs = operands.back();
    if (!cs.empty() && cs.front() == '/') cs.erase(0, 1);
    state_.fill_color_space = cs;
  } else if (op == "CS" && !operands.empty()) {
    std::string cs = operands.back();
    if (!cs.empty() && cs.front() == '/') cs.erase(0, 1);
    state_.stroke_color_space = cs;
  } else if (op == "sc" || op == "scn") {
    std::string color = resolve_scn_color(operands, false);
    if (!color.empty()) {
      state_.fill_color = color;
    }
  } else if (op == "SC" || op == "SCN") {
    std::string color = resolve_scn_color(operands, true);
    if (!color.empty()) {
      state_.stroke_color = color;
    }
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

// Helper: apply pending clipping path in canvas mode
void CanvasExporter::apply_pending_clip() {
  if (!state_.pending_clip) return;
  if (state_.pending_clip_evenodd) {
    add_canvas_command("clip", {"\"evenodd\""});
  } else {
    add_canvas_command("clip");
  }
  state_.pending_clip = false;
  state_.pending_clip_evenodd = false;
}

// Helper: apply pending clipping path in SVG mode
void CanvasExporter::apply_pending_clip_svg() {
  if (!state_.pending_clip || !state_.in_path || state_.path_data.empty()) {
    state_.pending_clip = false;
    state_.pending_clip_evenodd = false;
    return;
  }

  std::string clip_id = "clip_" + std::to_string(++svg_clip_counter_);

  // Emit <clipPath> definition
  std::map<std::string, std::string> clip_attrs;
  clip_attrs["id"] = clip_id;
  add_svg_element("clipPath", clip_attrs);

  // Emit path inside clipPath
  std::map<std::string, std::string> path_attrs;
  path_attrs["d"] = state_.path_data;
  if (state_.pending_clip_evenodd) {
    path_attrs["clip-rule"] = "evenodd";
  }
  add_svg_element("path", path_attrs);
  add_svg_element("/clipPath");

  // Wrap subsequent content in a <g> with clip-path
  std::map<std::string, std::string> g_attrs;
  g_attrs["clip-path"] = "url(#" + clip_id + ")";
  add_svg_element("g", g_attrs);
  state_.svg_clip_group_depth++;

  state_.pending_clip = false;
  state_.pending_clip_evenodd = false;
}

// CMYK to hex conversion helper
std::string CanvasExporter::cmyk_to_hex(double c, double m, double y, double k) const {
  c = std::max(0.0, std::min(1.0, c));
  m = std::max(0.0, std::min(1.0, m));
  y = std::max(0.0, std::min(1.0, y));
  k = std::max(0.0, std::min(1.0, k));
  double r = (1.0 - c) * (1.0 - k);
  double g = (1.0 - m) * (1.0 - k);
  double b = (1.0 - y) * (1.0 - k);
  return rgb_to_hex(r, g, b);
}

// Resolve SC/SCN/sc/scn color based on current color space
std::string CanvasExporter::resolve_scn_color(const std::vector<std::string>& operands,
                                               bool is_stroking) {
  if (operands.empty()) return "";

  const std::string& cs = is_stroking ? state_.stroke_color_space : state_.fill_color_space;

  // Check if last operand is a pattern name (starts with /)
  std::string last = operands.back();
  if (!last.empty() && last.front() == '/') {
    // Pattern color space - look up pattern resource
    std::string pat_name = last.substr(1);
    auto it = pattern_resources_.find(pat_name);
    if (it != pattern_resources_.end()) {
      // For SVG mode, create gradient/pattern reference
      if (svg_mode_) {
        return create_svg_pattern(it->second, state_.ctm);
      }
      // For canvas mode, try to create gradient from shading pattern
      // (Canvas doesn't support tiling patterns natively)
      return create_svg_gradient(it->second, state_.ctm);
    }
    return "";
  }

  try {
    if ((cs == "DeviceGray" || cs == "CalGray") && operands.size() >= 1) {
      double gray = std::stod(operands[0]);
      return rgb_to_hex(gray, gray, gray);
    } else if ((cs == "DeviceRGB" || cs == "CalRGB") && operands.size() >= 3) {
      return rgb_to_hex(std::stod(operands[0]), std::stod(operands[1]), std::stod(operands[2]));
    } else if (cs == "DeviceCMYK") {
      if (operands.size() >= 4) {
        return cmyk_to_hex(std::stod(operands[0]), std::stod(operands[1]),
                           std::stod(operands[2]), std::stod(operands[3]));
      }
    } else if (cs == "ICCBased" || cs == "Indexed" || cs == "Separation" || cs == "DeviceN") {
      // Best effort: interpret based on operand count
      if (operands.size() >= 4) {
        return cmyk_to_hex(std::stod(operands[0]), std::stod(operands[1]),
                           std::stod(operands[2]), std::stod(operands[3]));
      } else if (operands.size() >= 3) {
        return rgb_to_hex(std::stod(operands[0]), std::stod(operands[1]), std::stod(operands[2]));
      } else if (operands.size() >= 1) {
        double gray = std::stod(operands[0]);
        return rgb_to_hex(gray, gray, gray);
      }
    } else {
      // Unknown color space - guess from operand count
      if (operands.size() >= 4) {
        return cmyk_to_hex(std::stod(operands[0]), std::stod(operands[1]),
                           std::stod(operands[2]), std::stod(operands[3]));
      } else if (operands.size() >= 3) {
        return rgb_to_hex(std::stod(operands[0]), std::stod(operands[1]), std::stod(operands[2]));
      } else if (operands.size() >= 1) {
        double gray = std::stod(operands[0]);
        return rgb_to_hex(gray, gray, gray);
      }
    }
  } catch (...) {}
  return "";
}

// Canvas mode: Do operator - render Form XObjects
void CanvasExporter::handle_xobject_command(const std::vector<std::string>& operands) {
  if (operands.empty() || !current_pdf_) return;

  std::string name = operands.back();
  if (!name.empty() && name.front() == '/') name.erase(0, 1);

  // Try as Image XObject first
  auto img_it = image_xobjects_.find(name);
  if (img_it != image_xobjects_.end()) {
    // Image rendering - delegate to existing canvas image pipeline
    const auto& image = img_it->second;
    std::vector<uint8_t> pixels;
    int components = 0;
    if (prepare_image_pixels(image, pixels, components)) {
      // Encode to PNG data URL and emit drawImage
      std::vector<uint8_t> png_data;
      int w = image.width;
      int h = image.height;
      if (components == 1) {
        // Expand grayscale to RGB for PNG
        std::vector<uint8_t> rgb(w * h * 3);
        for (int i = 0; i < w * h; ++i) {
          rgb[i * 3] = rgb[i * 3 + 1] = rgb[i * 3 + 2] = pixels[i];
        }
        pixels = std::move(rgb);
        components = 3;
      }
      // Use stb to write PNG
      int stride = w * components;
      auto write_func = [](void* ctx, void* data, int size) {
        auto* out = static_cast<std::vector<uint8_t>*>(ctx);
        auto* bytes = static_cast<uint8_t*>(data);
        out->insert(out->end(), bytes, bytes + size);
      };
      stbi_write_png_to_func(write_func, &png_data, w, h, components,
                              pixels.data(), stride);
      if (!png_data.empty()) {
        std::string data_url = "data:image/png;base64," + base64_encode(png_data);
        queue_canvas_image(
          static_cast<double>(w), 0.0, 0.0, static_cast<double>(h),
          0.0, 0.0, data_url);
      }
    }
    return;
  }

  // Try as Form XObject
  const auto& pages = current_pdf_->catalog.pages;
  for (const auto& page : pages) {
    auto xobj_it = page.resources.find("XObject");
    if (xobj_it == page.resources.end() || xobj_it->second.type != Value::DICTIONARY)
      continue;
    auto form_it = xobj_it->second.dict.find(name);
    if (form_it == xobj_it->second.dict.end()) continue;

    const Value& xobj_ref = form_it->second;
    Value resolved;
    if (xobj_ref.type == Value::REFERENCE) {
      ResolvedObject res = resolve_reference(*current_pdf_, xobj_ref.ref_object_number,
                                              xobj_ref.ref_generation_number);
      if (!res.success) continue;
      resolved = std::move(res.value);
    } else {
      resolved = xobj_ref;
    }
    if (resolved.type != Value::STREAM) continue;

    auto subtype_it = resolved.stream.dict.find("Subtype");
    if (subtype_it == resolved.stream.dict.end() ||
        subtype_it->second.type != Value::NAME ||
        subtype_it->second.name != "Form")
      continue;

    uint32_t obj_num = (xobj_ref.type == Value::REFERENCE) ? xobj_ref.ref_object_number : 0;
    uint16_t gen_num = (xobj_ref.type == Value::REFERENCE) ? xobj_ref.ref_generation_number : 0;
    DecodedStream decoded = decode_stream(*current_pdf_, resolved, obj_num, gen_num);
    if (!decoded.success || decoded.data.empty()) continue;

    // Save state
    add_canvas_command("save");
    graphics_state_stack_.push_back(state_);

    // Apply transparency group attributes
    auto group_it = resolved.stream.dict.find("Group");
    if (group_it != resolved.stream.dict.end()) {
      const Value* group_val = &group_it->second;
      Value group_resolved;
      if (group_val->type == Value::REFERENCE) {
        ResolvedObject gr = resolve_reference(*current_pdf_, group_val->ref_object_number,
                                               group_val->ref_generation_number);
        if (gr.success) {
          group_resolved = std::move(gr.value);
          group_val = &group_resolved;
        }
      }
      // Transparency group opacity
      double opacity = std::min(state_.fill_alpha, state_.stroke_alpha);
      if (std::abs(opacity - 1.0) > 1e-6) {
        add_canvas_command("globalAlpha", {double_to_string(opacity)});
      }
    }

    // Apply Form XObject matrix
    auto matrix_it = resolved.stream.dict.find("Matrix");
    if (matrix_it != resolved.stream.dict.end() &&
        matrix_it->second.type == Value::ARRAY &&
        matrix_it->second.array.size() >= 6) {
      const auto& m = matrix_it->second.array;
      if (m[0].type == Value::NUMBER && m[1].type == Value::NUMBER &&
          m[2].type == Value::NUMBER && m[3].type == Value::NUMBER &&
          m[4].type == Value::NUMBER && m[5].type == Value::NUMBER) {
        add_canvas_command("transform", {
          double_to_string(m[0].number), double_to_string(m[1].number),
          double_to_string(m[2].number), double_to_string(m[3].number),
          double_to_string(m[4].number), double_to_string(m[5].number)
        });
      }
    }

    // Load Form XObject resources
    auto res_it = resolved.stream.dict.find("Resources");
    if (res_it != resolved.stream.dict.end()) {
      const Value* res_val = &res_it->second;
      Value res_resolved;
      if (res_val->type == Value::REFERENCE) {
        ResolvedObject rr = resolve_reference(*current_pdf_, res_val->ref_object_number,
                                               res_val->ref_generation_number);
        if (rr.success) {
          res_resolved = std::move(rr.value);
          res_val = &res_resolved;
        }
      }
      if (res_val->type == Value::DICTIONARY) {
        auto form_images = parse_xobject_resources(*current_pdf_, res_val->dict);
        for (auto& img_pair : form_images) {
          image_xobjects_[img_pair.first] = std::move(img_pair.second);
        }
        auto gs_it = res_val->dict.find("ExtGState");
        if (gs_it != res_val->dict.end() && gs_it->second.type == Value::DICTIONARY) {
          for (const auto& gs_entry : gs_it->second.dict) {
            const Value* gs_val = &gs_entry.second;
            Value gs_resolved;
            if (gs_val->type == Value::REFERENCE) {
              ResolvedObject gr = resolve_reference(*current_pdf_, gs_val->ref_object_number,
                                                     gs_val->ref_generation_number);
              if (gr.success) {
                gs_resolved = std::move(gr.value);
                gs_val = &gs_resolved;
              }
            }
            if (gs_val->type == Value::DICTIONARY) {
              ext_gstates_[gs_entry.first] = parse_ext_gstate(*current_pdf_, gs_val->dict);
            }
          }
        }
        parse_shading_resources(*current_pdf_, res_val->dict);
        parse_pattern_resources(*current_pdf_, res_val->dict);
      }
    }

    // Parse Form content
    TokenizedStream form_stream = tokenize_content_stream(decoded.data);
    for (auto& img : form_stream.inline_images) {
      inline_images_.push_back(std::move(img));
    }
    parse_content_stream_from_tokens(form_stream.tokens);

    // Restore state
    if (!graphics_state_stack_.empty()) {
      state_ = graphics_state_stack_.back();
      graphics_state_stack_.pop_back();
    }
    add_canvas_command("restore");
    return;
  }
}

// Canvas mode: sh operator - paint shading directly
void CanvasExporter::handle_shading_command(const std::vector<std::string>& operands) {
  if (operands.empty() || !current_pdf_) return;

  std::string name = operands.back();
  if (!name.empty() && name.front() == '/') name.erase(0, 1);

  auto it = shading_resources_.find(name);
  if (it == shading_resources_.end()) return;

  // Use create_svg_gradient to parse the shading and extract gradient data,
  // then emit equivalent canvas gradient commands
  size_t grad_idx = svg_gradients_.size();
  std::string grad_ref = create_svg_gradient(it->second, state_.ctm);
  if (grad_ref.empty() || svg_gradients_.size() <= grad_idx) return;

  const SvgGradient& grad = svg_gradients_.back();

  add_canvas_command("save");

  // Create the gradient
  std::string grad_var = "g" + std::to_string(svg_gradient_counter_);
  if (grad.is_radial) {
    add_canvas_command("createRadialGradient", {
      double_to_string(grad.fx), double_to_string(grad.fy), "0",
      double_to_string(grad.cx), double_to_string(grad.cy), double_to_string(grad.r)
    });
  } else {
    add_canvas_command("createLinearGradient", {
      double_to_string(grad.x1), double_to_string(grad.y1),
      double_to_string(grad.x2), double_to_string(grad.y2)
    });
  }

  // Add color stops
  for (const auto& stop : grad.stops) {
    add_canvas_command("addColorStop", {
      double_to_string(stop.offset),
      canvas_color_string(stop.color, stop.opacity)
    });
  }

  // Fill the page with the gradient
  add_canvas_command("fillGradient");
  add_canvas_command("fillRect", {"0", "0", double_to_string(page_width_), double_to_string(page_height_)});

  add_canvas_command("restore");
}

// SVG mode: sh operator - paint shading as a filled rect with gradient
void CanvasExporter::handle_shading_command_svg(const std::vector<std::string>& operands) {
  if (ocg_skip_depth_ > 0) return;
  if (operands.empty() || !current_pdf_) return;

  std::string name = operands.back();
  if (!name.empty() && name.front() == '/') name.erase(0, 1);

  auto it = shading_resources_.find(name);
  if (it == shading_resources_.end()) return;

  std::string fill_ref = create_svg_gradient(it->second, state_.ctm);
  if (fill_ref.empty()) return;

  // Emit a rect covering the page filled with the gradient
  std::map<std::string, std::string> attrs;
  attrs["x"] = "0";
  attrs["y"] = "0";
  attrs["width"] = double_to_string(page_width_);
  attrs["height"] = double_to_string(page_height_);
  attrs["fill"] = fill_ref;
  if (std::abs(state_.fill_alpha - 1.0) > 1e-6) {
    attrs["fill-opacity"] = double_to_string(state_.fill_alpha);
  }
  std::string css_mode = blend_mode_to_css(state_.blend_mode);
  if (!css_mode.empty()) {
    attrs["style"] = "mix-blend-mode:" + css_mode + ';';
  }
  add_svg_element("rect", attrs);
}

void CanvasExporter::handle_xobject_command_svg(const std::vector<std::string>& operands) {
  // Skip rendering when inside a hidden OCG layer
  if (ocg_skip_depth_ > 0) return;

  if (operands.empty()) {
    return;
  }

  std::string name = operands.back();
  if (!name.empty() && name.front() == '/') {
    name = name.substr(1);
  }

  // Try as Image XObject first
  auto it = image_xobjects_.find(name);
  if (it != image_xobjects_.end()) {
    emit_svg_image(name, it->second);
    return;
  }

  // Try as Form XObject (transparency groups, reusable content)
  if (!current_pdf_) return;

  // Look up the XObject in the page resources
  // We need to find the Form XObject stream and render its content
  const auto& pages = current_pdf_->catalog.pages;
  for (const auto& page : pages) {
    auto xobj_it = page.resources.find("XObject");
    if (xobj_it == page.resources.end() || xobj_it->second.type != Value::DICTIONARY) {
      continue;
    }
    auto form_it = xobj_it->second.dict.find(name);
    if (form_it == xobj_it->second.dict.end()) continue;

    const Value& xobj_ref = form_it->second;
    Value resolved;
    if (xobj_ref.type == Value::REFERENCE) {
      ResolvedObject res = resolve_reference(*current_pdf_, xobj_ref.ref_object_number,
                                              xobj_ref.ref_generation_number);
      if (!res.success) continue;
      resolved = std::move(res.value);
    } else {
      resolved = xobj_ref;
    }

    if (resolved.type != Value::STREAM) continue;

    auto subtype_it = resolved.stream.dict.find("Subtype");
    if (subtype_it == resolved.stream.dict.end() ||
        subtype_it->second.type != Value::NAME ||
        subtype_it->second.name != "Form") {
      continue;
    }

    // Decode the Form XObject content stream
    uint32_t obj_num = (xobj_ref.type == Value::REFERENCE) ? xobj_ref.ref_object_number : 0;
    uint16_t gen_num = (xobj_ref.type == Value::REFERENCE) ? xobj_ref.ref_generation_number : 0;
    DecodedStream decoded = decode_stream(*current_pdf_, resolved, obj_num, gen_num);
    if (!decoded.success || decoded.data.empty()) continue;

    // Build transparency group attributes for wrapping <g>
    bool has_group = false;
    bool isolated = false;
    std::string group_blend;
    auto group_it = resolved.stream.dict.find("Group");
    if (group_it != resolved.stream.dict.end()) {
      const Value* group_val = &group_it->second;
      Value group_resolved;
      if (group_val->type == Value::REFERENCE) {
        ResolvedObject gr = resolve_reference(*current_pdf_, group_val->ref_object_number,
                                               group_val->ref_generation_number);
        if (gr.success) {
          group_resolved = std::move(gr.value);
          group_val = &group_resolved;
        }
      }
      if (group_val->type == Value::DICTIONARY) {
        has_group = true;
        auto iso_it = group_val->dict.find("I");
        if (iso_it != group_val->dict.end() && iso_it->second.type == Value::BOOLEAN) {
          isolated = iso_it->second.boolean;
        }
      }
    }

    // Emit a wrapping <g> element with opacity and blend mode
    std::map<std::string, std::string> g_attrs;
    double opacity = std::min(state_.fill_alpha, state_.stroke_alpha);
    if (std::abs(opacity - 1.0) > 1e-6) {
      g_attrs["opacity"] = double_to_string(opacity);
    }
    std::string css_mode = blend_mode_to_css(state_.blend_mode);
    if (!css_mode.empty()) {
      std::string style = "mix-blend-mode:" + css_mode;
      if (isolated || has_group) {
        style += ";isolation:isolate";
      }
      g_attrs["style"] = style;
    } else if (isolated || has_group) {
      g_attrs["style"] = "isolation:isolate";
    }

    // Apply Form XObject matrix if present
    auto matrix_it = resolved.stream.dict.find("Matrix");
    if (matrix_it != resolved.stream.dict.end() &&
        matrix_it->second.type == Value::ARRAY &&
        matrix_it->second.array.size() >= 6) {
      const auto& m = matrix_it->second.array;
      if (m[0].type == Value::NUMBER && m[1].type == Value::NUMBER &&
          m[2].type == Value::NUMBER && m[3].type == Value::NUMBER &&
          m[4].type == Value::NUMBER && m[5].type == Value::NUMBER) {
        std::stringstream ss;
        ss << "matrix(" << double_to_string(m[0].number) << " "
           << double_to_string(m[1].number) << " "
           << double_to_string(m[2].number) << " "
           << double_to_string(m[3].number) << " "
           << double_to_string(m[4].number) << " "
           << double_to_string(m[5].number) << ")";
        g_attrs["transform"] = ss.str();
      }
    }

    if (!g_attrs.empty()) {
      add_svg_element("g", g_attrs);
    }

    // Save graphics state and parse the Form XObject content
    graphics_state_stack_.push_back(state_);

    // Load any resources from the Form XObject
    auto res_it = resolved.stream.dict.find("Resources");
    if (res_it != resolved.stream.dict.end()) {
      const Value* res_val = &res_it->second;
      Value res_resolved;
      if (res_val->type == Value::REFERENCE) {
        ResolvedObject rr = resolve_reference(*current_pdf_, res_val->ref_object_number,
                                               res_val->ref_generation_number);
        if (rr.success) {
          res_resolved = std::move(rr.value);
          res_val = &res_resolved;
        }
      }
      if (res_val->type == Value::DICTIONARY) {
        // Load image XObjects from Form's resources
        auto form_images = parse_xobject_resources(*current_pdf_, res_val->dict);
        for (auto& img_pair : form_images) {
          image_xobjects_[img_pair.first] = std::move(img_pair.second);
        }
        // Load ExtGState resources
        auto gs_it = res_val->dict.find("ExtGState");
        if (gs_it != res_val->dict.end() && gs_it->second.type == Value::DICTIONARY) {
          for (const auto& gs_entry : gs_it->second.dict) {
            const Value* gs_val = &gs_entry.second;
            Value gs_resolved;
            if (gs_val->type == Value::REFERENCE) {
              ResolvedObject gr = resolve_reference(*current_pdf_, gs_val->ref_object_number,
                                                     gs_val->ref_generation_number);
              if (gr.success) {
                gs_resolved = std::move(gr.value);
                gs_val = &gs_resolved;
              }
            }
            if (gs_val->type == Value::DICTIONARY) {
              ext_gstates_[gs_entry.first] = parse_ext_gstate(*current_pdf_, gs_val->dict);
            }
          }
        }
      }
    }

    // Tokenize and parse the content stream
    TokenizedStream form_stream = tokenize_content_stream(decoded.data);
    // Store any inline images from the Form XObject
    for (auto& img : form_stream.inline_images) {
      inline_images_.push_back(std::move(img));
    }
    parse_content_stream_for_svg_from_tokens(form_stream.tokens);

    // Restore graphics state
    if (!graphics_state_stack_.empty()) {
      state_ = graphics_state_stack_.back();
      graphics_state_stack_.pop_back();
    }

    // Close wrapping <g>
    if (!g_attrs.empty()) {
      add_svg_element("/g");
    }
    break;
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

  // Add defs section for gradients and patterns
  if (!svg_gradients_.empty() || !svg_patterns_.empty()) {
    svg << "  <defs>\n";

    // Output gradients
    for (const auto& grad : svg_gradients_) {
      if (grad.is_radial) {
        svg << "    <radialGradient id=\"" << grad.id << "\"";
        svg << " cx=\"" << double_to_string(grad.cx) << "\"";
        svg << " cy=\"" << double_to_string(grad.cy) << "\"";
        svg << " r=\"" << double_to_string(grad.r) << "\"";
        svg << " fx=\"" << double_to_string(grad.fx) << "\"";
        svg << " fy=\"" << double_to_string(grad.fy) << "\"";
        svg << " gradientUnits=\"" << grad.units << "\"";
        if (!grad.transform.empty()) {
          svg << " gradientTransform=\"" << grad.transform << "\"";
        }
        svg << ">\n";
      } else {
        svg << "    <linearGradient id=\"" << grad.id << "\"";
        svg << " x1=\"" << double_to_string(grad.x1) << "\"";
        svg << " y1=\"" << double_to_string(grad.y1) << "\"";
        svg << " x2=\"" << double_to_string(grad.x2) << "\"";
        svg << " y2=\"" << double_to_string(grad.y2) << "\"";
        svg << " gradientUnits=\"" << grad.units << "\"";
        if (!grad.transform.empty()) {
          svg << " gradientTransform=\"" << grad.transform << "\"";
        }
        svg << ">\n";
      }

      for (const auto& stop : grad.stops) {
        svg << "      <stop offset=\"" << double_to_string(stop.offset * 100) << "%\"";
        svg << " stop-color=\"" << stop.color << "\"";
        if (std::abs(stop.opacity - 1.0) > 1e-6) {
          svg << " stop-opacity=\"" << double_to_string(stop.opacity) << "\"";
        }
        svg << "/>\n";
      }

      if (grad.is_radial) {
        svg << "    </radialGradient>\n";
      } else {
        svg << "    </linearGradient>\n";
      }
    }

    // Output patterns
    for (const auto& pat : svg_patterns_) {
      svg << "    <pattern id=\"" << pat.id << "\"";
      svg << " x=\"" << double_to_string(pat.x) << "\"";
      svg << " y=\"" << double_to_string(pat.y) << "\"";
      svg << " width=\"" << double_to_string(pat.width) << "\"";
      svg << " height=\"" << double_to_string(pat.height) << "\"";
      svg << " patternUnits=\"" << pat.units << "\"";
      svg << " patternContentUnits=\"" << pat.content_units << "\"";
      if (!pat.transform.empty()) {
        svg << " patternTransform=\"" << pat.transform << "\"";
      }
      svg << ">\n";

      for (const auto& elem : pat.content) {
        svg << "      <" << elem.element_type;
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

      svg << "    </pattern>\n";
    }

    svg << "  </defs>\n";
  }

  for (const SvgElement& elem : elements) {
    // Handle closing tags (element_type starts with '/')
    if (!elem.element_type.empty() && elem.element_type[0] == '/') {
      svg << "  <" << elem.element_type << ">\n";
      continue;
    }

    svg << "  <" << elem.element_type;
    for (const auto& attr : elem.attributes) {
      svg << " " << attr.first << "=\"" << escape_xml_string(attr.second) << "\"";
    }

    // Elements that can contain children (g, clipPath, etc.) should not self-close
    bool is_container = (elem.element_type == "g" || elem.element_type == "clipPath" ||
                         elem.element_type == "defs" || elem.element_type == "svg");
    if (!elem.text_content.empty()) {
      svg << ">" << escape_xml_string(elem.text_content)
          << "</" << elem.element_type << ">\n";
    } else if (is_container) {
      svg << ">\n";
    } else {
      svg << " />\n";
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

// Parse shading resources from page resources
void CanvasExporter::parse_shading_resources(const Pdf& pdf, const Dictionary& resources) {
  auto shading_it = resources.find("Shading");
  if (shading_it == resources.end()) return;

  const Value& shading_dict_val = shading_it->second;
  if (shading_dict_val.type == Value::REFERENCE) {
    auto resolved = resolve_reference(pdf, shading_dict_val.ref_object_number,
                                       shading_dict_val.ref_generation_number);
    if (resolved.success && resolved.value.type == Value::DICTIONARY) {
      for (const auto& entry : resolved.value.dict) {
        shading_resources_[entry.first] = entry.second;
      }
    }
  } else if (shading_dict_val.type == Value::DICTIONARY) {
    for (const auto& entry : shading_dict_val.dict) {
      shading_resources_[entry.first] = entry.second;
    }
  }
}

// Parse pattern resources from page resources
void CanvasExporter::parse_pattern_resources(const Pdf& pdf, const Dictionary& resources) {
  auto pattern_it = resources.find("Pattern");
  if (pattern_it == resources.end()) return;

  const Value& pattern_dict_val = pattern_it->second;
  if (pattern_dict_val.type == Value::REFERENCE) {
    auto resolved = resolve_reference(pdf, pattern_dict_val.ref_object_number,
                                       pattern_dict_val.ref_generation_number);
    if (resolved.success && resolved.value.type == Value::DICTIONARY) {
      for (const auto& entry : resolved.value.dict) {
        pattern_resources_[entry.first] = entry.second;
      }
    }
  } else if (pattern_dict_val.type == Value::DICTIONARY) {
    for (const auto& entry : pattern_dict_val.dict) {
      pattern_resources_[entry.first] = entry.second;
    }
  }
}

// Create SVG gradient from PDF shading dictionary
std::string CanvasExporter::create_svg_gradient(const Value& shading_val, const Matrix2D& ctm) {
  if (!current_pdf_) return "";

  Value shading = shading_val;
  if (shading.type == Value::REFERENCE) {
    auto resolved = resolve_reference(*current_pdf_, shading.ref_object_number,
                                       shading.ref_generation_number);
    if (!resolved.success) return "";
    shading = resolved.value;
  }

  if (shading.type != Value::DICTIONARY && shading.type != Value::STREAM) {
    return "";
  }

  const Dictionary& dict = (shading.type == Value::STREAM)
                               ? shading.stream.dict
                               : shading.dict;

  // Get shading type
  auto type_it = dict.find("ShadingType");
  if (type_it == dict.end() || type_it->second.type != Value::NUMBER) {
    return "";
  }
  int shading_type = static_cast<int>(type_it->second.number);

  // Only support Type 2 (linear) and Type 3 (radial)
  if (shading_type != 2 && shading_type != 3) {
    return "";
  }

  SvgGradient gradient;
  gradient.id = "grad_" + std::to_string(++svg_gradient_counter_);
  gradient.is_radial = (shading_type == 3);
  gradient.units = "userSpaceOnUse";

  // Get coords
  auto coords_it = dict.find("Coords");
  if (coords_it == dict.end() || coords_it->second.type != Value::ARRAY) {
    return "";
  }

  const auto& coords = coords_it->second.array;
  if (shading_type == 2 && coords.size() >= 4) {
    // Linear gradient: x0, y0, x1, y1
    double gx1 = 0, gy1 = 0, gx2 = 0, gy2 = 0;
    if (coords[0].type == Value::NUMBER) gx1 = coords[0].number;
    if (coords[1].type == Value::NUMBER) gy1 = coords[1].number;
    if (coords[2].type == Value::NUMBER) gx2 = coords[2].number;
    if (coords[3].type == Value::NUMBER) gy2 = coords[3].number;

    // Apply CTM transform to gradient coordinates
    apply_matrix_to_point(ctm, gx1, gy1);
    apply_matrix_to_point(ctm, gx2, gy2);

    gradient.x1 = gx1;
    gradient.y1 = page_height_ - gy1;
    gradient.x2 = gx2;
    gradient.y2 = page_height_ - gy2;
  } else if (shading_type == 3 && coords.size() >= 6) {
    // Radial gradient: x0, y0, r0, x1, y1, r1
    double x0 = 0, y0 = 0, r0 = 0, x1 = 0, y1 = 0, r1 = 0;
    if (coords[0].type == Value::NUMBER) x0 = coords[0].number;
    if (coords[1].type == Value::NUMBER) y0 = coords[1].number;
    if (coords[2].type == Value::NUMBER) r0 = coords[2].number;
    if (coords[3].type == Value::NUMBER) x1 = coords[3].number;
    if (coords[4].type == Value::NUMBER) y1 = coords[4].number;
    if (coords[5].type == Value::NUMBER) r1 = coords[5].number;

    // Apply CTM transform to gradient center and focal points
    apply_matrix_to_point(ctm, x0, y0);
    apply_matrix_to_point(ctm, x1, y1);

    // Scale radius by the average scale factor of the CTM
    double scale_x = std::sqrt(ctm.a * ctm.a + ctm.b * ctm.b);
    double scale_y = std::sqrt(ctm.c * ctm.c + ctm.d * ctm.d);
    double scale = (scale_x + scale_y) * 0.5;

    // SVG radial gradient uses outer circle (cx, cy, r) and focal point (fx, fy)
    gradient.cx = x1;
    gradient.cy = page_height_ - y1;
    gradient.r = r1 * scale;
    gradient.fx = x0;
    gradient.fy = page_height_ - y0;
    (void)r0;  // Inner radius not directly supported in SVG
  }

  // Determine color space components
  auto color_space_it = dict.find("ColorSpace");
  int num_components = 3;  // Default RGB
  if (color_space_it != dict.end()) {
    if (color_space_it->second.type == Value::NAME) {
      const std::string& cs_name = color_space_it->second.name;
      if (cs_name == "DeviceGray") num_components = 1;
      else if (cs_name == "DeviceCMYK") num_components = 4;
    }
  }

  // Helper: convert color components to hex based on component count
  auto components_to_hex = [this](const std::vector<double>& c, int nc) -> std::string {
    if (nc == 1 && c.size() >= 1) {
      return rgb_to_hex(c[0], c[0], c[0]);
    } else if (nc == 4 && c.size() >= 4) {
      return cmyk_to_hex(c[0], c[1], c[2], c[3]);
    } else if (c.size() >= 3) {
      return rgb_to_hex(c[0], c[1], c[2]);
    } else if (c.size() >= 1) {
      return rgb_to_hex(c[0], c[0], c[0]);
    }
    return "#000000";
  };

  // Helper: resolve a function Value (dereference if needed)
  auto resolve_func = [this](const Value& v) -> Value {
    if (v.type == Value::REFERENCE) {
      auto res = resolve_reference(*current_pdf_, v.ref_object_number, v.ref_generation_number);
      if (res.success) return res.value;
    }
    return v;
  };

  // Helper: extract Type 2 C0/C1 colors from an exponential function
  auto extract_type2_colors = [&](const Dictionary& fd,
                                   std::vector<double>& c0_out,
                                   std::vector<double>& c1_out) {
    auto c0_it = fd.find("C0");
    if (c0_it != fd.end() && c0_it->second.type == Value::ARRAY) {
      for (const auto& v : c0_it->second.array)
        if (v.type == Value::NUMBER) c0_out.push_back(v.number);
    }
    auto c1_it = fd.find("C1");
    if (c1_it != fd.end() && c1_it->second.type == Value::ARRAY) {
      for (const auto& v : c1_it->second.array)
        if (v.type == Value::NUMBER) c1_out.push_back(v.number);
    }
    if (c0_out.empty()) c0_out.assign(num_components, 0.0);
    if (c1_out.empty()) c1_out.assign(num_components, 1.0);
  };

  // Get color function and extract gradient stops
  auto function_it = dict.find("Function");
  if (function_it != dict.end()) {
    Value func = resolve_func(function_it->second);
    const Dictionary* fd = nullptr;
    if (func.type == Value::DICTIONARY) {
      fd = &func.dict;
    } else if (func.type == Value::STREAM) {
      fd = &func.stream.dict;
    }

    if (fd) {
      auto func_type_it = fd->find("FunctionType");
      int func_type = (func_type_it != fd->end() && func_type_it->second.type == Value::NUMBER)
                          ? static_cast<int>(func_type_it->second.number) : -1;

      if (func_type == 2) {
        // Type 2: Exponential interpolation - two stops
        std::vector<double> c0, c1;
        extract_type2_colors(*fd, c0, c1);
        gradient.stops.push_back({0.0, components_to_hex(c0, num_components), 1.0});
        gradient.stops.push_back({1.0, components_to_hex(c1, num_components), 1.0});
      } else if (func_type == 3) {
        // Type 3: Stitching function - multi-stop gradient
        auto bounds_it = fd->find("Bounds");
        auto funcs_it = fd->find("Functions");
        auto encode_it = fd->find("Encode");

        if (funcs_it != fd->end() && funcs_it->second.type == Value::ARRAY) {
          const auto& sub_funcs = funcs_it->second.array;
          size_t n = sub_funcs.size();

          // Get domain (default 0..1)
          double domain_min = 0.0, domain_max = 1.0;
          auto domain_it = fd->find("Domain");
          if (domain_it != fd->end() && domain_it->second.type == Value::ARRAY &&
              domain_it->second.array.size() >= 2) {
            if (domain_it->second.array[0].type == Value::NUMBER)
              domain_min = domain_it->second.array[0].number;
            if (domain_it->second.array[1].type == Value::NUMBER)
              domain_max = domain_it->second.array[1].number;
          }
          double domain_range = domain_max - domain_min;
          if (domain_range <= 0.0) domain_range = 1.0;

          // Get bounds array
          std::vector<double> bounds;
          if (bounds_it != fd->end() && bounds_it->second.type == Value::ARRAY) {
            for (const auto& bv : bounds_it->second.array) {
              if (bv.type == Value::NUMBER) bounds.push_back(bv.number);
            }
          }

          // For each sub-function, extract its C0 and C1 and map to gradient offsets
          // Boundaries define: [domain_min, bounds[0], bounds[1], ..., domain_max]
          for (size_t i = 0; i < n; ++i) {
            Value sf = resolve_func(sub_funcs[i]);
            const Dictionary* sfd = nullptr;
            if (sf.type == Value::DICTIONARY) sfd = &sf.dict;
            else if (sf.type == Value::STREAM) sfd = &sf.stream.dict;
            if (!sfd) continue;

            double seg_start = (i == 0) ? domain_min : (i - 1 < bounds.size() ? bounds[i - 1] : domain_min);
            double seg_end = (i < bounds.size()) ? bounds[i] : domain_max;

            // Normalize to 0..1
            double offset_start = (seg_start - domain_min) / domain_range;
            double offset_end = (seg_end - domain_min) / domain_range;

            auto sf_type_it = sfd->find("FunctionType");
            int sf_type = (sf_type_it != sfd->end() && sf_type_it->second.type == Value::NUMBER)
                             ? static_cast<int>(sf_type_it->second.number) : -1;

            if (sf_type == 2) {
              std::vector<double> c0, c1;
              extract_type2_colors(*sfd, c0, c1);
              // Add start stop (avoid duplicating the first stop on subsequent segments)
              if (i == 0 || gradient.stops.empty() ||
                  std::abs(gradient.stops.back().offset - offset_start) > 1e-6) {
                gradient.stops.push_back({offset_start, components_to_hex(c0, num_components), 1.0});
              }
              gradient.stops.push_back({offset_end, components_to_hex(c1, num_components), 1.0});
            } else {
              // Unknown sub-function type: add placeholder stops
              if (gradient.stops.empty()) {
                gradient.stops.push_back({offset_start, "#000000", 1.0});
              }
              gradient.stops.push_back({offset_end, "#000000", 1.0});
            }
          }
        }
      }
    } else if (func.type == Value::ARRAY) {
      // Function array - one function per color component
      // Evaluate at t=0 and t=1 to get endpoint colors
      std::vector<double> c0_vals, c1_vals;
      for (const auto& fn_ref : func.array) {
        Value fn = resolve_func(fn_ref);
        const Dictionary* fnd = nullptr;
        if (fn.type == Value::DICTIONARY) fnd = &fn.dict;
        else if (fn.type == Value::STREAM) fnd = &fn.stream.dict;
        if (!fnd) { c0_vals.push_back(0.0); c1_vals.push_back(1.0); continue; }

        auto ft_it = fnd->find("FunctionType");
        int ft = (ft_it != fnd->end() && ft_it->second.type == Value::NUMBER)
                    ? static_cast<int>(ft_it->second.number) : -1;
        if (ft == 2) {
          std::vector<double> c0, c1;
          extract_type2_colors(*fnd, c0, c1);
          c0_vals.push_back(c0.empty() ? 0.0 : c0[0]);
          c1_vals.push_back(c1.empty() ? 1.0 : c1[0]);
        } else {
          c0_vals.push_back(0.0);
          c1_vals.push_back(1.0);
        }
      }
      gradient.stops.push_back({0.0, components_to_hex(c0_vals, num_components), 1.0});
      gradient.stops.push_back({1.0, components_to_hex(c1_vals, num_components), 1.0});
    }
  }

  if (gradient.stops.empty()) {
    // Add default black-white gradient
    gradient.stops.push_back({0.0, "#000000", 1.0});
    gradient.stops.push_back({1.0, "#ffffff", 1.0});
  }

  svg_gradients_.push_back(gradient);
  return "url(#" + gradient.id + ")";
}

// Create SVG pattern from PDF pattern dictionary
std::string CanvasExporter::create_svg_pattern(const Value& pattern_val, const Matrix2D& ctm) {
  if (!current_pdf_) return "";

  Value pattern = pattern_val;
  if (pattern.type == Value::REFERENCE) {
    auto resolved = resolve_reference(*current_pdf_, pattern.ref_object_number,
                                       pattern.ref_generation_number);
    if (!resolved.success) return "";
    pattern = resolved.value;
  }

  if (pattern.type != Value::DICTIONARY && pattern.type != Value::STREAM) {
    return "";
  }

  const Dictionary& dict = (pattern.type == Value::STREAM)
                               ? pattern.stream.dict
                               : pattern.dict;

  // Get pattern type
  auto type_it = dict.find("PatternType");
  if (type_it == dict.end() || type_it->second.type != Value::NUMBER) {
    return "";
  }
  int pattern_type = static_cast<int>(type_it->second.number);

  if (pattern_type == 2) {
    // Shading pattern - create gradient instead
    auto shading_it = dict.find("Shading");
    if (shading_it != dict.end()) {
      return create_svg_gradient(shading_it->second, ctm);
    }
    return "";
  }

  if (pattern_type != 1) {
    return "";  // Only support Type 1 (tiling) patterns
  }

  SvgPattern svg_pattern;
  svg_pattern.id = "pat_" + std::to_string(++svg_pattern_counter_);

  // Get BBox
  auto bbox_it = dict.find("BBox");
  if (bbox_it != dict.end() && bbox_it->second.type == Value::ARRAY &&
      bbox_it->second.array.size() >= 4) {
    const auto& bbox = bbox_it->second.array;
    double llx = 0, lly = 0, urx = 0, ury = 0;
    if (bbox[0].type == Value::NUMBER) llx = bbox[0].number;
    if (bbox[1].type == Value::NUMBER) lly = bbox[1].number;
    if (bbox[2].type == Value::NUMBER) urx = bbox[2].number;
    if (bbox[3].type == Value::NUMBER) ury = bbox[3].number;

    svg_pattern.x = llx;
    svg_pattern.y = lly;
    svg_pattern.width = urx - llx;
    svg_pattern.height = ury - lly;
  }

  // Get XStep and YStep
  auto xstep_it = dict.find("XStep");
  auto ystep_it = dict.find("YStep");
  if (xstep_it != dict.end() && xstep_it->second.type == Value::NUMBER) {
    svg_pattern.width = std::abs(xstep_it->second.number);
  }
  if (ystep_it != dict.end() && ystep_it->second.type == Value::NUMBER) {
    svg_pattern.height = std::abs(ystep_it->second.number);
  }

  // Get pattern matrix and combine with CTM
  Matrix2D pattern_matrix;  // Identity by default
  auto matrix_it = dict.find("Matrix");
  if (matrix_it != dict.end() && matrix_it->second.type == Value::ARRAY &&
      matrix_it->second.array.size() >= 6) {
    const auto& m = matrix_it->second.array;
    if (m[0].type == Value::NUMBER) pattern_matrix.a = m[0].number;
    if (m[1].type == Value::NUMBER) pattern_matrix.b = m[1].number;
    if (m[2].type == Value::NUMBER) pattern_matrix.c = m[2].number;
    if (m[3].type == Value::NUMBER) pattern_matrix.d = m[3].number;
    if (m[4].type == Value::NUMBER) pattern_matrix.e = m[4].number;
    if (m[5].type == Value::NUMBER) pattern_matrix.f = m[5].number;
  }

  // Combine pattern matrix with CTM: final = CTM * patternMatrix
  // Then apply PDF-to-SVG Y-flip
  Matrix2D combined = multiply_matrices(ctm, pattern_matrix);
  Matrix2D y_flip;
  y_flip.a = 1.0; y_flip.b = 0.0;
  y_flip.c = 0.0; y_flip.d = -1.0;
  y_flip.e = 0.0; y_flip.f = page_height_;
  combined = multiply_matrices(y_flip, combined);

  {
    std::stringstream transform;
    transform << "matrix("
              << double_to_string(combined.a) << " "
              << double_to_string(combined.b) << " "
              << double_to_string(combined.c) << " "
              << double_to_string(combined.d) << " "
              << double_to_string(combined.e) << " "
              << double_to_string(combined.f) << ")";
    svg_pattern.transform = transform.str();
  }

  // For tiling patterns, add a simple rectangle as placeholder
  // A full implementation would parse the pattern's content stream
  SvgElement rect("rect");
  rect.attributes["x"] = "0";
  rect.attributes["y"] = "0";
  rect.attributes["width"] = double_to_string(svg_pattern.width);
  rect.attributes["height"] = double_to_string(svg_pattern.height);
  rect.attributes["fill"] = "#808080";
  rect.attributes["fill-opacity"] = "0.5";
  svg_pattern.content.push_back(rect);

  svg_patterns_.push_back(svg_pattern);
  return "url(#" + svg_pattern.id + ")";
}

}  // namespace nanopdf
