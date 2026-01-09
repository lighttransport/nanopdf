#include "canvas-exporter.hh"

#include <cassert>
#include <string>
#include <vector>

using namespace nanopdf;

namespace {

Value make_number(double value) {
  Value v;
  v.SetType(Value::NUMBER);
  v.number = value;
  return v;
}

Value make_name(const std::string& name) {
  Value v;
  v.SetType(Value::NAME);
  v.name = name;
  return v;
}

Value make_dictionary(std::initializer_list<std::pair<const char*, Value>> init) {
  Value v;
  v.SetType(Value::DICTIONARY);
  for (const auto& kv : init) {
    v.dict[kv.first] = kv.second;
  }
  return v;
}

Value make_number_array(std::initializer_list<double> values) {
  Value v;
  v.SetType(Value::ARRAY);
  for (double value : values) {
    v.array.push_back(make_number(value));
  }
  return v;
}

Value make_dash_pattern(std::initializer_list<double> pattern, double phase) {
  Value dash;
  dash.SetType(Value::ARRAY);
  dash.array.push_back(make_number_array(pattern));
  dash.array.push_back(make_number(phase));
  return dash;
}

Value make_stream(const std::string& content) {
  Value v;
  v.SetType(Value::STREAM);
  v.stream.data.assign(content.begin(), content.end());
  return v;
}

}  // namespace

int main() {
  Pdf pdf;
  Page page;
  page.media_box = {0, 0, 100, 100};

  // Prepare ExtGState with alpha and blend mode.
  Value gs_entry = make_dictionary({
      {"Type", make_name("ExtGState")},
      {"ca", make_number(0.5)},
      {"CA", make_number(0.25)},
      {"BM", make_name("Multiply")},
      {"LW", make_number(3.5)},
      {"LC", make_number(2)},
      {"LJ", make_number(1)},
      {"ML", make_number(5.0)},
      {"D", make_dash_pattern({3.0, 1.5}, 2.25)},
  });

  Value ext_gstate_dict;
  ext_gstate_dict.SetType(Value::DICTIONARY);
  ext_gstate_dict.dict["GS1"] = gs_entry;

  page.resources["ExtGState"] = ext_gstate_dict;

  const std::string content =
      "q\n"
      "/GS1 gs\n"
      "0.2 0.4 0.6 rg\n"
      "0 0 1 RG\n"
      "10 10 20 20 re B\n"
      "Q\n";

  page.contents.push_back(make_stream(content));

  pdf.catalog.pages.push_back(std::move(page));
  pdf.catalog.pages_count = 1;

  CanvasExporter exporter;
  CanvasExportResult canvas_result = exporter.export_page(pdf, pdf.catalog.pages.front());
  assert(canvas_result.success);

  bool saw_fill = false;
  bool saw_stroke = false;
  bool saw_blend = false;
  bool saw_line_width = false;
  bool saw_line_cap = false;
  bool saw_line_join = false;
  bool saw_miter_limit = false;
  bool saw_set_line_dash = false;
  bool saw_dash_offset = false;
  for (const auto& cmd : canvas_result.commands) {
    if (cmd.command == "fillStyle" && !cmd.args.empty()) {
      if (cmd.args[0].find("rgba(51,102,153,0.500)") != std::string::npos) {
        saw_fill = true;
      }
    } else if (cmd.command == "strokeStyle" && !cmd.args.empty()) {
      if (cmd.args[0].find("rgba(0,0,255,0.250)") != std::string::npos) {
        saw_stroke = true;
      }
    } else if (cmd.command == "globalCompositeOperation" && !cmd.args.empty()) {
      if (cmd.args[0].find("multiply") != std::string::npos) {
        saw_blend = true;
      }
    } else if (cmd.command == "lineWidth" && !cmd.args.empty()) {
      if (cmd.args[0] == "3.500") {
        saw_line_width = true;
      }
    } else if (cmd.command == "lineCap" && !cmd.args.empty()) {
      if (cmd.args[0] == "\"square\"") {
        saw_line_cap = true;
      }
    } else if (cmd.command == "lineJoin" && !cmd.args.empty()) {
      if (cmd.args[0] == "\"round\"") {
        saw_line_join = true;
      }
    } else if (cmd.command == "miterLimit" && !cmd.args.empty()) {
      if (cmd.args[0] == "5.000") {
        saw_miter_limit = true;
      }
    } else if (cmd.command == "setLineDash" && !cmd.args.empty()) {
      if (cmd.args[0] == "[3.000, 1.500]") {
        saw_set_line_dash = true;
      }
    } else if (cmd.command == "lineDashOffset" && !cmd.args.empty()) {
      if (cmd.args[0] == "2.250") {
        saw_dash_offset = true;
      }
    }
  }

  assert(saw_fill);
  assert(saw_stroke);
  assert(saw_blend);
  assert(saw_line_width);
  assert(saw_line_cap);
  assert(saw_line_join);
  assert(saw_miter_limit);
  assert(saw_set_line_dash);
  assert(saw_dash_offset);

  SvgExportResult svg_result = exporter.export_page_to_svg(pdf, pdf.catalog.pages.front());
  assert(svg_result.success);

  bool svg_opacity_fill = false;
  bool svg_opacity_stroke = false;
  bool svg_blend = false;

  for (const auto& element : svg_result.elements) {
    auto type_it = element.attributes.find("fill-opacity");
    if (type_it != element.attributes.end() && type_it->second == "0.500") {
      svg_opacity_fill = true;
    }
    auto stroke_it = element.attributes.find("stroke-opacity");
    if (stroke_it != element.attributes.end() && stroke_it->second == "0.250") {
      svg_opacity_stroke = true;
    }
    auto style_it = element.attributes.find("style");
    if (style_it != element.attributes.end() && style_it->second.find("mix-blend-mode:multiply") != std::string::npos) {
      svg_blend = true;
    }
  }

  assert(svg_opacity_fill);
  assert(svg_opacity_stroke);
  assert(svg_blend);

  return 0;
}
