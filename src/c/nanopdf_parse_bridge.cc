#include "nanopdf_basic_document.h"
#include "nanopdf_c_internal.h"
#include "nanopdf_object.h"
#include "nanopdf_parse.h"
#include "nanopdf_text.h"

#include "../nanopdf.hh"
#include "../pdf-attachments.hh"
#include "../table-extraction.hh"
#include "../text-layout.hh"

#include <cstring>
#include <memory>
#include <new>
#include <string>
#include <utility>
#include <vector>

typedef struct nanopdf_document {
  nanopdf_context* context;
  uint8_t* owned_data;
  size_t owned_size;
  nanopdf_basic_document basic;
  nanopdf_parse_options parse_options;
  void* cpp_bridge;
} nanopdf_document;

struct nanopdf_table_result {
  nanopdf_context* context{nullptr};
  std::vector<nanopdf::Table> tables;
};

struct nanopdf_pdfa_report {
  nanopdf_context* context{nullptr};
  nanopdf::PdfAValidationResult result;
};

namespace {

using nanopdf::ErrorKind;
using nanopdf::FileAttachment;
using nanopdf::OutlineAction;
using nanopdf::OutlineItem;
using nanopdf::ParseOptions;
using nanopdf::Pdf;
using nanopdf::PdfAValidationResult;
using nanopdf::PdfAViolation;
using nanopdf::Table;
using nanopdf::TableCell;
using nanopdf::TableExtractionConfig;
using nanopdf::TextLayoutOptions;
using nanopdf::AttachmentExtractor;

struct FlatBookmark {
  const OutlineItem* item{nullptr};
  uint32_t depth{0};
  uint32_t page_index{0};
};

struct ParsedAnnotation {
  nanopdf_annotation_type type{NANOPDF_ANNOTATION_TYPE_TEXT};
  double rect[4]{0.0, 0.0, 0.0, 0.0};
  std::string contents;
  std::string name;
  std::string modified_date;
  uint32_t flags{0};
  nanopdf_annotation_action_type action_type{NANOPDF_ANNOTATION_ACTION_NONE};
  std::string uri;
  nanopdf_field_type field_type{NANOPDF_FIELD_TYPE_TEXT};
  std::string field_name;
  std::string field_value;
};

struct DocumentBridge {
  std::unique_ptr<Pdf> pdf;
  std::vector<uint8_t> annotations_loaded;
  std::vector<std::vector<ParsedAnnotation>> parsed_annotations;
  std::vector<FileAttachment> attachments;
  std::vector<std::unique_ptr<OutlineItem>> outline_roots;
  std::vector<FlatBookmark> bookmarks;
  bool attachments_loaded{false};
  bool bookmarks_loaded{false};
};

static nanopdf_status set_error(
    nanopdf_context* context,
    nanopdf_status status,
    const char* message) {
  nanopdf__set_error(context, status, message);
  return status;
}

static nanopdf_status clear_success(nanopdf_context* context) {
  nanopdf__clear_error(context);
  return NANOPDF_STATUS_OK;
}

static nanopdf_status copy_string(
    nanopdf_context* context,
    const std::string& value,
    char** out_value) {
  return nanopdf__copy_owned_string(context, value.c_str(), out_value);
}

static nanopdf_status copy_bytes(
    nanopdf_context* context,
    const std::vector<uint8_t>& value,
    void** out_data,
    size_t* out_size) {
  void* output = nullptr;

  if (!context || !out_data || !out_size) {
    return set_error(
        context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "invalid attachment output buffer arguments");
  }

  *out_data = nullptr;
  *out_size = 0;
  if (!value.empty()) {
    output = nanopdf__allocator_alloc(&context->allocator, value.size());
    if (!output) {
      return set_error(
          context,
          NANOPDF_STATUS_OUT_OF_MEMORY,
          "failed to allocate attachment output buffer");
    }
    std::memcpy(output, value.data(), value.size());
  }

  *out_data = output;
  *out_size = value.size();
  return NANOPDF_STATUS_OK;
}

static nanopdf_status validate_document_handle(
    nanopdf_document* document,
    const char* message) {
  if (!document || !document->context) {
    return set_error(
        document ? document->context : nullptr,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        message);
  }
  return NANOPDF_STATUS_OK;
}

static nanopdf_status map_error_kind(ErrorKind kind) {
  switch (kind) {
    case ErrorKind::Malformed:
      return NANOPDF_STATUS_MALFORMED;
    case ErrorKind::Unsupported:
      return NANOPDF_STATUS_UNSUPPORTED;
    case ErrorKind::Encrypted:
      return NANOPDF_STATUS_ENCRYPTED;
    case ErrorKind::IOError:
      return NANOPDF_STATUS_IO_ERROR;
    case ErrorKind::Internal:
      return NANOPDF_STATUS_INTERNAL_ERROR;
    case ErrorKind::None:
    default:
      return NANOPDF_STATUS_PARSE_ERROR;
  }
}

static DocumentBridge* get_bridge(nanopdf_document* document) {
  return reinterpret_cast<DocumentBridge*>(document->cpp_bridge);
}

static nanopdf_status ensure_bridge(
    nanopdf_document* document,
    DocumentBridge** out_bridge) {
  DocumentBridge* bridge = nullptr;

  if (validate_document_handle(document, "document handle is null") != NANOPDF_STATUS_OK) {
    return NANOPDF_STATUS_INVALID_ARGUMENT;
  }

  bridge = get_bridge(document);
  if (!bridge) {
    bridge = new (std::nothrow) DocumentBridge();
    if (!bridge) {
      return set_error(
          document->context,
          NANOPDF_STATUS_OUT_OF_MEMORY,
          "failed to allocate advanced parse bridge");
    }
    document->cpp_bridge = bridge;
  }

  if (out_bridge) {
    *out_bridge = bridge;
  }
  return NANOPDF_STATUS_OK;
}

static nanopdf_status ensure_pdf(
    nanopdf_document* document,
    DocumentBridge** out_bridge) {
  DocumentBridge* bridge = nullptr;
  nanopdf::ParseResult parse_result;
  ParseOptions options;

  if (ensure_bridge(document, &bridge) != NANOPDF_STATUS_OK) {
    return document->context ? document->context->last_status : NANOPDF_STATUS_INVALID_ARGUMENT;
  }

  if (!bridge->pdf) {
    bridge->pdf.reset(new (std::nothrow) Pdf());
    if (!bridge->pdf) {
      return set_error(
          document->context,
          NANOPDF_STATUS_OUT_OF_MEMORY,
          "failed to allocate parsed PDF state");
    }

    options.auto_repair = document->parse_options.auto_repair != 0;
    options.recover_stream_length = document->parse_options.recover_stream_length != 0;
    options.max_repair_scan = document->parse_options.max_repair_scan;

    parse_result = nanopdf::parse_pdf(
        document->owned_data,
        document->owned_size,
        bridge->pdf.get(),
        options);
    if (!parse_result.success) {
      bridge->pdf.reset();
      return set_error(
          document->context,
          map_error_kind(parse_result.kind),
          parse_result.error.empty()
              ? "failed to parse PDF for advanced C API features"
              : parse_result.error.c_str());
    }

    bridge->pdf->ensure_pages_loaded();
    bridge->annotations_loaded.assign(bridge->pdf->catalog.pages.size(), 0u);
  }

  if (out_bridge) {
    *out_bridge = bridge;
  }
  return NANOPDF_STATUS_OK;
}

static uint32_t map_outline_page_index(
    const Pdf& pdf,
    uint32_t dest_page_object_number);

static void flatten_outline(
    const Pdf& pdf,
    const OutlineItem* parent,
    uint32_t depth,
    std::vector<FlatBookmark>* out) {
  if (!parent || !out) {
    return;
  }

  for (const auto& child : parent->children) {
    uint32_t child_page_index = map_outline_page_index(pdf, child->dest_page);
    out->push_back({child.get(), depth, child_page_index});
    flatten_outline(pdf, child.get(), depth + 1u, out);
  }
}

static uint32_t map_outline_page_index(
    const Pdf& pdf,
    uint32_t dest_page_object_number) {
  size_t page_index = 0;
  for (page_index = 0; page_index < pdf.catalog.pages.size(); ++page_index) {
    if (pdf.catalog.pages[page_index].object_number == dest_page_object_number) {
      return static_cast<uint32_t>(page_index);
    }
  }
  return dest_page_object_number;
}

static const char* basic_object_text(const nanopdf_basic_object* object) {
  if (!object) {
    return "";
  }
  if (object->type == NANOPDF_BASIC_OBJECT_STRING ||
      object->type == NANOPDF_BASIC_OBJECT_NAME) {
    return object->as.text ? object->as.text : "";
  }
  return "";
}

static int basic_object_number_value(
    const nanopdf_basic_object* object,
    double* out_value) {
  if (!object || object->type != NANOPDF_BASIC_OBJECT_NUMBER || !out_value) {
    return 0;
  }
  *out_value = object->as.number;
  return 1;
}

static nanopdf_annotation_type annotation_type_from_subtype(
    const char* subtype) {
  if (!subtype) {
    return NANOPDF_ANNOTATION_TYPE_TEXT;
  }
  if (strcmp(subtype, "Text") == 0) return NANOPDF_ANNOTATION_TYPE_TEXT;
  if (strcmp(subtype, "Link") == 0) return NANOPDF_ANNOTATION_TYPE_LINK;
  if (strcmp(subtype, "FreeText") == 0) return NANOPDF_ANNOTATION_TYPE_FREE_TEXT;
  if (strcmp(subtype, "Line") == 0) return NANOPDF_ANNOTATION_TYPE_LINE;
  if (strcmp(subtype, "Square") == 0) return NANOPDF_ANNOTATION_TYPE_SQUARE;
  if (strcmp(subtype, "Circle") == 0) return NANOPDF_ANNOTATION_TYPE_CIRCLE;
  if (strcmp(subtype, "Polygon") == 0) return NANOPDF_ANNOTATION_TYPE_POLYGON;
  if (strcmp(subtype, "PolyLine") == 0) return NANOPDF_ANNOTATION_TYPE_POLY_LINE;
  if (strcmp(subtype, "Highlight") == 0) return NANOPDF_ANNOTATION_TYPE_HIGHLIGHT;
  if (strcmp(subtype, "Underline") == 0) return NANOPDF_ANNOTATION_TYPE_UNDERLINE;
  if (strcmp(subtype, "Squiggly") == 0) return NANOPDF_ANNOTATION_TYPE_SQUIGGLY;
  if (strcmp(subtype, "StrikeOut") == 0) return NANOPDF_ANNOTATION_TYPE_STRIKE_OUT;
  if (strcmp(subtype, "Stamp") == 0) return NANOPDF_ANNOTATION_TYPE_STAMP;
  if (strcmp(subtype, "Caret") == 0) return NANOPDF_ANNOTATION_TYPE_CARET;
  if (strcmp(subtype, "Ink") == 0) return NANOPDF_ANNOTATION_TYPE_INK;
  if (strcmp(subtype, "Popup") == 0) return NANOPDF_ANNOTATION_TYPE_POPUP;
  if (strcmp(subtype, "FileAttachment") == 0) return NANOPDF_ANNOTATION_TYPE_FILE_ATTACHMENT;
  if (strcmp(subtype, "Sound") == 0) return NANOPDF_ANNOTATION_TYPE_SOUND;
  if (strcmp(subtype, "Movie") == 0) return NANOPDF_ANNOTATION_TYPE_MOVIE;
  if (strcmp(subtype, "Widget") == 0) return NANOPDF_ANNOTATION_TYPE_WIDGET;
  if (strcmp(subtype, "Screen") == 0) return NANOPDF_ANNOTATION_TYPE_SCREEN;
  if (strcmp(subtype, "PrinterMark") == 0) return NANOPDF_ANNOTATION_TYPE_PRINTER_MARK;
  if (strcmp(subtype, "TrapNet") == 0) return NANOPDF_ANNOTATION_TYPE_TRAP_NET;
  if (strcmp(subtype, "Watermark") == 0) return NANOPDF_ANNOTATION_TYPE_WATERMARK;
  if (strcmp(subtype, "3D") == 0) return NANOPDF_ANNOTATION_TYPE_THREE_D;
  if (strcmp(subtype, "Redact") == 0) return NANOPDF_ANNOTATION_TYPE_REDACT;
  return NANOPDF_ANNOTATION_TYPE_TEXT;
}

static nanopdf_annotation_action_type annotation_action_from_name(
    const char* action_name) {
  if (!action_name) {
    return NANOPDF_ANNOTATION_ACTION_NONE;
  }
  if (strcmp(action_name, "GoTo") == 0) return NANOPDF_ANNOTATION_ACTION_GOTO;
  if (strcmp(action_name, "GoToR") == 0) return NANOPDF_ANNOTATION_ACTION_GOTO_REMOTE;
  if (strcmp(action_name, "Launch") == 0) return NANOPDF_ANNOTATION_ACTION_LAUNCH;
  if (strcmp(action_name, "URI") == 0) return NANOPDF_ANNOTATION_ACTION_URI;
  if (strcmp(action_name, "Named") == 0) return NANOPDF_ANNOTATION_ACTION_NAMED;
  if (strcmp(action_name, "JavaScript") == 0) return NANOPDF_ANNOTATION_ACTION_JAVASCRIPT;
  return NANOPDF_ANNOTATION_ACTION_NONE;
}

static nanopdf_field_type field_type_from_name(const char* field_type_name) {
  if (!field_type_name) {
    return NANOPDF_FIELD_TYPE_TEXT;
  }
  if (strcmp(field_type_name, "Btn") == 0) return NANOPDF_FIELD_TYPE_BUTTON;
  if (strcmp(field_type_name, "Tx") == 0) return NANOPDF_FIELD_TYPE_TEXT;
  if (strcmp(field_type_name, "Ch") == 0) return NANOPDF_FIELD_TYPE_CHOICE;
  if (strcmp(field_type_name, "Sig") == 0) return NANOPDF_FIELD_TYPE_SIGNATURE;
  return NANOPDF_FIELD_TYPE_TEXT;
}

static void parse_annotation_rect(
    const nanopdf_basic_object* rect_object,
    ParsedAnnotation* annotation) {
  size_t i = 0;
  if (!rect_object || rect_object->type != NANOPDF_BASIC_OBJECT_ARRAY || !annotation) {
    return;
  }
  for (i = 0; i < 4 && i < rect_object->as.array.count; ++i) {
    double value = 0.0;
    if (basic_object_number_value(&rect_object->as.array.items[i], &value)) {
      annotation->rect[i] = value;
    }
  }
}

static void parse_annotation_action(
    nanopdf_document* document,
    const nanopdf_basic_object* action_object,
    ParsedAnnotation* annotation) {
  nanopdf_basic_object action_storage;
  const nanopdf_basic_object* resolved_action = action_object;
  const nanopdf_basic_dict* action_dict = nullptr;

  if (!document || !action_object || !annotation) {
    return;
  }

  memset(&action_storage, 0, sizeof(action_storage));
  if (action_object->type == NANOPDF_BASIC_OBJECT_REF && action_object->as.ref.valid) {
    nanopdf_basic_object_init(&action_storage);
    if (nanopdf_basic_load_object(
            document->context,
            &document->basic,
            action_object->as.ref,
            &action_storage) == NANOPDF_STATUS_OK) {
      resolved_action = &action_storage;
    } else {
      resolved_action = nullptr;
    }
  }

  action_dict = nanopdf_basic_object_as_dict(resolved_action);
  if (action_dict) {
    const nanopdf_basic_object* type_object = nanopdf_basic_dict_get(action_dict, "S");
    const nanopdf_basic_object* uri_object = nanopdf_basic_dict_get(action_dict, "URI");
    annotation->action_type = annotation_action_from_name(basic_object_text(type_object));
    if (uri_object) {
      annotation->uri = basic_object_text(uri_object);
    }
  }

  if (resolved_action == &action_storage) {
    nanopdf_basic_object_destroy(&document->context->allocator, &action_storage);
  }
}

static void parse_widget_fields(
    nanopdf_document* document,
    const nanopdf_basic_dict* dict,
    ParsedAnnotation* annotation) {
  nanopdf_basic_object parent_storage;
  const nanopdf_basic_dict* active_dict = dict;
  const nanopdf_basic_object* field_type_object = nullptr;
  const nanopdf_basic_object* field_name_object = nullptr;
  const nanopdf_basic_object* field_value_object = nullptr;

  if (!document || !dict || !annotation) {
    return;
  }

  memset(&parent_storage, 0, sizeof(parent_storage));
  field_type_object = nanopdf_basic_dict_get(active_dict, "FT");
  if (!field_type_object) {
    const nanopdf_basic_object* parent_object = nanopdf_basic_dict_get(active_dict, "Parent");
    if (parent_object && parent_object->type == NANOPDF_BASIC_OBJECT_REF && parent_object->as.ref.valid) {
      nanopdf_basic_object_init(&parent_storage);
      if (nanopdf_basic_load_object(
              document->context,
              &document->basic,
              parent_object->as.ref,
              &parent_storage) == NANOPDF_STATUS_OK) {
        active_dict = nanopdf_basic_object_as_dict(&parent_storage);
        field_type_object = nanopdf_basic_dict_get(active_dict, "FT");
      }
    }
  }

  annotation->field_type = field_type_from_name(basic_object_text(field_type_object));
  field_name_object = nanopdf_basic_dict_get(active_dict, "T");
  field_value_object = nanopdf_basic_dict_get(active_dict, "V");
  if (field_name_object) {
    annotation->field_name = basic_object_text(field_name_object);
  }
  if (field_value_object) {
    annotation->field_value = basic_object_text(field_value_object);
  }

  if (active_dict != dict) {
    nanopdf_basic_object_destroy(&document->context->allocator, &parent_storage);
  }
}

static void parse_basic_annotation(
    nanopdf_document* document,
    const nanopdf_basic_dict* dict,
    ParsedAnnotation* annotation) {
  const nanopdf_basic_object* subtype_object = nullptr;
  const nanopdf_basic_object* rect_object = nullptr;
  const nanopdf_basic_object* contents_object = nullptr;
  const nanopdf_basic_object* name_object = nullptr;
  const nanopdf_basic_object* modified_object = nullptr;
  const nanopdf_basic_object* flags_object = nullptr;
  const nanopdf_basic_object* action_object = nullptr;
  double flags_value = 0.0;

  if (!document || !dict || !annotation) {
    return;
  }

  subtype_object = nanopdf_basic_dict_get(dict, "Subtype");
  annotation->type = annotation_type_from_subtype(basic_object_text(subtype_object));
  rect_object = nanopdf_basic_dict_get(dict, "Rect");
  contents_object = nanopdf_basic_dict_get(dict, "Contents");
  name_object = nanopdf_basic_dict_get(dict, "NM");
  modified_object = nanopdf_basic_dict_get(dict, "M");
  flags_object = nanopdf_basic_dict_get(dict, "F");
  action_object = nanopdf_basic_dict_get(dict, "A");

  parse_annotation_rect(rect_object, annotation);
  annotation->contents = basic_object_text(contents_object);
  annotation->name = basic_object_text(name_object);
  annotation->modified_date = basic_object_text(modified_object);
  if (basic_object_number_value(flags_object, &flags_value)) {
    annotation->flags = static_cast<uint32_t>(flags_value);
  }
  if (action_object) {
    parse_annotation_action(document, action_object, annotation);
  }
  if (annotation->type == NANOPDF_ANNOTATION_TYPE_WIDGET) {
    parse_widget_fields(document, dict, annotation);
  }
}

static nanopdf_status ensure_bookmarks(
    nanopdf_document* document,
    DocumentBridge** out_bridge) {
  DocumentBridge* bridge = nullptr;
  nanopdf_status status = ensure_pdf(document, &bridge);
  if (status != NANOPDF_STATUS_OK) {
    return status;
  }

  if (!bridge->bookmarks_loaded) {
    bridge->outline_roots.clear();
    bridge->bookmarks.clear();
    nanopdf::ResolvedObject catalog_resolved =
        nanopdf::resolve_reference(*bridge->pdf, bridge->pdf->root, 0);
    if (catalog_resolved.success &&
        catalog_resolved.value.type == nanopdf::Value::DICTIONARY) {
      auto outlines_it = catalog_resolved.value.dict.find("Outlines");
      if (outlines_it != catalog_resolved.value.dict.end()) {
        nanopdf::Value outlines_value = outlines_it->second;
        if (outlines_value.type == nanopdf::Value::REFERENCE) {
          nanopdf::ResolvedObject outlines_resolved =
              nanopdf::resolve_reference(
                  *bridge->pdf,
                  outlines_value.ref_object_number,
                  outlines_value.ref_generation_number);
          if (outlines_resolved.success &&
              outlines_resolved.value.type == nanopdf::Value::DICTIONARY) {
            auto first_it = outlines_resolved.value.dict.find("First");
            if (first_it != outlines_resolved.value.dict.end() &&
                first_it->second.type == nanopdf::Value::REFERENCE) {
              nanopdf::ResolvedObject item_resolved =
                  nanopdf::resolve_reference(
                      *bridge->pdf,
                      first_it->second.ref_object_number,
                      first_it->second.ref_generation_number);
              while (item_resolved.success &&
                     item_resolved.value.type == nanopdf::Value::DICTIONARY) {
                auto item = nanopdf::parse_outline_item(
                    *bridge->pdf, item_resolved.value.dict);
                if (item) {
                  uint32_t page_index =
                      map_outline_page_index(*bridge->pdf, item->dest_page);
                  bridge->bookmarks.push_back({item.get(), 0u, page_index});
                  flatten_outline(*bridge->pdf, item.get(), 1u, &bridge->bookmarks);
                  bridge->outline_roots.push_back(std::move(item));
                }

                auto next_it = item_resolved.value.dict.find("Next");
                if (next_it == item_resolved.value.dict.end() ||
                    next_it->second.type != nanopdf::Value::REFERENCE) {
                  break;
                }

                item_resolved = nanopdf::resolve_reference(
                    *bridge->pdf,
                    next_it->second.ref_object_number,
                    next_it->second.ref_generation_number);
              }
            }
          }
        }
      }
    }
    bridge->bookmarks_loaded = true;
  }

  if (out_bridge) {
    *out_bridge = bridge;
  }
  return NANOPDF_STATUS_OK;
}

static nanopdf_status ensure_attachments(
    nanopdf_document* document,
    DocumentBridge** out_bridge) {
  DocumentBridge* bridge = nullptr;
  nanopdf_status status = ensure_pdf(document, &bridge);
  if (status != NANOPDF_STATUS_OK) {
    return status;
  }

  if (!bridge->attachments_loaded) {
    AttachmentExtractor extractor(*bridge->pdf);
    int attachment_count = extractor.get_count();
    int index = 0;
    bridge->attachments.clear();
    if (attachment_count > 0) {
      bridge->attachments.reserve(static_cast<size_t>(attachment_count));
      for (index = 0; index < attachment_count; ++index) {
        bridge->attachments.push_back(extractor.get_attachment(index));
      }
    }
    bridge->attachments_loaded = true;
  }

  if (out_bridge) {
    *out_bridge = bridge;
  }
  return NANOPDF_STATUS_OK;
}

static nanopdf_status ensure_page_annotations(
    nanopdf_document* document,
    uint32_t page_index,
    DocumentBridge** out_bridge) {
  DocumentBridge* bridge = nullptr;
  nanopdf_status status = ensure_pdf(document, &bridge);
  if (status != NANOPDF_STATUS_OK) {
    return status;
  }

  if (page_index >= bridge->pdf->catalog.pages.size() ||
      page_index >= document->basic.page_count) {
    return set_error(
        document->context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "page index is out of range");
  }

  if (bridge->annotations_loaded.size() < document->basic.page_count) {
    bridge->annotations_loaded.assign(document->basic.page_count, 0u);
  }
  if (bridge->parsed_annotations.size() < document->basic.page_count) {
    bridge->parsed_annotations.assign(document->basic.page_count, {});
  }

  if (!bridge->annotations_loaded[page_index]) {
    nanopdf_basic_object page_object;
    nanopdf_basic_object annots_object;
    const nanopdf_basic_dict* page_dict = nullptr;
    const nanopdf_basic_object* annots_value = nullptr;
    nanopdf_basic_ref page_ref;
    size_t annotation_index = 0;

    memset(&page_object, 0, sizeof(page_object));
    memset(&annots_object, 0, sizeof(annots_object));
    bridge->parsed_annotations[page_index].clear();

    page_ref.object_number = document->basic.pages[page_index].object_number;
    page_ref.generation = document->basic.pages[page_index].generation;
    page_ref.valid = 1u;
    nanopdf_basic_object_init(&page_object);
    if (nanopdf_basic_load_object(
            document->context,
            &document->basic,
            page_ref,
            &page_object) == NANOPDF_STATUS_OK) {
      page_dict = nanopdf_basic_object_as_dict(&page_object);
      annots_value = nanopdf_basic_dict_get(page_dict, "Annots");
      if (annots_value && annots_value->type == NANOPDF_BASIC_OBJECT_REF &&
          annots_value->as.ref.valid) {
        nanopdf_basic_object_init(&annots_object);
        if (nanopdf_basic_load_object(
                document->context,
                &document->basic,
                annots_value->as.ref,
                &annots_object) == NANOPDF_STATUS_OK) {
          annots_value = &annots_object;
        }
      }

      if (annots_value && annots_value->type == NANOPDF_BASIC_OBJECT_ARRAY) {
        for (annotation_index = 0;
             annotation_index < annots_value->as.array.count;
             ++annotation_index) {
          nanopdf_basic_object annotation_object;
          const nanopdf_basic_object* annotation_value =
              &annots_value->as.array.items[annotation_index];
          const nanopdf_basic_dict* annotation_dict = nullptr;
          ParsedAnnotation parsed_annotation;

          memset(&annotation_object, 0, sizeof(annotation_object));
          if (annotation_value->type == NANOPDF_BASIC_OBJECT_REF &&
              annotation_value->as.ref.valid) {
            nanopdf_basic_object_init(&annotation_object);
            if (nanopdf_basic_load_object(
                    document->context,
                    &document->basic,
                    annotation_value->as.ref,
                    &annotation_object) == NANOPDF_STATUS_OK) {
              annotation_value = &annotation_object;
            }
          }

          annotation_dict = nanopdf_basic_object_as_dict(annotation_value);
          if (annotation_dict) {
            parse_basic_annotation(document, annotation_dict, &parsed_annotation);
            bridge->parsed_annotations[page_index].push_back(std::move(parsed_annotation));
          }

          if (annotation_value == &annotation_object) {
            nanopdf_basic_object_destroy(&document->context->allocator, &annotation_object);
          }
        }
      }
    }

    nanopdf_basic_object_destroy(&document->context->allocator, &page_object);
    if (annots_value == &annots_object) {
      nanopdf_basic_object_destroy(&document->context->allocator, &annots_object);
    }
    bridge->annotations_loaded[page_index] = 1u;
  }

  if (out_bridge) {
    *out_bridge = bridge;
  }
  return NANOPDF_STATUS_OK;
}

static nanopdf_bookmark_action map_bookmark_action(OutlineAction action) {
  switch (action) {
    case OutlineAction::GoTo:
      return NANOPDF_BOOKMARK_ACTION_GOTO;
    case OutlineAction::GoToR:
      return NANOPDF_BOOKMARK_ACTION_GOTO_REMOTE;
    case OutlineAction::URI:
      return NANOPDF_BOOKMARK_ACTION_URI;
    case OutlineAction::Launch:
      return NANOPDF_BOOKMARK_ACTION_LAUNCH;
    default:
      return NANOPDF_BOOKMARK_ACTION_GOTO;
  }
}

static nanopdf_pdfa_rule map_pdfa_rule(PdfAViolation::Rule rule) {
  switch (rule) {
    case PdfAViolation::Rule::MissingXMPMetadata:
      return NANOPDF_PDFA_RULE_MISSING_XMP_METADATA;
    case PdfAViolation::Rule::MissingOutputIntent:
      return NANOPDF_PDFA_RULE_MISSING_OUTPUT_INTENT;
    case PdfAViolation::Rule::FontNotEmbedded:
      return NANOPDF_PDFA_RULE_FONT_NOT_EMBEDDED;
    case PdfAViolation::Rule::TransparencyUsed:
      return NANOPDF_PDFA_RULE_TRANSPARENCY_USED;
    case PdfAViolation::Rule::MissingDocumentInfo:
      return NANOPDF_PDFA_RULE_MISSING_DOCUMENT_INFO;
    case PdfAViolation::Rule::EncryptionPresent:
      return NANOPDF_PDFA_RULE_ENCRYPTION_PRESENT;
    case PdfAViolation::Rule::InvalidColorSpace:
      return NANOPDF_PDFA_RULE_INVALID_COLOR_SPACE;
    default:
      return NANOPDF_PDFA_RULE_MISSING_XMP_METADATA;
  }
}

static void fill_annotation_info(
    const ParsedAnnotation& annotation,
    nanopdf_annotation_info* out_info) {
  size_t i = 0;

  memset(out_info, 0, sizeof(*out_info));
  out_info->type = annotation.type;
  for (i = 0; i < 4; ++i) {
    out_info->rect[i] = annotation.rect[i];
  }
  out_info->contents = annotation.contents.c_str();
  out_info->name = annotation.name.c_str();
  out_info->modified_date = annotation.modified_date.c_str();
  out_info->flags = annotation.flags;
  out_info->action_type = annotation.action_type;
  out_info->field_type = annotation.field_type;
  out_info->field_name = annotation.field_name.c_str();
  out_info->field_value = annotation.field_value.c_str();
  out_info->uri = annotation.uri.c_str();
}

static void fill_bookmark_info(
    const FlatBookmark& bookmark,
    nanopdf_bookmark_info* out_info) {
  memset(out_info, 0, sizeof(*out_info));
  out_info->title = bookmark.item->title.empty() ? "" : bookmark.item->title.c_str();
  out_info->depth = bookmark.depth;
  out_info->action = map_bookmark_action(bookmark.item->action_type);
  out_info->page_index = bookmark.page_index;
  out_info->uri = bookmark.item->uri.empty() ? "" : bookmark.item->uri.c_str();
  out_info->file = bookmark.item->file.empty() ? "" : bookmark.item->file.c_str();
  out_info->position = bookmark.item->dest_position.empty()
      ? nullptr : bookmark.item->dest_position.data();
  out_info->position_count = bookmark.item->dest_position.size();
  out_info->color = bookmark.item->color.empty() ? nullptr : bookmark.item->color.data();
  out_info->color_count = bookmark.item->color.size();
  out_info->open = bookmark.item->open ? 1u : 0u;
  out_info->bold = bookmark.item->bold ? 1u : 0u;
  out_info->italic = bookmark.item->italic ? 1u : 0u;
  out_info->visible_descendant_count = bookmark.item->count;
}

static void fill_attachment_info(
    const FileAttachment& attachment,
    nanopdf_attachment_info* out_info) {
  memset(out_info, 0, sizeof(*out_info));
  out_info->name = attachment.name.c_str();
  out_info->description = attachment.description.c_str();
  out_info->mime_type = attachment.mime_type.c_str();
  out_info->checksum = attachment.checksum.c_str();
  out_info->creation_date = attachment.creation_date.c_str();
  out_info->modification_date = attachment.modification_date.c_str();
  out_info->relationship = attachment.relationship.c_str();
  out_info->size = attachment.size;
}

static void fill_table_info(
    const Table& table,
    nanopdf_table_info* out_info) {
  memset(out_info, 0, sizeof(*out_info));
  out_info->row_count = static_cast<uint32_t>(table.rows);
  out_info->column_count = static_cast<uint32_t>(table.cols);
  out_info->x = table.x;
  out_info->y = table.y;
  out_info->width = table.width;
  out_info->height = table.height;
}

static size_t table_cell_count(const Table& table) {
  size_t count = 0;
  for (const auto& row : table.cells) {
    count += row.size();
  }
  return count;
}

static const TableCell* get_table_cell(
    const Table& table,
    size_t flat_index) {
  size_t current = 0;
  for (const auto& row : table.cells) {
    for (const auto& cell : row) {
      if (current == flat_index) {
        return &cell;
      }
      current += 1;
    }
  }
  return nullptr;
}

static void fill_table_cell_info(
    const TableCell& cell,
    nanopdf_table_cell_info* out_info) {
  memset(out_info, 0, sizeof(*out_info));
  out_info->text = cell.text.c_str();
  out_info->row = static_cast<uint32_t>(cell.row);
  out_info->column = static_cast<uint32_t>(cell.col);
  out_info->row_span = static_cast<uint32_t>(cell.row_span);
  out_info->column_span = static_cast<uint32_t>(cell.col_span);
  out_info->x = cell.x;
  out_info->y = cell.y;
  out_info->width = cell.width;
  out_info->height = cell.height;
}

static std::string render_table_text(
    const Table& table,
    nanopdf_table_output_format format) {
  switch (format) {
    case NANOPDF_TABLE_OUTPUT_CSV:
      return table.to_csv();
    case NANOPDF_TABLE_OUTPUT_HTML:
      return table.to_html();
    case NANOPDF_TABLE_OUTPUT_JSON:
      return table.to_json();
    case NANOPDF_TABLE_OUTPUT_MARKDOWN:
      return table.to_markdown();
    case NANOPDF_TABLE_OUTPUT_TEXT:
    default:
      return table.get_text();
  }
}

}  // namespace

extern "C" {

void nanopdf__document_destroy_cpp_bridge(nanopdf_document* document) {
  if (!document) {
    return;
  }
  delete get_bridge(document);
  document->cpp_bridge = nullptr;
}

uint32_t nanopdf_document_bookmark_count(const nanopdf_document* document) {
  nanopdf_document* mutable_document = (nanopdf_document*)document;
  DocumentBridge* bridge = nullptr;
  if (!mutable_document || !mutable_document->context) {
    return 0;
  }
  if (ensure_bookmarks(mutable_document, &bridge) != NANOPDF_STATUS_OK) {
    return 0;
  }
  return static_cast<uint32_t>(bridge->bookmarks.size());
}

nanopdf_status nanopdf_document_get_bookmark(
    const nanopdf_document* document,
    uint32_t index,
    nanopdf_bookmark_info* out_bookmark) {
  nanopdf_document* mutable_document = (nanopdf_document*)document;
  DocumentBridge* bridge = nullptr;
  if (validate_document_handle(mutable_document, "document handle is null") != NANOPDF_STATUS_OK) {
    return NANOPDF_STATUS_INVALID_ARGUMENT;
  }
  if (!out_bookmark) {
    return set_error(
        mutable_document->context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "bookmark output pointer is null");
  }
  if (ensure_bookmarks(mutable_document, &bridge) != NANOPDF_STATUS_OK) {
    return mutable_document->context->last_status;
  }
  if (index >= bridge->bookmarks.size()) {
    return set_error(
        mutable_document->context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "bookmark index is out of range");
  }

  fill_bookmark_info(bridge->bookmarks[index], out_bookmark);
  return clear_success(mutable_document->context);
}

uint32_t nanopdf_document_attachment_count(const nanopdf_document* document) {
  nanopdf_document* mutable_document = (nanopdf_document*)document;
  DocumentBridge* bridge = nullptr;
  if (!mutable_document || !mutable_document->context) {
    return 0;
  }
  if (ensure_attachments(mutable_document, &bridge) != NANOPDF_STATUS_OK) {
    return 0;
  }
  return static_cast<uint32_t>(bridge->attachments.size());
}

nanopdf_status nanopdf_document_get_attachment_info(
    const nanopdf_document* document,
    uint32_t index,
    nanopdf_attachment_info* out_attachment) {
  nanopdf_document* mutable_document = (nanopdf_document*)document;
  DocumentBridge* bridge = nullptr;
  const FileAttachment* attachment = nullptr;

  if (validate_document_handle(mutable_document, "document handle is null") != NANOPDF_STATUS_OK) {
    return NANOPDF_STATUS_INVALID_ARGUMENT;
  }
  if (!out_attachment) {
    return set_error(
        mutable_document->context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "attachment output pointer is null");
  }
  if (ensure_attachments(mutable_document, &bridge) != NANOPDF_STATUS_OK) {
    return mutable_document->context->last_status;
  }
  if (index >= bridge->attachments.size()) {
    return set_error(
        mutable_document->context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "attachment index is out of range");
  }

  attachment = &bridge->attachments[index];
  if (!attachment->success) {
    return set_error(
        mutable_document->context,
        NANOPDF_STATUS_NOT_FOUND,
        attachment->error.empty() ? "attachment could not be extracted"
                                  : attachment->error.c_str());
  }

  fill_attachment_info(*attachment, out_attachment);
  return clear_success(mutable_document->context);
}

nanopdf_status nanopdf_document_copy_attachment_data(
    const nanopdf_document* document,
    uint32_t index,
    void** out_data,
    size_t* out_size) {
  nanopdf_document* mutable_document = (nanopdf_document*)document;
  DocumentBridge* bridge = nullptr;
  const FileAttachment* attachment = nullptr;

  if (validate_document_handle(mutable_document, "document handle is null") != NANOPDF_STATUS_OK) {
    return NANOPDF_STATUS_INVALID_ARGUMENT;
  }
  if (!out_data || !out_size) {
    return set_error(
        mutable_document->context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "attachment data output pointers are null");
  }
  if (ensure_attachments(mutable_document, &bridge) != NANOPDF_STATUS_OK) {
    return mutable_document->context->last_status;
  }
  if (index >= bridge->attachments.size()) {
    return set_error(
        mutable_document->context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "attachment index is out of range");
  }

  attachment = &bridge->attachments[index];
  if (!attachment->success) {
    return set_error(
        mutable_document->context,
        NANOPDF_STATUS_NOT_FOUND,
        attachment->error.empty() ? "attachment could not be extracted"
                                  : attachment->error.c_str());
  }

  return copy_bytes(mutable_document->context, attachment->data, out_data, out_size);
}

uint32_t nanopdf_page_annotation_count(
    const nanopdf_document* document,
    uint32_t page_index) {
  nanopdf_document* mutable_document = (nanopdf_document*)document;
  DocumentBridge* bridge = nullptr;
  if (!mutable_document || !mutable_document->context) {
    return 0;
  }
  if (ensure_page_annotations(mutable_document, page_index, &bridge) != NANOPDF_STATUS_OK) {
    return 0;
  }
  return static_cast<uint32_t>(bridge->parsed_annotations[page_index].size());
}

nanopdf_status nanopdf_page_get_annotation(
    const nanopdf_document* document,
    uint32_t page_index,
    uint32_t annotation_index,
    nanopdf_annotation_info* out_annotation) {
  nanopdf_document* mutable_document = (nanopdf_document*)document;
  DocumentBridge* bridge = nullptr;
  const ParsedAnnotation* annotation = nullptr;

  if (validate_document_handle(mutable_document, "document handle is null") != NANOPDF_STATUS_OK) {
    return NANOPDF_STATUS_INVALID_ARGUMENT;
  }
  if (!out_annotation) {
    return set_error(
        mutable_document->context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "annotation output pointer is null");
  }
  if (ensure_page_annotations(mutable_document, page_index, &bridge) != NANOPDF_STATUS_OK) {
    return mutable_document->context->last_status;
  }
  if (annotation_index >= bridge->parsed_annotations[page_index].size()) {
    return set_error(
        mutable_document->context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "annotation index is out of range");
  }

  annotation = &bridge->parsed_annotations[page_index][annotation_index];
  fill_annotation_info(*annotation, out_annotation);
  return clear_success(mutable_document->context);
}

nanopdf_status nanopdf_page_extract_tables(
    const nanopdf_document* document,
    uint32_t page_index,
    const nanopdf_table_extraction_options* options,
    nanopdf_table_result** out_result) {
  nanopdf_document* mutable_document = (nanopdf_document*)document;
  DocumentBridge* bridge = nullptr;
  TextLayoutOptions layout_options;
  TableExtractionConfig config;
  std::unique_ptr<nanopdf::TextPage> text_page;
  nanopdf_table_result* result = nullptr;

  if (validate_document_handle(mutable_document, "document handle is null") != NANOPDF_STATUS_OK) {
    return NANOPDF_STATUS_INVALID_ARGUMENT;
  }
  if (!out_result) {
    return set_error(
        mutable_document->context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "table result output pointer is null");
  }

  *out_result = nullptr;
  if (ensure_pdf(mutable_document, &bridge) != NANOPDF_STATUS_OK) {
    return mutable_document->context->last_status;
  }
  if (page_index >= bridge->pdf->catalog.pages.size()) {
    return set_error(
        mutable_document->context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "page index is out of range");
  }

  if (options) {
    config.alignment_tolerance = options->alignment_tolerance;
    config.min_rows = options->min_rows;
    config.min_cols = options->min_cols;
    config.max_cell_gap = options->max_cell_gap;
    config.min_chars_per_cell = options->min_chars_per_cell;
    config.debug = options->debug != 0;
  }

  text_page = nanopdf::extract_text_layout(
      *bridge->pdf,
      bridge->pdf->catalog.pages[page_index],
      layout_options);
  if (!text_page) {
    return set_error(
        mutable_document->context,
        NANOPDF_STATUS_INTERNAL_ERROR,
        "failed to extract text layout for table extraction");
  }

  result = new (std::nothrow) nanopdf_table_result();
  if (!result) {
    return set_error(
        mutable_document->context,
        NANOPDF_STATUS_OUT_OF_MEMORY,
        "failed to allocate table extraction result");
  }

  result->context = mutable_document->context;
  result->tables = nanopdf::extract_tables(*text_page, config);
  *out_result = result;
  return clear_success(mutable_document->context);
}

void nanopdf_table_result_destroy(nanopdf_table_result* result) {
  delete result;
}

size_t nanopdf_table_result_table_count(const nanopdf_table_result* result) {
  if (!result) {
    return 0;
  }
  return result->tables.size();
}

nanopdf_status nanopdf_table_result_get_table_info(
    const nanopdf_table_result* result,
    size_t table_index,
    nanopdf_table_info* out_table) {
  if (!result || !out_table) {
    return set_error(
        result ? result->context : nullptr,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "invalid table result arguments");
  }
  if (table_index >= result->tables.size()) {
    return set_error(
        result->context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "table index is out of range");
  }

  fill_table_info(result->tables[table_index], out_table);
  return clear_success(result->context);
}

size_t nanopdf_table_result_table_cell_count(
    const nanopdf_table_result* result,
    size_t table_index) {
  if (!result || table_index >= result->tables.size()) {
    return 0;
  }
  return table_cell_count(result->tables[table_index]);
}

nanopdf_status nanopdf_table_result_get_table_cell(
    const nanopdf_table_result* result,
    size_t table_index,
    size_t cell_index,
    nanopdf_table_cell_info* out_cell) {
  const TableCell* cell = nullptr;

  if (!result || !out_cell) {
    return set_error(
        result ? result->context : nullptr,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "invalid table cell arguments");
  }
  if (table_index >= result->tables.size()) {
    return set_error(
        result->context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "table index is out of range");
  }

  cell = get_table_cell(result->tables[table_index], cell_index);
  if (!cell) {
    return set_error(
        result->context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "table cell index is out of range");
  }

  fill_table_cell_info(*cell, out_cell);
  return clear_success(result->context);
}

nanopdf_status nanopdf_table_result_copy_table_text(
    nanopdf_table_result* result,
    size_t table_index,
    nanopdf_table_output_format format,
    char** out_text) {
  std::string rendered;

  if (!result || !out_text) {
    return set_error(
        result ? result->context : nullptr,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "invalid table render arguments");
  }
  if (table_index >= result->tables.size()) {
    return set_error(
        result->context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "table index is out of range");
  }

  rendered = render_table_text(result->tables[table_index], format);
  return copy_string(result->context, rendered, out_text);
}

nanopdf_status nanopdf_document_validate_pdfa(
    const nanopdf_document* document,
    nanopdf_pdfa_report** out_report) {
  nanopdf_document* mutable_document = (nanopdf_document*)document;
  DocumentBridge* bridge = nullptr;
  nanopdf_pdfa_report* report = nullptr;

  if (validate_document_handle(mutable_document, "document handle is null") != NANOPDF_STATUS_OK) {
    return NANOPDF_STATUS_INVALID_ARGUMENT;
  }
  if (!out_report) {
    return set_error(
        mutable_document->context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "PDF/A report output pointer is null");
  }

  *out_report = nullptr;
  if (ensure_pdf(mutable_document, &bridge) != NANOPDF_STATUS_OK) {
    return mutable_document->context->last_status;
  }

  bridge->pdf->ensure_metadata_loaded();
  for (auto& page : bridge->pdf->catalog.pages) {
    page.ensure_fonts_loaded(*bridge->pdf);
  }

  report = new (std::nothrow) nanopdf_pdfa_report();
  if (!report) {
    return set_error(
        mutable_document->context,
        NANOPDF_STATUS_OUT_OF_MEMORY,
        "failed to allocate PDF/A report");
  }

  report->context = mutable_document->context;
  report->result = nanopdf::validate_pdfa(*bridge->pdf);
  *out_report = report;
  return clear_success(mutable_document->context);
}

void nanopdf_pdfa_report_destroy(nanopdf_pdfa_report* report) {
  delete report;
}

nanopdf_status nanopdf_pdfa_report_get_summary(
    const nanopdf_pdfa_report* report,
    nanopdf_pdfa_summary* out_summary) {
  if (!report || !out_summary) {
    return set_error(
        report ? report->context : nullptr,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "invalid PDF/A summary arguments");
  }

  memset(out_summary, 0, sizeof(*out_summary));
  out_summary->valid = report->result.valid ? 1u : 0u;
  out_summary->claimed_level = report->result.claimed_level.empty()
      ? "" : report->result.claimed_level.c_str();
  return clear_success(report->context);
}

size_t nanopdf_pdfa_report_violation_count(const nanopdf_pdfa_report* report) {
  if (!report) {
    return 0;
  }
  return report->result.violations.size();
}

nanopdf_status nanopdf_pdfa_report_get_violation(
    const nanopdf_pdfa_report* report,
    size_t index,
    nanopdf_pdfa_violation* out_violation) {
  const PdfAViolation* violation = nullptr;

  if (!report || !out_violation) {
    return set_error(
        report ? report->context : nullptr,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "invalid PDF/A violation arguments");
  }
  if (index >= report->result.violations.size()) {
    return set_error(
        report->context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "PDF/A violation index is out of range");
  }

  violation = &report->result.violations[index];
  memset(out_violation, 0, sizeof(*out_violation));
  out_violation->rule = map_pdfa_rule(violation->rule);
  out_violation->message = violation->message.empty() ? "" : violation->message.c_str();
  out_violation->detail = violation->detail.empty() ? "" : violation->detail.c_str();
  return clear_success(report->context);
}

}  // extern "C"
