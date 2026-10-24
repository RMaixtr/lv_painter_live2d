/*
 * pe_live.h - playback engine for PainterEngine "LiveD" binary models (format v1)
 *
 * SPDX-License-Identifier: MIT
 *
 * Derived from PainterEngine kernel/PX_LiveFramework.c,
 * Copyright (c) 2023 DBinary <matrixcascade@gmail.com>, MIT License,
 * https://github.com/matrixcascade/PainterEngine
 * Referenced upstream revision:
 * db7617ffe1ca83c414921726fc5222116d5b27dc
 *
 * Only the playback path of the original framework is kept: container parsing,
 * key-frame interpolation, bone/stretch transforms, the spring simulation of
 * elastic vertices and textured triangle rasterization. The editor, exporter,
 * PainterEngine object tree and surface layer were removed. Added on top:
 * per-layer runtime overrides (rotation, translation, texture, Action Tracer)
 * that let an application steer the character, e.g. from face tracking.
 *
 * The model buffer is read in place and must stay valid while the engine is
 * open: textures, triangle indices and animation frames are not copied (the
 * buffer may live in XIP flash). Only the mutable per-layer/per-vertex state is
 * allocated through the caller supplied allocator. PE_LIVE_FLAG_CACHE_TEXTURES
 * additionally copies texture pixels into allocator memory, which is much
 * faster to sample than flash.
 *
 * Output is a premultiplied RGBA8888 buffer that the caller composites itself.
 */
#ifndef PE_LIVE_H
#define PE_LIVE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PE_LIVE_ID_MAX_LEN 32

typedef struct pe_live pe_live_t;

typedef struct {
    void *(*alloc)(void *ctx, size_t size);
    void (*free)(void *ctx, void *ptr);
    void *ctx;
} pe_live_allocator_t;

typedef enum {
    PE_LIVE_OK = 0,
    PE_LIVE_ERR_ARGUMENT,  /* NULL pointer, misaligned buffer or bad index */
    PE_LIVE_ERR_MAGIC,     /* not a PainterEngine live binary */
    PE_LIVE_ERR_VERSION,   /* container version other than 1 */
    PE_LIVE_ERR_TRUNCATED, /* a record extends past the end of the buffer */
    PE_LIVE_ERR_CORRUPT,   /* counts, indices or frame payloads are inconsistent */
    PE_LIVE_ERR_NO_MEMORY,
} pe_live_result_t;

/* pe_live_open() flags */
#define PE_LIVE_FLAG_CACHE_TEXTURES (1u << 0) /* copy texture pixels into allocator memory */

typedef enum {
    PE_LIVE_UPDATE_NONE = 0,
    PE_LIVE_UPDATE_WRAPPED,  /* looping animation restarted from its first frame */
    PE_LIVE_UPDATE_FINISHED, /* non-looping animation reached its last frame */
} pe_live_update_result_t;

typedef enum {
    PE_LIVE_STATE_STOPPED = 0, /* rest pose, no animation selected or stopped */
    PE_LIVE_STATE_PLAYING,
    PE_LIVE_STATE_PAUSED,
    PE_LIVE_STATE_FINISHED, /* holds the end pose of the last frame */
} pe_live_state_t;

typedef struct {
    uint8_t *pixels; /* premultiplied RGBA8888 (R, G, B, A byte order), row-major */
    int32_t width;
    int32_t height;
    int32_t stride;  /* bytes per row, at least width * 4 */
    float scale;     /* model canvas units to target pixels */
    float offset_x;  /* target pixel position of the canvas origin */
    float offset_y;
} pe_live_target_t;

pe_live_result_t pe_live_open(const void *data, size_t size, const pe_live_allocator_t *allocator,
                              uint32_t flags, pe_live_t **out);
void pe_live_close(pe_live_t *live);
const char *pe_live_result_string(pe_live_result_t result);

int32_t pe_live_get_width(const pe_live_t *live);
int32_t pe_live_get_height(const pe_live_t *live);
size_t pe_live_get_memory_usage(const pe_live_t *live);

int32_t pe_live_get_animation_count(const pe_live_t *live);
const char *pe_live_get_animation_id(const pe_live_t *live, int32_t index);
uint32_t pe_live_get_animation_duration(const pe_live_t *live, int32_t index);
/* Case-insensitive lookup like PainterEngine; returns -1 when absent. */
int32_t pe_live_find_animation(const pe_live_t *live, const char *id);

/* Starts an animation. The transition from the current pose to the first
 * key frame is interpolated, so switching animations does not jump. */
bool pe_live_play(pe_live_t *live, int32_t index);
void pe_live_set_loop(pe_live_t *live, bool loop);
bool pe_live_get_loop(const pe_live_t *live);
void pe_live_pause(pe_live_t *live);  /* freezes time; pe_live_update() becomes a no-op */
void pe_live_resume(pe_live_t *live);
void pe_live_stop(pe_live_t *live);   /* back to the rest pose, runtime overrides cleared */
pe_live_state_t pe_live_get_state(const pe_live_t *live);
int32_t pe_live_get_current_animation(const pe_live_t *live); /* -1 when none */

/* Layers and textures by index, stable while the model is open. Lookups are
 * exact (case-sensitive) and return -1 when absent; an empty id never
 * matches (the format allows unnamed layers). */
int32_t pe_live_get_layer_count(const pe_live_t *live);
const char *pe_live_get_layer_id(const pe_live_t *live, int32_t index);
int32_t pe_live_find_layer(const pe_live_t *live, const char *id);
int32_t pe_live_get_texture_count(const pe_live_t *live);
const char *pe_live_get_texture_id(const pe_live_t *live, int32_t index);
int32_t pe_live_find_texture(const pe_live_t *live, const char *id);

/* Runtime overrides, applied on top of whatever the key frames (or the rest
 * pose) produce, so an animation keeps playing underneath:
 *  - rotation offset: the layer and all its descendants turn rigidly by
 *    degrees about the layer's animated pivot;
 *  - translation offset: the layer and its descendants move by (dx, dy)
 *    model canvas units;
 *  - texture: the layer is drawn with this texture instead of the one the
 *    current key frame selects; -1 returns to the key frame's choice.
 *    Bone-only layers (no mesh, no texture of their own) accept only -1.
 * Offsets take effect at the next pe_live_update(), textures at the next
 * pe_live_render(). A paused engine keeps its last pose. The setters return
 * false and change nothing for a bad index or a non-finite value. */
bool pe_live_set_layer_rotation_offset(pe_live_t *live, int32_t layer, float degrees);
bool pe_live_set_layer_translation_offset(pe_live_t *live, int32_t layer, float dx, float dy);
bool pe_live_set_layer_texture(pe_live_t *live, int32_t layer, int32_t texture);

/* Action Tracer region, model canvas units */
typedef struct {
    float x, y, width, height; /* vertices outside the rectangle do not move */
    float center_x, center_y;  /* rest centre, strictly inside the rectangle */
} pe_live_tracer_region_t;

/* Action Tracer override: PainterEngine's way to turn or nod a 2D face (the
 * "panc" deformation that key frames can also carry). Inside the region the
 * rest centre is dragged by (dx, dy) while the region's edges stay put, so
 * the middle of the face (eyes, nose, mouth) shifts towards the turn, that
 * side compresses and the outline stays - like a face drawn on a sphere that
 * turns. It applies to the layer and all its descendants, taking the place of
 * the key frames' own tracer there; dx = dy = 0 leaves them undeformed. The
 * drag is clamped so the centre stays 10 % of the region's size away from its
 * edges and no triangle folds over. region NULL removes the override. Returns
 * false and changes nothing for a bad index, a non-finite value or a centre
 * outside the region. Takes effect at the next pe_live_update(). */
bool pe_live_set_layer_tracer(pe_live_t *live, int32_t layer, const pe_live_tracer_region_t *region, float dx,
                              float dy);
void pe_live_clear_overrides(pe_live_t *live);

/* Advances the key-frame program and the physics by elapsed_ms. */
pe_live_update_result_t pe_live_update(pe_live_t *live, uint32_t elapsed_ms);
/* Composites the current pose into target. The target is not cleared. */
void pe_live_render(const pe_live_t *live, const pe_live_target_t *target);

#ifdef __cplusplus
}
#endif

#endif /* PE_LIVE_H */
