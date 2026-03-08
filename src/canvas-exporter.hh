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
#include <map>
#include <memory>
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

// SVG gradient definition
struct SvgGradient {
  std::string id;
  bool is_radial{false};
  // Linear: x1, y1, x2, y2
  // Radial: cx, cy, r, fx, fy
  double x1{0.0}, y1{0.0}, x2{1.0}, y2{0.0};
  double cx{0.5}, cy{0.5}, r{0.5}, fx{0.5}, fy{0.5};
  struct Stop {
    double offset;  // 0.0-1.0
    std::string color;
    double opacity{1.0};
  };
  std::vector<Stop> stops;
  std::string transform;  // Optional transform attribute
  std::string units{"objectBoundingBox"};  // or "userSpaceOnUse"
};

// SVG pattern definition
struct SvgPattern {
  std::string id;
  double x{0.0}, y{0.0};
  double width{0.0}, height{0.0};
  std::string units{"userSpaceOnUse"};
  std::string content_units{"userSpaceOnUse"};
  std::string transform;
  std::vector<SvgElement> content;
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
  std::string svg_to_markup(const std::vector<SvgElement>& elements) const;

private:
  struct Matrix2D {
    double a{1.0};
    double b{0.0};
    double c{0.0};
    double d{1.0};
    double e{0.0};
    double f{0.0};
  };

  void reset_state();
  void parse_content_stream_from_tokens(const std::vector<std::string>& tokens);
  void parse_content_stream_for_svg_from_tokens(const std::vector<std::string>& tokens);
  void process_pdf_command(const std::string& operator_name, 
                         const std::vector<std::string>& operands);
  
  void handle_text_command(const std::string& op, const std::vector<std::string>& operands);
  void handle_path_command(const std::string& op, const std::vector<std::string>& operands);
  void handle_graphics_state_command(const std::string& op, const std::vector<std::string>& operands);
  void handle_color_command(const std::string& op, const std::vector<std::string>& operands);
  void handle_xobject_command(const std::vector<std::string>& operands);
  void handle_shading_command(const std::vector<std::string>& operands);
  void apply_pending_clip();
  void apply_pending_clip_svg();

  void handle_text_command_svg(const std::string& op, const std::vector<std::string>& operands);
  void handle_path_command_svg(const std::string& op, const std::vector<std::string>& operands);
  void handle_graphics_state_command_svg(const std::string& op, const std::vector<std::string>& operands);
  void handle_color_command_svg(const std::string& op, const std::vector<std::string>& operands);
  void handle_xobject_command_svg(const std::vector<std::string>& operands);
  void handle_shading_command_svg(const std::vector<std::string>& operands);
  
  void add_canvas_command(const std::string& command);
  void add_canvas_command(const std::string& command, const std::vector<std::string>& args);
  void add_svg_element(const std::string& element_type, const std::map<std::string, std::string>& attributes = {});
  void add_svg_element(const std::string& element_type, const std::map<std::string, std::string>& attributes, const std::string& text_content);
  
  void transform_coordinates(double& x, double& y) const;
  void transform_coordinates_svg(double& x, double& y) const;
  std::string double_to_string(double value) const;
  std::string escape_json_string(const std::string& str) const;
  std::string escape_xml_string(const std::string& str) const;
  std::string rgb_to_hex(double r, double g, double b) const;
  std::string cmyk_to_hex(double c, double m, double y, double k) const;
  std::string canvas_color_string(const std::string& hex, double alpha) const;
  std::string resolve_scn_color(const std::vector<std::string>& operands, bool is_stroking);
  std::string decode_pdf_text_for_display(const std::string& str) const;
  std::string decode_pdf_text_with_font(const std::string& str) const;
  std::string resolve_font_family(const std::string& resource_name) const;
  bool is_vertical_font(const std::string& resource_name) const;
  double compute_string_width(const std::string& raw_str) const;
  void advance_text_matrix(const std::string& raw_str);
  void advance_text_matrix_tj(const std::vector<std::string>& operands);
  void update_canvas_fill_style();
  void update_canvas_stroke_style();
  void update_canvas_line_width();
  void update_canvas_line_cap();
  void update_canvas_line_join();
  void update_canvas_miter_limit();
  void update_canvas_line_dash();
  void update_canvas_blend_mode();
  std::string blend_mode_to_canvas(BlendMode mode) const;
  std::string blend_mode_to_css(const std::string& canvas_mode) const;
  void emit_svg_path(bool close_path, bool fill, bool stroke, bool use_evenodd);
  Matrix2D multiply_matrices(const Matrix2D& lhs, const Matrix2D& rhs) const;
  void apply_matrix_to_point(const Matrix2D& matrix, double& x, double& y) const;
  std::string dash_pattern_to_string(const std::vector<double>& pattern) const;
  std::string dash_pattern_to_canvas_array(const std::vector<double>& pattern) const;
  void parse_dash_pattern(const std::vector<std::string>& operands,
                          std::vector<double>* pattern,
                          double* phase) const;
  void emit_svg_image(const std::string& name, const ImageXObject& image);
  std::string base64_encode(const std::vector<uint8_t>& data) const;
  bool prepare_image_pixels(const ImageXObject& image,
                            std::vector<uint8_t>& out,
                            int& components) const;
  bool expand_low_bpc_gray(const ImageXObject& image,
                           std::vector<uint8_t>& out) const;
  bool expand_indexed_pixels(const ImageXObject& image,
                             std::vector<uint8_t>& out,
                             int& components) const;
  bool apply_decode_to_buffer(const ImageXObject& image,
                              std::vector<uint8_t>& buffer,
                              int components) const;
  bool convert_cmyk_to_rgb(const ImageXObject& image,
                           std::vector<uint8_t>& out) const;
  bool expand_index_buffer(const std::vector<uint8_t>& data,
                           int width,
                           int height,
                           int bits,
                           std::vector<uint8_t>& indices) const;
  double apply_decode_component(const ImageXObject& image,
                                int component_index,
                                double normalized_value) const;
  bool process_inline_image(const std::vector<std::string>& tokens,
                            size_t& index,
                            bool emit_svg);
  bool build_inline_image(const std::map<std::string, std::vector<std::string>>& dict,
                          const std::vector<uint8_t>& data,
                          ImageXObject* out_image) const;
  bool prepare_mask_pixels(const ImageXObject& image,
                           std::vector<uint8_t>& out) const;
  bool combine_with_pending_inline_mask(const ImageXObject& image,
                                        std::vector<uint8_t>& buffer,
                                        int& components);
  bool prepare_soft_mask_object(const ImageXObject& mask_image,
                                std::vector<uint8_t>& mask_pixels) const;
  bool prepare_image_by_id(const std::string& name,
                           ImageXObject* out_image) const;
  bool apply_soft_mask_to_pixels(const ImageXObject& image,
                                 const ImageXObject& mask,
                                 std::vector<uint8_t>& buffer,
                                 int& components,
                                 SoftMaskType mask_type) const;
  bool load_soft_mask_from_value(const Value& value,
                                 ImageXObject* out_image) const;
  void apply_extended_graphics_state(const std::string& name);
  void apply_extended_graphics_state(const ExtendedGraphicsState& ext_state);
  void queue_canvas_image(double a, double b, double c, double d,
                          double e, double f, const std::string& data_url);

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
    double fill_alpha{1.0};
    double stroke_alpha{1.0};
    std::string blend_mode{"source-over"};
    double stroke_width{1.0};
    std::string line_cap{"butt"};
    std::string line_join{"miter"};
    double miter_limit{10.0};
    std::vector<double> dash_pattern;
    double dash_phase{0.0};
    bool in_text_block{false};
    bool in_path{false};
    std::string path_data;
    Matrix2D ctm;
   Matrix2D text_matrix;
   Matrix2D text_line_matrix;
   double leading{0.0};
   double char_spacing{0.0};      // Tc operator
   double word_spacing{0.0};      // Tw operator
   double horizontal_scaling{100.0}; // Tz operator (percentage)
   double text_rise{0.0};         // Ts operator
   int text_render_mode{0};       // Tr operator
    SoftMaskType soft_mask_type{SoftMaskType::None};
    Value soft_mask_value;
    std::string fill_color_space;    // Current nonstroking color space name
    std::string stroke_color_space;  // Current stroking color space name
    bool pending_clip{false};        // W operator sets this
    bool pending_clip_evenodd{false}; // W* operator sets this
    int svg_clip_group_depth{0};     // Open <g clip-path> elements at this save level
  } state_;

  // OCG (Optional Content Group) visibility tracking
  // Stack of skip depth counters: >0 means content is hidden
  int ocg_skip_depth_{0};
  std::vector<bool> marked_content_visibility_stack_;

  std::map<std::string, ImageXObject> image_xobjects_;
  std::vector<std::vector<uint8_t>> inline_images_;
  std::unique_ptr<ImageXObject> pending_inline_mask_;
  size_t inline_image_cursor_{0};
  std::map<std::string, ExtendedGraphicsState> ext_gstates_;
  const Pdf* current_pdf_{nullptr};
  const Page* current_page_{nullptr};
  std::vector<GraphicsState> graphics_state_stack_;
  bool canvas_mode_{false};
  bool svg_mode_{false};

  // SVG gradient and pattern definitions
  std::vector<SvgGradient> svg_gradients_;
  std::vector<SvgPattern> svg_patterns_;
  int svg_gradient_counter_{0};
  int svg_pattern_counter_{0};
  int svg_clip_counter_{0};

  // Methods for gradient/pattern handling
  std::string create_svg_gradient(const Value& shading, const Matrix2D& ctm);
  std::string create_svg_pattern(const Value& pattern, const Matrix2D& ctm);
  void parse_shading_resources(const Pdf& pdf, const Dictionary& resources);
  void parse_pattern_resources(const Pdf& pdf, const Dictionary& resources);
  std::map<std::string, Value> shading_resources_;
  std::map<std::string, Value> pattern_resources_;

  // Color space resources: resolved ColorSpace objects by resource name
  std::map<std::string, ColorSpace> color_space_resources_;
  void parse_color_space_resources(const Pdf& pdf, const Dictionary& resources);
  std::string resolve_color_with_space(const ColorSpace& cs,
                                        const std::vector<std::string>& operands);
  std::string lab_to_hex(double l_star, double a_star, double b_star,
                          const ColorSpace& cs) const;
};

}  // namespace nanopdf
