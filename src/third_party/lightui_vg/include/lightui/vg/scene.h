/*
 * lightui/scene.h — Ordered layer stack with dirty-region compositing
 *
 * A scene holds an ordered list of layers (back to front).  Compositing
 * paints only the dirty regions of each visible layer onto the target canvas,
 * and respects a frame-clock deadline for early termination (tearing).
 *
 * Dirty propagation model:
 *   1. Application modifies layer content →
 *        lui_scene_invalidate_layer(scene, idx, rect)
 *      which records the dirty rect in both the layer and the scene.
 *   2. lui_scene_composite() composites only dirty scene regions.
 *   3. lui_scene_flush_dirty() clears the scene dirty after presenting.
 *
 * Early-termination / tearing:
 *   If the frame clock expires mid-composite, the function returns before
 *   all layers are drawn.  The remaining layers stay dirty, and the next
 *   frame will pick up where this one left off.  The partial frame can be
 *   presented immediately — for smooth animation this produces tearing on
 *   the incomplete region, but keeps input latency low.
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#ifndef LIGHTUI_SCENE_H
#define LIGHTUI_SCENE_H

#include <lightui/layer.h>
#include "canvas.h"
#include <lightui/dirty.h>
#include <lightui/frame_clock.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LUI_SCENE_MAX_LAYERS 64

typedef struct {
    lui_layer_t *layers[LUI_SCENE_MAX_LAYERS];
    int          count;
    lui_dirty_t  dirty;   /* scene-space aggregate dirty */
} lui_scene_t;

/* Initialise a scene (stack allocation OK). */
void lui_scene_init(lui_scene_t *scene);

/*
 * Append a layer to the scene (drawn last = on top).
 * Returns the layer index, or -1 if the scene is full.
 */
int lui_scene_add_layer(lui_scene_t *scene, lui_layer_t *layer);

/* Remove a layer by index.  Remaining layers shift down. */
void lui_scene_remove_layer(lui_scene_t *scene, int index);

/*
 * Invalidate a rect in layer-local coordinates.
 * Marks both the layer and the corresponding scene-space region dirty.
 * Pass rect==NULL to invalidate the entire layer.
 */
void lui_scene_invalidate_layer(lui_scene_t *scene, int index,
                                 const lui_rect_t *rect);

/* Mark the entire scene dirty (forces full re-composite). */
void lui_scene_invalidate_all(lui_scene_t *scene);

/*
 * Composite all visible layers back-to-front onto @canvas.
 *
 * Only regions that intersect the scene's dirty list are painted.
 * Before compositing each layer, the frame clock is checked; if the
 * deadline has expired, compositing stops and the function returns.
 *
 * @clk  Frame clock (may be NULL to disable deadline checking).
 *
 * Returns the number of layers fully composited.  If the return value is
 * less than scene->count, some layers were skipped due to the deadline —
 * the scene's dirty list is NOT cleared for those layers.
 */
int lui_scene_composite(lui_scene_t *scene, lui_canvas_t *canvas,
                         lui_frame_clock_t *clk);

/*
 * Clear the scene dirty list after the frame has been presented.
 * Only call this after a complete composite (all layers painted).
 */
void lui_scene_flush_dirty(lui_scene_t *scene);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* LIGHTUI_SCENE_H */
