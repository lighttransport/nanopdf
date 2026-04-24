#include "nanopdf_c_internal.h"

#include <string.h>

static void nanopdf__resolve_allocator(
    const nanopdf_context_options* options,
    nanopdf_allocator* out_allocator) {
  nanopdf_context_options defaults;

  nanopdf_default_context_options(&defaults);
  *out_allocator = defaults.allocator;

  if (!options) {
    return;
  }

  if (options->allocator.user_data) {
    out_allocator->user_data = options->allocator.user_data;
  }
  if (options->allocator.alloc) {
    out_allocator->alloc = options->allocator.alloc;
  }
  if (options->allocator.realloc) {
    out_allocator->realloc = options->allocator.realloc;
  }
  if (options->allocator.free) {
    out_allocator->free = options->allocator.free;
  }
}

nanopdf_status nanopdf_context_create(
    const nanopdf_context_options* options,
    nanopdf_context** out_context) {
  nanopdf_allocator allocator;
  nanopdf_context* context;

  if (!out_context) {
    return NANOPDF_STATUS_INVALID_ARGUMENT;
  }

  *out_context = NULL;
  nanopdf__resolve_allocator(options, &allocator);
  context = (nanopdf_context*)nanopdf__allocator_alloc(&allocator, sizeof(*context));
  if (!context) {
    return NANOPDF_STATUS_OUT_OF_MEMORY;
  }

  memset(context, 0, sizeof(*context));
  context->allocator = allocator;
  context->last_status = NANOPDF_STATUS_OK;
  *out_context = context;
  return NANOPDF_STATUS_OK;
}

void nanopdf_context_destroy(nanopdf_context* context) {
  if (!context) {
    return;
  }

  nanopdf__allocator_free(&context->allocator, context->last_error);
  nanopdf__allocator_free(&context->allocator, context);
}

nanopdf_status nanopdf_context_last_status(const nanopdf_context* context) {
  if (!context) {
    return NANOPDF_STATUS_INVALID_ARGUMENT;
  }

  return context->last_status;
}

const char* nanopdf_context_last_error(const nanopdf_context* context) {
  if (!context || !context->last_error) {
    return "";
  }

  return context->last_error;
}

const nanopdf_allocator* nanopdf_context_get_allocator(
    const nanopdf_context* context) {
  if (!context) {
    return NULL;
  }

  return &context->allocator;
}

void nanopdf_free(nanopdf_context* context, void* ptr) {
  if (!context) {
    return;
  }

  nanopdf__allocator_free(&context->allocator, ptr);
}

uint32_t nanopdf_version_major(void) {
  return NANOPDF_C_VERSION_MAJOR;
}

uint32_t nanopdf_version_minor(void) {
  return NANOPDF_C_VERSION_MINOR;
}

uint32_t nanopdf_version_patch(void) {
  return NANOPDF_C_VERSION_PATCH;
}

void nanopdf__clear_error(nanopdf_context* context) {
  if (!context) {
    return;
  }

  context->last_status = NANOPDF_STATUS_OK;
  if (context->last_error) {
    context->last_error[0] = '\0';
  }
}

void nanopdf__set_error(
    nanopdf_context* context,
    nanopdf_status status,
    const char* message) {
  size_t required;
  char* storage;

  if (!context) {
    return;
  }

  context->last_status = status;
  if (!message) {
    message = "";
  }

  required = strlen(message) + 1;
  if (required > context->last_error_capacity) {
    storage = (char*)nanopdf__allocator_realloc(
        &context->allocator,
        context->last_error,
        required);
    if (!storage) {
      context->last_status = NANOPDF_STATUS_OUT_OF_MEMORY;
      return;
    }
    context->last_error = storage;
    context->last_error_capacity = required;
  }

  memcpy(context->last_error, message, required);
}

nanopdf_status nanopdf__copy_owned_string(
    nanopdf_context* context,
    const char* value,
    char** out_value) {
  size_t length;
  char* storage;

  if (!context || !out_value) {
    if (context) {
      nanopdf__set_error(
          context,
          NANOPDF_STATUS_INVALID_ARGUMENT,
          "output string pointer is null");
    }
    return NANOPDF_STATUS_INVALID_ARGUMENT;
  }

  *out_value = NULL;
  if (!value) {
    nanopdf__clear_error(context);
    return NANOPDF_STATUS_OK;
  }

  length = strlen(value) + 1;
  storage = (char*)nanopdf__allocator_alloc(&context->allocator, length);
  if (!storage) {
    nanopdf__set_error(
        context,
        NANOPDF_STATUS_OUT_OF_MEMORY,
        "failed to allocate output string");
    return NANOPDF_STATUS_OUT_OF_MEMORY;
  }

  memcpy(storage, value, length);
  *out_value = storage;
  nanopdf__clear_error(context);
  return NANOPDF_STATUS_OK;
}

char* nanopdf__strdup(
    const nanopdf_allocator* allocator,
    const char* value) {
  size_t length;
  char* storage;

  if (!allocator || !value) {
    return NULL;
  }

  length = strlen(value) + 1;
  storage = (char*)nanopdf__allocator_alloc(allocator, length);
  if (!storage) {
    return NULL;
  }

  memcpy(storage, value, length);
  return storage;
}
