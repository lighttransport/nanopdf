/*
 * src/vg/backend_registry.c — Custom canvas backend registry
 *
 * Holds runtime-registered backend descriptors. The well-known backends
 * (SOFTWARE / BLEND2D / THORVG) bypass the registry — they are dispatched
 * directly from lui_canvas_init_backend() in canvas.c.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include <lightui/vg/canvas_backend.h>

#include <stddef.h>

#define LUI_CANVAS_BACKEND_CUSTOM_COUNT \
    (LUI_CANVAS_BACKEND_CUSTOM_LAST - LUI_CANVAS_BACKEND_CUSTOM_FIRST + 1)

static const lui_canvas_backend_desc_t *
    g_custom_backends[LUI_CANVAS_BACKEND_CUSTOM_COUNT];

int lui_canvas_register_custom_backend(const lui_canvas_backend_desc_t *desc)
{
    if (!desc || !desc->name || !desc->init) return LUI_ERR_INVALID;
    for (int i = 0; i < LUI_CANVAS_BACKEND_CUSTOM_COUNT; i++) {
        if (!g_custom_backends[i]) {
            g_custom_backends[i] = desc;
            return LUI_CANVAS_BACKEND_CUSTOM_FIRST + i;
        }
    }
    return LUI_ERR_NOMEM;
}

const lui_canvas_backend_desc_t *lui_canvas_backend_describe(int slot)
{
    if (slot < LUI_CANVAS_BACKEND_CUSTOM_FIRST ||
        slot > LUI_CANVAS_BACKEND_CUSTOM_LAST) return NULL;
    return g_custom_backends[slot - LUI_CANVAS_BACKEND_CUSTOM_FIRST];
}

void lui_canvas_reset_custom_backends(void)
{
    for (int i = 0; i < LUI_CANVAS_BACKEND_CUSTOM_COUNT; i++)
        g_custom_backends[i] = NULL;
}
