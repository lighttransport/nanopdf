#include "nanopdf_basic_document.h"
#include "nanopdf_basic_layout.h"
#include "nanopdf_c_internal.h"
#include "nanopdf_forms.h"
#include "nanopdf_parse.h"
#include "nanopdf_text.h"

#include <stdio.h>
#include <string.h>

typedef struct nanopdf_document {
  nanopdf_context* context;
  uint8_t* owned_data;
  size_t owned_size;
  nanopdf_basic_document basic;
  nanopdf_parse_options parse_options;
  void* cpp_bridge;
} nanopdf_document;

typedef struct nanopdf_text_layout {
  nanopdf_context* context;
  nanopdf_basic_layout basic;
} nanopdf_text_layout;

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
    const char* value,
    char** out_value) {
  return nanopdf__copy_owned_string(context, value ? value : "", out_value);
}

static nanopdf_status validate_document_handle(
    nanopdf_document* document,
    const char* message) {
  if (!document || !document->context) {
    return set_error(
        document ? document->context : NULL,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        message);
  }

  return NANOPDF_STATUS_OK;
}

static const char* get_basic_info_value(
    const nanopdf_basic_document* document,
    nanopdf_info_key key) {
  switch (key) {
    case NANOPDF_INFO_TITLE:
      return document->title;
    case NANOPDF_INFO_AUTHOR:
      return document->author;
    case NANOPDF_INFO_SUBJECT:
      return document->subject;
    case NANOPDF_INFO_KEYWORDS:
      return document->keywords;
    case NANOPDF_INFO_CREATOR:
      return document->creator;
    case NANOPDF_INFO_PRODUCER:
      return document->producer;
    case NANOPDF_INFO_CREATION_DATE:
      return document->creation_date;
    case NANOPDF_INFO_MOD_DATE:
      return document->mod_date;
    case NANOPDF_INFO_TRAPPED:
      return document->trapped;
  }

  return NULL;
}

static const char* get_basic_custom_info_value(
    const nanopdf_basic_document* document,
    const char* key) {
  size_t i = 0;
  if (!document || !key) {
    return NULL;
  }
  for (i = 0; i < document->custom_info_count; ++i) {
    if (document->custom_info[i].key &&
        strcmp(document->custom_info[i].key, key) == 0) {
      return document->custom_info[i].value;
    }
  }
  return NULL;
}

static nanopdf_basic_form_field* find_basic_field_by_name(
    nanopdf_document* document,
    const char* field_name) {
  size_t i;

  if (!document || !field_name) {
    return NULL;
  }

  for (i = 0; i < document->basic.form_field_count; ++i) {
    nanopdf_basic_form_field* field = &document->basic.form_fields[i];
    if ((field->full_name && strcmp(field->full_name, field_name) == 0) ||
        (field->partial_name && strcmp(field->partial_name, field_name) == 0)) {
      return field;
    }
  }

  return NULL;
}

static nanopdf_status replace_field_value(
    nanopdf_document* document,
    nanopdf_basic_form_field* field,
    const char* value) {
  char* duplicate = NULL;

  if (!document || !field || !value) {
    return set_error(
        document ? document->context : NULL,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "invalid field value arguments");
  }

  duplicate = nanopdf__strdup(&document->context->allocator, value);
  if (!duplicate) {
    return set_error(
        document->context,
        NANOPDF_STATUS_OUT_OF_MEMORY,
        "failed to allocate field value");
  }

  nanopdf__allocator_free(&document->context->allocator, field->value);
  field->value = duplicate;
  return clear_success(document->context);
}

static nanopdf_status copy_indices_string(
    nanopdf_document* document,
    const int32_t* indices,
    size_t index_count,
    char** out_value) {
  char* buffer = NULL;
  size_t capacity = 0;
  size_t length = 0;
  size_t i;

  if (!document || !out_value) {
    return set_error(
        document ? document->context : NULL,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "invalid choice field arguments");
  }

  if (index_count == 0) {
    *out_value = nanopdf__strdup(&document->context->allocator, "");
    if (!*out_value) {
      return set_error(
          document->context,
          NANOPDF_STATUS_OUT_OF_MEMORY,
          "failed to allocate choice selection");
    }
    return clear_success(document->context);
  }

  for (i = 0; i < index_count; ++i) {
    char chunk[32];
    int written = 0;
    size_t required = 0;
    char* resized = NULL;

    written = snprintf(chunk, sizeof(chunk), "%s%d", i == 0 ? "" : ",", (int)indices[i]);
    if (written < 0) {
      nanopdf__allocator_free(&document->context->allocator, buffer);
      return set_error(
          document->context,
          NANOPDF_STATUS_INTERNAL_ERROR,
          "failed to encode choice selection");
    }

    required = length + (size_t)written + 1;
    if (required > capacity) {
      size_t new_capacity = capacity == 0 ? 32 : capacity * 2;
      while (new_capacity < required) {
        new_capacity *= 2;
      }
      resized = (char*)nanopdf__allocator_realloc(
          &document->context->allocator, buffer, new_capacity);
      if (!resized) {
        nanopdf__allocator_free(&document->context->allocator, buffer);
        return set_error(
            document->context,
            NANOPDF_STATUS_OUT_OF_MEMORY,
            "failed to allocate choice selection");
      }
      buffer = resized;
      capacity = new_capacity;
    }

    memcpy(buffer + length, chunk, (size_t)written);
    length += (size_t)written;
    buffer[length] = '\0';
  }

  *out_value = buffer;
  return clear_success(document->context);
}

nanopdf_status nanopdf_document_open_memory(
    nanopdf_context* context,
    const void* data,
    size_t size,
    const nanopdf_parse_options* options,
    nanopdf_document** out_document) {
  nanopdf_document* document = NULL;
  nanopdf_parse_options defaults;
  nanopdf_status status = NANOPDF_STATUS_OK;

  if (!context || !data || size == 0 || !out_document) {
    return set_error(
        context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "invalid arguments to nanopdf_document_open_memory");
  }

  *out_document = NULL;
  document = (nanopdf_document*)nanopdf__allocator_alloc(
      &context->allocator, sizeof(*document));
  if (!document) {
    return set_error(
        context,
        NANOPDF_STATUS_OUT_OF_MEMORY,
        "failed to allocate document handle");
  }

  memset(document, 0, sizeof(*document));
  document->context = context;
  nanopdf_basic_document_init(&document->basic);

  document->owned_data = (uint8_t*)nanopdf__allocator_alloc(
      &context->allocator, size);
  if (!document->owned_data) {
    nanopdf__allocator_free(&context->allocator, document);
    return set_error(
        context,
        NANOPDF_STATUS_OUT_OF_MEMORY,
        "failed to allocate document buffer");
  }

  memcpy(document->owned_data, data, size);
  document->owned_size = size;
  nanopdf_default_parse_options(&defaults);
  document->parse_options = options ? *options : defaults;
  (void)document->parse_options;

  status = nanopdf_basic_document_parse(
      context,
      document->owned_data,
      document->owned_size,
      &document->parse_options,
      &document->basic);
  if (status != NANOPDF_STATUS_OK) {
    nanopdf_basic_document_destroy(&context->allocator, &document->basic);
    nanopdf__allocator_free(&context->allocator, document->owned_data);
    nanopdf__allocator_free(&context->allocator, document);
    return status;
  }

  *out_document = document;
  return clear_success(context);
}

void nanopdf_document_close(nanopdf_document* document) {
  if (!document) {
    return;
  }

  nanopdf__document_destroy_cpp_bridge(document);
  nanopdf_basic_document_destroy(&document->context->allocator, &document->basic);
  nanopdf__allocator_free(&document->context->allocator, document->owned_data);
  nanopdf__allocator_free(&document->context->allocator, document);
}

uint32_t nanopdf_document_page_count(const nanopdf_document* document) {
  if (!document || !document->context) {
    return 0;
  }

  return (uint32_t)document->basic.page_count;
}

nanopdf_status nanopdf_document_get_page_info(
    const nanopdf_document* document,
    uint32_t page_index,
    nanopdf_page_info* out_page_info) {
  nanopdf_document* mutable_document = (nanopdf_document*)document;
  const size_t index = (size_t)page_index;

  if (validate_document_handle(mutable_document, "document handle is null") != NANOPDF_STATUS_OK) {
    return NANOPDF_STATUS_INVALID_ARGUMENT;
  }
  if (!out_page_info) {
    return set_error(
        mutable_document->context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "page info output pointer is null");
  }
  if (index >= mutable_document->basic.page_count) {
    return set_error(
        mutable_document->context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "page index is out of range");
  }

  out_page_info->page_index = page_index;
  out_page_info->width = mutable_document->basic.pages[index].width;
  out_page_info->height = mutable_document->basic.pages[index].height;
  out_page_info->rotation = mutable_document->basic.pages[index].rotation;
  return clear_success(mutable_document->context);
}

nanopdf_status nanopdf_document_copy_info_value(
    nanopdf_document* document,
    nanopdf_info_key key,
    char** out_value) {
  const char* value = NULL;

  if (validate_document_handle(document, "document handle is null") != NANOPDF_STATUS_OK) {
    return NANOPDF_STATUS_INVALID_ARGUMENT;
  }
  if (!out_value) {
    return set_error(
        document->context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "metadata output pointer is null");
  }

  value = get_basic_info_value(&document->basic, key);
  if (!value && key >= NANOPDF_INFO_TITLE && key <= NANOPDF_INFO_TRAPPED) {
    value = "";
  }
  if (!value) {
    return set_error(
        document->context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "unknown metadata key");
  }

  return copy_string(document->context, value, out_value);
}

nanopdf_status nanopdf_document_copy_custom_info_value(
    nanopdf_document* document,
    const char* key,
    char** out_value) {
  const char* value = NULL;

  if (validate_document_handle(document, "document handle is null") != NANOPDF_STATUS_OK) {
    return NANOPDF_STATUS_INVALID_ARGUMENT;
  }
  if (!key || key[0] == '\0') {
    return set_error(
        document->context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "custom metadata key is null or empty");
  }
  if (!out_value) {
    return set_error(
        document->context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "custom metadata output pointer is null");
  }

  value = get_basic_custom_info_value(&document->basic, key);
  if (!value) {
    return set_error(
        document->context,
        NANOPDF_STATUS_NOT_FOUND,
        "custom metadata key was not found");
  }

  return copy_string(document->context, value, out_value);
}

nanopdf_status nanopdf_document_copy_language(
    const nanopdf_document* document,
    char** out_language) {
  nanopdf_document* mutable_document = (nanopdf_document*)document;

  if (validate_document_handle(mutable_document, "document handle is null") != NANOPDF_STATUS_OK) {
    return NANOPDF_STATUS_INVALID_ARGUMENT;
  }
  if (!out_language) {
    return set_error(
        mutable_document->context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "language output pointer is null");
  }
  if (!mutable_document->basic.language) {
    return set_error(
        mutable_document->context,
        NANOPDF_STATUS_NOT_FOUND,
        "document language was not found");
  }

  return copy_string(mutable_document->context, mutable_document->basic.language, out_language);
}

nanopdf_status nanopdf_document_copy_xmp_metadata(
    const nanopdf_document* document,
    char** out_xml) {
  nanopdf_document* mutable_document = (nanopdf_document*)document;

  if (validate_document_handle(mutable_document, "document handle is null") != NANOPDF_STATUS_OK) {
    return NANOPDF_STATUS_INVALID_ARGUMENT;
  }
  if (!out_xml) {
    return set_error(
        mutable_document->context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "XMP output pointer is null");
  }
  if (!mutable_document->basic.xmp_metadata) {
    return set_error(
        mutable_document->context,
        NANOPDF_STATUS_NOT_FOUND,
        "XMP metadata was not found");
  }

  return copy_string(mutable_document->context, mutable_document->basic.xmp_metadata, out_xml);
}

nanopdf_status nanopdf_document_copy_open_action_named_destination(
    const nanopdf_document* document,
    char** out_destination_name) {
  nanopdf_document* mutable_document = (nanopdf_document*)document;

  if (validate_document_handle(mutable_document, "document handle is null") != NANOPDF_STATUS_OK) {
    return NANOPDF_STATUS_INVALID_ARGUMENT;
  }
  if (!out_destination_name) {
    return set_error(
        mutable_document->context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "open action output pointer is null");
  }
  if (!mutable_document->basic.open_action_named_destination) {
    return set_error(
        mutable_document->context,
        NANOPDF_STATUS_NOT_FOUND,
        "open action named destination was not found");
  }

  return copy_string(
      mutable_document->context,
      mutable_document->basic.open_action_named_destination,
      out_destination_name);
}

nanopdf_status nanopdf_document_get_page_layout(
    const nanopdf_document* document,
    nanopdf_page_layout* out_layout) {
  nanopdf_document* mutable_document = (nanopdf_document*)document;

  if (validate_document_handle(mutable_document, "document handle is null") != NANOPDF_STATUS_OK) {
    return NANOPDF_STATUS_INVALID_ARGUMENT;
  }
  if (!out_layout) {
    return set_error(
        mutable_document->context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "page layout output pointer is null");
  }

  *out_layout = mutable_document->basic.page_layout;
  return clear_success(mutable_document->context);
}

nanopdf_status nanopdf_document_get_page_mode(
    const nanopdf_document* document,
    nanopdf_page_mode* out_mode) {
  nanopdf_document* mutable_document = (nanopdf_document*)document;

  if (validate_document_handle(mutable_document, "document handle is null") != NANOPDF_STATUS_OK) {
    return NANOPDF_STATUS_INVALID_ARGUMENT;
  }
  if (!out_mode) {
    return set_error(
        mutable_document->context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "page mode output pointer is null");
  }

  *out_mode = mutable_document->basic.page_mode;
  return clear_success(mutable_document->context);
}

nanopdf_status nanopdf_document_get_viewer_preferences(
    const nanopdf_document* document,
    nanopdf_viewer_preferences* out_preferences) {
  nanopdf_document* mutable_document = (nanopdf_document*)document;

  if (validate_document_handle(mutable_document, "document handle is null") != NANOPDF_STATUS_OK) {
    return NANOPDF_STATUS_INVALID_ARGUMENT;
  }
  if (!out_preferences) {
    return set_error(
        mutable_document->context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "viewer preferences output pointer is null");
  }
  if (!mutable_document->basic.has_viewer_preferences) {
    return set_error(
        mutable_document->context,
        NANOPDF_STATUS_NOT_FOUND,
        "viewer preferences were not found");
  }

  *out_preferences = mutable_document->basic.viewer_preferences;
  return clear_success(mutable_document->context);
}

nanopdf_status nanopdf_document_get_mark_info(
    const nanopdf_document* document,
    nanopdf_mark_info* out_mark_info) {
  nanopdf_document* mutable_document = (nanopdf_document*)document;

  if (validate_document_handle(mutable_document, "document handle is null") != NANOPDF_STATUS_OK) {
    return NANOPDF_STATUS_INVALID_ARGUMENT;
  }
  if (!out_mark_info) {
    return set_error(
        mutable_document->context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "mark info output pointer is null");
  }
  if (!mutable_document->basic.has_mark_info) {
    return set_error(
        mutable_document->context,
        NANOPDF_STATUS_NOT_FOUND,
        "MarkInfo was not found");
  }

  *out_mark_info = mutable_document->basic.mark_info;
  return clear_success(mutable_document->context);
}

uint32_t nanopdf_document_output_intent_count(const nanopdf_document* document) {
  const nanopdf_document* immutable_document = document;
  if (!immutable_document || !immutable_document->context) {
    return 0;
  }
  return (uint32_t)immutable_document->basic.output_intent_count;
}

nanopdf_status nanopdf_document_get_output_intent(
    const nanopdf_document* document,
    uint32_t index,
    nanopdf_output_intent* out_output_intent) {
  nanopdf_document* mutable_document = (nanopdf_document*)document;
  const size_t output_intent_index = (size_t)index;
  const nanopdf_basic_output_intent* source = NULL;

  if (validate_document_handle(mutable_document, "document handle is null") != NANOPDF_STATUS_OK) {
    return NANOPDF_STATUS_INVALID_ARGUMENT;
  }
  if (!out_output_intent) {
    return set_error(
        mutable_document->context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "output intent output pointer is null");
  }
  if (output_intent_index >= mutable_document->basic.output_intent_count) {
    return set_error(
        mutable_document->context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "output intent index is out of range");
  }

  source = &mutable_document->basic.output_intents[output_intent_index];
  memset(out_output_intent, 0, sizeof(*out_output_intent));
  out_output_intent->subtype = source->subtype;
  out_output_intent->output_condition = source->output_condition;
  out_output_intent->output_condition_identifier = source->output_condition_identifier;
  out_output_intent->registry_name = source->registry_name;
  out_output_intent->info = source->info;
  out_output_intent->dest_output_profile.data = source->dest_output_profile_data;
  out_output_intent->dest_output_profile.size = source->dest_output_profile_size;
  out_output_intent->color_components = source->color_components;
  return clear_success(mutable_document->context);
}

uint32_t nanopdf_document_page_label_count(const nanopdf_document* document) {
  const nanopdf_document* immutable_document = document;
  if (!immutable_document || !immutable_document->context) {
    return 0;
  }
  return (uint32_t)immutable_document->basic.page_label_count;
}

nanopdf_status nanopdf_document_get_page_label(
    const nanopdf_document* document,
    uint32_t index,
    uint32_t* out_page_index,
    nanopdf_page_label* out_label) {
  nanopdf_document* mutable_document = (nanopdf_document*)document;
  const size_t page_label_index = (size_t)index;
  const nanopdf_basic_page_label_entry* source = NULL;

  if (validate_document_handle(mutable_document, "document handle is null") != NANOPDF_STATUS_OK) {
    return NANOPDF_STATUS_INVALID_ARGUMENT;
  }
  if (!out_page_index) {
    return set_error(
        mutable_document->context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "page label index output pointer is null");
  }
  if (!out_label) {
    return set_error(
        mutable_document->context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "page label output pointer is null");
  }
  if (page_label_index >= mutable_document->basic.page_label_count) {
    return set_error(
        mutable_document->context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "page label index is out of range");
  }

  source = &mutable_document->basic.page_labels[page_label_index];
  *out_page_index = source->page_index;
  memset(out_label, 0, sizeof(*out_label));
  out_label->style = source->style;
  out_label->prefix = source->prefix;
  out_label->start_value = source->start_value;
  return clear_success(mutable_document->context);
}

uint32_t nanopdf_document_named_destination_count(const nanopdf_document* document) {
  const nanopdf_document* immutable_document = document;
  if (!immutable_document || !immutable_document->context) {
    return 0;
  }
  return (uint32_t)immutable_document->basic.named_destination_count;
}

nanopdf_status nanopdf_document_get_named_destination(
    const nanopdf_document* document,
    uint32_t index,
    nanopdf_named_destination* out_destination) {
  nanopdf_document* mutable_document = (nanopdf_document*)document;
  const size_t destination_index = (size_t)index;
  const nanopdf_basic_named_destination* source = NULL;

  if (validate_document_handle(mutable_document, "document handle is null") != NANOPDF_STATUS_OK) {
    return NANOPDF_STATUS_INVALID_ARGUMENT;
  }
  if (!out_destination) {
    return set_error(
        mutable_document->context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "named destination output pointer is null");
  }
  if (destination_index >= mutable_document->basic.named_destination_count) {
    return set_error(
        mutable_document->context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "named destination index is out of range");
  }

  source = &mutable_document->basic.named_destinations[destination_index];
  memset(out_destination, 0, sizeof(*out_destination));
  out_destination->name = source->name;
  out_destination->page_index = source->page_index;
  out_destination->fit_type = source->fit_type;
  out_destination->position = source->position;
  out_destination->position_count = source->position_count;
  return clear_success(mutable_document->context);
}

nanopdf_status nanopdf_page_extract_text(
    nanopdf_document* document,
    uint32_t page_index,
    char** out_text) {
  if (validate_document_handle(document, "document handle is null") != NANOPDF_STATUS_OK) {
    return NANOPDF_STATUS_INVALID_ARGUMENT;
  }
  if (!out_text) {
    return set_error(
        document->context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "text output pointer is null");
  }

  return nanopdf_basic_document_extract_text(
      document->context, &document->basic, page_index, out_text);
}

nanopdf_status nanopdf_page_extract_text_layout(
    nanopdf_document* document,
    uint32_t page_index,
    const nanopdf_text_layout_options* options,
    nanopdf_text_layout** out_layout) {
  nanopdf_text_layout* layout = NULL;
  char* text = NULL;
  const size_t index = (size_t)page_index;
  nanopdf_status status = NANOPDF_STATUS_OK;

  (void)options;

  if (validate_document_handle(document, "document handle is null") != NANOPDF_STATUS_OK) {
    return NANOPDF_STATUS_INVALID_ARGUMENT;
  }
  if (!out_layout) {
    return set_error(
        document->context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "layout output pointer is null");
  }
  if (index >= document->basic.page_count) {
    return set_error(
        document->context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "page index is out of range");
  }

  *out_layout = NULL;
  status = nanopdf_basic_document_extract_text(
      document->context, &document->basic, page_index, &text);
  if (status != NANOPDF_STATUS_OK) {
    return status;
  }

  layout = (nanopdf_text_layout*)nanopdf__allocator_alloc(
      &document->context->allocator, sizeof(*layout));
  if (!layout) {
    nanopdf_free(document->context, text);
    return set_error(
        document->context,
        NANOPDF_STATUS_OUT_OF_MEMORY,
        "failed to allocate text layout handle");
  }

  memset(layout, 0, sizeof(*layout));
  layout->context = document->context;
  status = nanopdf_basic_layout_build(
      document->context,
      text,
      document->basic.pages[index].width,
      document->basic.pages[index].height,
      &layout->basic);
  nanopdf_free(document->context, text);
  if (status != NANOPDF_STATUS_OK) {
    nanopdf_text_layout_destroy(layout);
    return status;
  }

  *out_layout = layout;
  return clear_success(document->context);
}

void nanopdf_text_layout_destroy(nanopdf_text_layout* layout) {
  if (!layout) {
    return;
  }

  nanopdf_basic_layout_destroy(&layout->context->allocator, &layout->basic);
  nanopdf__allocator_free(&layout->context->allocator, layout);
}

size_t nanopdf_text_layout_char_count(const nanopdf_text_layout* layout) {
  if (!layout) {
    return 0;
  }
  return layout->basic.char_count;
}

size_t nanopdf_text_layout_line_count(const nanopdf_text_layout* layout) {
  if (!layout) {
    return 0;
  }
  return layout->basic.line_count;
}

size_t nanopdf_text_layout_word_count(const nanopdf_text_layout* layout) {
  if (!layout) {
    return 0;
  }
  return layout->basic.word_count;
}

nanopdf_status nanopdf_text_layout_get_char(
    const nanopdf_text_layout* layout,
    size_t index,
    nanopdf_text_char* out_char) {
  const nanopdf_basic_layout_char* basic_char = NULL;

  if (!layout || !out_char) {
    return set_error(
        layout ? layout->context : NULL,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "invalid text layout arguments");
  }
  if (index >= layout->basic.char_count) {
    return set_error(
        layout->context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "text char index is out of range");
  }

  basic_char = &layout->basic.chars[index];
  memset(out_char, 0, sizeof(*out_char));
  out_char->unicode = basic_char->unicode;
  out_char->x = basic_char->x;
  out_char->y = basic_char->y;
  out_char->width = basic_char->width;
  out_char->height = basic_char->height;
  out_char->font_size = basic_char->height;
  out_char->line_index = basic_char->line_index;
  out_char->word_index = basic_char->word_index;
  out_char->matrix[0] = 1.0;
  out_char->matrix[3] = 1.0;
  return clear_success(layout->context);
}

nanopdf_status nanopdf_text_layout_copy_text(
    nanopdf_text_layout* layout,
    char** out_text) {
  if (!layout || !out_text) {
    return set_error(
        layout ? layout->context : NULL,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "invalid text layout arguments");
  }

  return copy_string(layout->context, layout->basic.text, out_text);
}

nanopdf_status nanopdf_text_layout_copy_text_in_rect(
    nanopdf_text_layout* layout,
    double x1,
    double y1,
    double x2,
    double y2,
    char** out_text) {
  if (!layout || !out_text) {
    return set_error(
        layout ? layout->context : NULL,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "invalid text layout arguments");
  }

  return nanopdf_basic_layout_copy_text_in_rect(
      layout->context, &layout->basic, x1, y1, x2, y2, out_text);
}

size_t nanopdf_document_form_field_count(const nanopdf_document* document) {
  nanopdf_document* mutable_document = (nanopdf_document*)document;

  if (!mutable_document || !mutable_document->context) {
    return 0;
  }

  clear_success(mutable_document->context);
  return mutable_document->basic.form_field_count;
}

nanopdf_status nanopdf_document_get_form_field_info(
    nanopdf_document* document,
    size_t field_index,
    nanopdf_form_field_info* out_field_info) {
  const nanopdf_basic_form_field* field = NULL;

  if (validate_document_handle(document, "document handle is null") != NANOPDF_STATUS_OK) {
    return NANOPDF_STATUS_INVALID_ARGUMENT;
  }
  if (!out_field_info) {
    return set_error(
        document->context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "form field info output pointer is null");
  }
  if (field_index >= document->basic.form_field_count) {
    return set_error(
        document->context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "form field index is out of range");
  }

  field = &document->basic.form_fields[field_index];
  out_field_info->type = field->type;
  out_field_info->partial_name = field->partial_name ? field->partial_name : "";
  out_field_info->full_name = field->full_name ? field->full_name : "";
  out_field_info->alternate_name = field->alternate_name ? field->alternate_name : "";
  out_field_info->mapping_name = field->mapping_name ? field->mapping_name : "";
  out_field_info->flags = field->flags;
  return clear_success(document->context);
}

nanopdf_status nanopdf_document_copy_form_field_value(
    nanopdf_document* document,
    size_t field_index,
    char** out_value) {
  if (validate_document_handle(document, "document handle is null") != NANOPDF_STATUS_OK) {
    return NANOPDF_STATUS_INVALID_ARGUMENT;
  }
  if (!out_value) {
    return set_error(
        document->context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "form value output pointer is null");
  }
  if (field_index >= document->basic.form_field_count) {
    return set_error(
        document->context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "form field index is out of range");
  }

  return copy_string(
      document->context,
      document->basic.form_fields[field_index].value,
      out_value);
}

nanopdf_status nanopdf_document_set_text_field(
    nanopdf_document* document,
    const char* field_name,
    const char* value) {
  nanopdf_basic_form_field* field = NULL;

  if (validate_document_handle(document, "document handle is null") != NANOPDF_STATUS_OK) {
    return NANOPDF_STATUS_INVALID_ARGUMENT;
  }
  if (!field_name || !value) {
    return set_error(
        document->context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "text field arguments are null");
  }

  field = find_basic_field_by_name(document, field_name);
  if (!field) {
    return set_error(
        document->context,
        NANOPDF_STATUS_NOT_FOUND,
        "form field was not found");
  }
  if (field->type != NANOPDF_FIELD_TYPE_TEXT) {
    return set_error(
        document->context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "field is not a text field");
  }

  return replace_field_value(document, field, value);
}

nanopdf_status nanopdf_document_set_button_field(
    nanopdf_document* document,
    const char* field_name,
    int checked) {
  nanopdf_basic_form_field* field = NULL;

  if (validate_document_handle(document, "document handle is null") != NANOPDF_STATUS_OK) {
    return NANOPDF_STATUS_INVALID_ARGUMENT;
  }
  if (!field_name) {
    return set_error(
        document->context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "button field name is null");
  }

  field = find_basic_field_by_name(document, field_name);
  if (!field) {
    return set_error(
        document->context,
        NANOPDF_STATUS_NOT_FOUND,
        "form field was not found");
  }
  if (field->type != NANOPDF_FIELD_TYPE_BUTTON) {
    return set_error(
        document->context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "field is not a button field");
  }

  return replace_field_value(document, field, checked ? "Yes" : "Off");
}

nanopdf_status nanopdf_document_set_choice_field_indices(
    nanopdf_document* document,
    const char* field_name,
    const int32_t* indices,
    size_t index_count) {
  nanopdf_basic_form_field* field = NULL;
  char* selection = NULL;
  nanopdf_status status = NANOPDF_STATUS_OK;

  if (validate_document_handle(document, "document handle is null") != NANOPDF_STATUS_OK) {
    return NANOPDF_STATUS_INVALID_ARGUMENT;
  }
  if (!field_name || (index_count > 0 && !indices)) {
    return set_error(
        document->context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "choice field arguments are invalid");
  }

  field = find_basic_field_by_name(document, field_name);
  if (!field) {
    return set_error(
        document->context,
        NANOPDF_STATUS_NOT_FOUND,
        "form field was not found");
  }
  if (field->type != NANOPDF_FIELD_TYPE_CHOICE) {
    return set_error(
        document->context,
        NANOPDF_STATUS_INVALID_ARGUMENT,
        "field is not a choice field");
  }

  status = copy_indices_string(document, indices, index_count, &selection);
  if (status != NANOPDF_STATUS_OK) {
    return status;
  }

  nanopdf__allocator_free(&document->context->allocator, field->value);
  field->value = selection;
  return clear_success(document->context);
}
