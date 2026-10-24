/*
 * pe_live.c - playback engine for PainterEngine "LiveD" binary models (format v1)
 *
 * SPDX-License-Identifier: MIT
 *
 * Derived from PainterEngine kernel/PX_LiveFramework.c,
 * Copyright (c) 2023 DBinary <matrixcascade@gmail.com>, MIT License,
 * https://github.com/matrixcascade/PainterEngine
 * Referenced upstream revision:
 * db7617ffe1ca83c414921726fc5222116d5b27dc
 *
 * See pe_live.h for the scope of the port. Behavioural notes:
 *  - the key-frame "virtual machine" (PX_LiveFrameworkUpdateVM/ExecuteInstr),
 *    the layer interpolation, key point propagation, the Action Tracer ("panc")
 *    deformation, child stretch and the elastic vertex spring are kept
 *    equation-for-equation;
 *  - rotation angles are in degrees, as in PainterEngine;
 *  - layers are composited back to front by their key point z (largest first),
 *    which is constant for a model, so the order is computed once;
 *  - rasterization was rewritten: pixel-centre scan conversion with affine
 *    texture mapping, alpha-weighted bilinear sampling and premultiplied
 *    compositing, so semi-transparent edges do not darken.
 */
#include "pe_live.h"

#include <math.h>
#include <string.h>

/* ------------------------------------------------------------------------- */
/* Container layout. Little-endian, every field naturally aligned to 4 bytes. */

#define PE_MAGIC          "PainterEngineLiveDBinary"
#define PE_MAGIC_LEN      24
#define PE_VERSION        1u
#define PE_MAX_CHILDREN   16
#define PE_MAX_LAYERS     256 /* PX_LIVEFRAMEWORK_MAX_SUPPORT_LAYER */
#define PE_MAX_DIMENSION  16384
#define PE_MAX_COUNT      65536
#define PE_ATOM_MS        50u
#define PE_DEG_TO_RAD     0.0174532925f
#define PE_MAX_ELAPSED_MS 1000u

typedef struct {
    float x, y, z;
} pe_vec3_t;

typedef struct {
    char id[PE_LIVE_ID_MAX_LEN];
    uint32_t version;
    int32_t width;
    int32_t height;
    int32_t layer_count;
    int32_t animation_count;
    int32_t texture_count;
} pe_file_header_t;

typedef struct {
    char id[PE_LIVE_ID_MAX_LEN];
    int32_t width;
    int32_t height;
    int32_t offset_x;
    int32_t offset_y;
} pe_file_texture_t; /* followed by width * height RGBA8888 texels */

typedef struct {
    char id[PE_LIVE_ID_MAX_LEN];
    int32_t parent;
    int32_t child[PE_MAX_CHILDREN]; /* -1 terminated */
    int32_t triangle_count;
    int32_t vertex_count;
    pe_vec3_t key_point;
    int32_t link_texture;
} pe_file_layer_t; /* followed by triangles then vertices */

typedef struct {
    int32_t index[3];
} pe_file_triangle_t;

typedef struct {
    pe_vec3_t source;
    pe_vec3_t current;
    pe_vec3_t normal;
    pe_vec3_t begin_translation;
    pe_vec3_t current_translation;
    pe_vec3_t end_translation;
    pe_vec3_t velocity;
    int32_t k;
    float u;
    float v;
} pe_file_vertex_t;

typedef struct {
    char id[PE_LIVE_ID_MAX_LEN];
    int32_t frame_count;
} pe_file_animation_t; /* followed by frame_count frames */

typedef struct {
    char id[PE_LIVE_ID_MAX_LEN];
    int32_t payload_size;
    uint32_t duration_ms;
} pe_file_frame_t; /* followed by one pe_file_payload_t (+ vertex translations) per layer */

typedef struct {
    pe_vec3_t translation; /* root layers only */
    float stretch;         /* child layers only */
    float rotation;        /* degrees */
    int32_t map_texture;
    int32_t vertex_count;
    pe_vec3_t impulse;
    float panc_x, panc_y, panc_width, panc_height;
    float panc_sx, panc_sy;
    float panc_end_x, panc_end_y;
    uint32_t reserved[32];
} pe_file_payload_t; /* followed by vertex_count pe_vec3_t translations */

#define PE_STATIC_ASSERT(cond, name) typedef char pe_static_assert_##name[(cond) ? 1 : -1]
PE_STATIC_ASSERT(sizeof(pe_file_header_t) == 56, header_size);
PE_STATIC_ASSERT(sizeof(pe_file_texture_t) == 48, texture_size);
PE_STATIC_ASSERT(sizeof(pe_file_layer_t) == 124, layer_size);
PE_STATIC_ASSERT(sizeof(pe_file_triangle_t) == 12, triangle_size);
PE_STATIC_ASSERT(sizeof(pe_file_vertex_t) == 96, vertex_size);
PE_STATIC_ASSERT(sizeof(pe_file_animation_t) == 36, animation_size);
PE_STATIC_ASSERT(sizeof(pe_file_frame_t) == 40, frame_size);
PE_STATIC_ASSERT(sizeof(pe_file_payload_t) == 200, payload_size);

/* ------------------------------------------------------------------------- */
/* Runtime state */

typedef struct {
    char id[PE_LIVE_ID_MAX_LEN + 1];
    const uint8_t *pixels; /* RGBA8888, straight alpha */
    int32_t width;
    int32_t height;
    int32_t offset_x; /* placement on the model canvas */
    int32_t offset_y;
} pe_texture_t;

typedef struct {
    pe_vec3_t source;
    pe_vec3_t current;
    pe_vec3_t begin_translation;
    pe_vec3_t current_translation;
    pe_vec3_t end_translation;
    pe_vec3_t velocity;
    int32_t k; /* spring constant, 0 = rigid */
} pe_vertex_t;

/* One Action Tracer warp (see pe_tracer_offset()), model canvas units */
typedef struct {
    float x, y, width, height; /* region; vertices outside it do not move */
    float sx, sy;              /* rest centre */
    float cx, cy;              /* where the centre is dragged to */
} pe_tracer_t;

typedef struct {
    char id[PE_LIVE_ID_MAX_LEN + 1];
    const pe_file_triangle_t *triangles;
    pe_vertex_t *vertices;
    int32_t triangle_count;
    int32_t vertex_count;
    int32_t parent; /* -1 for roots */
    int32_t child[PE_MAX_CHILDREN];
    int32_t child_count;
    int32_t link_texture;   /* texture bound in the editor, -1 for none */
    int32_t render_texture; /* texture selected by the current key frame */
    pe_vec3_t key_point;
    pe_vec3_t current_key_point;
    float begin_rotation, current_rotation, end_rotation;
    float begin_stretch, current_stretch, end_stretch;
    pe_vec3_t begin_translation, current_translation, end_translation;
    pe_vec3_t impulse;
    float panc_x, panc_y, panc_width, panc_height, panc_sx, panc_sy;
    float panc_begin_x, panc_begin_y, panc_current_x, panc_current_y, panc_end_x, panc_end_y;
    /* runtime overrides (pe_live_set_layer_*) */
    float rotation_offset;        /* degrees, for this layer's subtree */
    float subtree_rotation;       /* this and all ancestors' offsets, per update */
    pe_vec3_t translation_offset; /* canvas units, for this layer's subtree */
    int32_t texture_override;     /* -1: the key frame's texture */
    bool tracer_set;              /* Action Tracer override for this subtree */
    pe_tracer_t tracer;
    int32_t tracer_from;          /* per update: layer whose override governs this one, -1 none */
} pe_layer_t;

typedef struct {
    char id[PE_LIVE_ID_MAX_LEN + 1];
    int32_t frame_count;
    uint32_t total_duration_ms;
    const uint8_t *frames; /* first pe_file_frame_t */
} pe_animation_t;

struct pe_live {
    pe_live_allocator_t allocator;
    size_t memory_usage;
    int32_t width;
    int32_t height;
    int32_t texture_count;
    int32_t layer_count;
    int32_t animation_count;
    pe_texture_t *textures;
    pe_layer_t *layers;
    pe_vertex_t *vertex_pool;
    int32_t *draw_order;   /* layer indices, back to front */
    int32_t *update_order; /* layer indices, parents before children */
    pe_animation_t *animations;
    uint8_t *texture_cache;

    pe_live_state_t state;
    bool loop;
    int32_t animation;    /* current animation, -1 when none */
    int32_t ip;           /* next frame to execute */
    const uint8_t *frame; /* pe_file_frame_t at ip */
    uint32_t duration;    /* duration of the frame being interpolated */
    uint32_t elapsed;     /* time spent in that frame */
};

/* ------------------------------------------------------------------------- */
/* Small helpers */

static void *pe_alloc(pe_live_t *live, size_t size)
{
    void *p = live->allocator.alloc(live->allocator.ctx, size);
    if (p != NULL) {
        live->memory_usage += size;
    }
    return p;
}

static void pe_free(pe_live_t *live, void *ptr)
{
    if (ptr != NULL) {
        live->allocator.free(live->allocator.ctx, ptr);
    }
}

static inline pe_vec3_t pe_vec3(float x, float y, float z)
{
    pe_vec3_t v = { x, y, z };
    return v;
}

static inline pe_vec3_t pe_vec3_add(pe_vec3_t a, pe_vec3_t b)
{
    return pe_vec3(a.x + b.x, a.y + b.y, a.z + b.z);
}

static inline pe_vec3_t pe_vec3_sub(pe_vec3_t a, pe_vec3_t b)
{
    return pe_vec3(a.x - b.x, a.y - b.y, a.z - b.z);
}

static inline pe_vec3_t pe_vec3_scale(pe_vec3_t a, float m)
{
    return pe_vec3(a.x * m, a.y * m, a.z * m);
}

static inline float pe_vec3_dot(pe_vec3_t a, pe_vec3_t b)
{
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

static inline float pe_vec3_length(pe_vec3_t a)
{
    return sqrtf(pe_vec3_dot(a, a));
}

/* PX_PointNormalization: the zero vector stays zero. */
static inline pe_vec3_t pe_vec3_normalize(pe_vec3_t a)
{
    float len = pe_vec3_length(a);
    return len > 0.0f ? pe_vec3_scale(a, 1.0f / len) : a;
}

/* PX_PointRotate: rotation about z by an angle in degrees. */
static inline pe_vec3_t pe_vec3_rotate_z(pe_vec3_t p, float sin_a, float cos_a)
{
    return pe_vec3(p.x * cos_a - p.y * sin_a, p.x * sin_a + p.y * cos_a, p.z);
}

static void pe_sincos_deg(float degrees, float *sin_a, float *cos_a)
{
    /* PX_sin_angle reduces the angle to one turn before converting */
    degrees -= (float)((int32_t)degrees / 360) * 360.0f;
    float rad = degrees * PE_DEG_TO_RAD;
    *sin_a = sinf(rad);
    *cos_a = cosf(rad);
}

static inline float pe_lerp(float a, float b, float t)
{
    return a + (b - a) * t;
}

static inline bool pe_is_finite(float v)
{
    return (v - v) == 0.0f;
}

static inline int32_t pe_ceil_clamp(float v, int32_t limit)
{
    if (!(v > 0.0f)) {
        return 0; /* also catches NaN */
    }
    if (v >= (float)limit) {
        return limit;
    }
    return (int32_t)ceilf(v);
}

/* ------------------------------------------------------------------------- */
/* Container parsing */

typedef struct {
    const uint8_t *base;
    size_t size;
    size_t pos;
} pe_reader_t;

static const void *pe_reader_take(pe_reader_t *r, uint64_t bytes)
{
    if (bytes > (uint64_t)(r->size - r->pos)) {
        return NULL;
    }
    const void *p = r->base + r->pos;
    r->pos += (size_t)bytes;
    return p;
}

static void pe_copy_id(char *dst, const char *src)
{
    memcpy(dst, src, PE_LIVE_ID_MAX_LEN);
    dst[PE_LIVE_ID_MAX_LEN] = '\0';
}

static pe_live_result_t pe_parse_textures(pe_live_t *live, pe_reader_t *r)
{
    for (int32_t i = 0; i < live->texture_count; i++) {
        const pe_file_texture_t *rec = pe_reader_take(r, sizeof(*rec));
        if (rec == NULL) {
            return PE_LIVE_ERR_TRUNCATED;
        }
        if (rec->width <= 0 || rec->height <= 0 || rec->width > PE_MAX_DIMENSION || rec->height > PE_MAX_DIMENSION) {
            return PE_LIVE_ERR_CORRUPT;
        }
        const uint8_t *pixels = pe_reader_take(r, (uint64_t)rec->width * (uint64_t)rec->height * 4u);
        if (pixels == NULL) {
            return PE_LIVE_ERR_TRUNCATED;
        }
        pe_texture_t *tex = &live->textures[i];
        pe_copy_id(tex->id, rec->id);
        tex->pixels = pixels;
        tex->width = rec->width;
        tex->height = rec->height;
        tex->offset_x = rec->offset_x;
        tex->offset_y = rec->offset_y;
    }
    return PE_LIVE_OK;
}

/* Reads one layer record with its triangles and vertices, validating counts,
 * parent/child indices and triangle indices. */
static pe_live_result_t pe_read_layer(const pe_live_t *live, pe_reader_t *r, int32_t index,
                                      const pe_file_layer_t **rec_out, const pe_file_triangle_t **tri_out,
                                      const pe_file_vertex_t **vert_out)
{
    const pe_file_layer_t *rec = pe_reader_take(r, sizeof(*rec));
    if (rec == NULL) {
        return PE_LIVE_ERR_TRUNCATED;
    }
    if (rec->triangle_count < 0 || rec->triangle_count > PE_MAX_COUNT ||
        rec->vertex_count < 0 || rec->vertex_count > PE_MAX_COUNT ||
        rec->parent < -1 || rec->parent >= live->layer_count || rec->parent == index) {
        return PE_LIVE_ERR_CORRUPT;
    }
    const pe_file_triangle_t *triangles = pe_reader_take(r, (uint64_t)rec->triangle_count * sizeof(*triangles));
    if (triangles == NULL) {
        return PE_LIVE_ERR_TRUNCATED;
    }
    const pe_file_vertex_t *vertices = pe_reader_take(r, (uint64_t)rec->vertex_count * sizeof(*vertices));
    if (vertices == NULL) {
        return PE_LIVE_ERR_TRUNCATED;
    }
    for (int32_t t = 0; t < rec->triangle_count; t++) {
        for (int k = 0; k < 3; k++) {
            if (triangles[t].index[k] < 0 || triangles[t].index[k] >= rec->vertex_count) {
                return PE_LIVE_ERR_CORRUPT;
            }
        }
    }
    /* PainterEngine walks the child list up to the first -1 */
    for (int c = 0; c < PE_MAX_CHILDREN; c++) {
        int32_t child = rec->child[c];
        if (child == -1) {
            break;
        }
        if (child < 0 || child >= live->layer_count || child == index) {
            return PE_LIVE_ERR_CORRUPT;
        }
    }
    *rec_out = rec;
    *tri_out = triangles;
    *vert_out = vertices;
    return PE_LIVE_OK;
}

static pe_live_result_t pe_parse_layers(pe_live_t *live, pe_reader_t *r)
{
    const size_t start = r->pos;
    const pe_file_layer_t *rec;
    const pe_file_triangle_t *triangles;
    const pe_file_vertex_t *vertices;
    int64_t total_vertices = 0;

    /* first pass: validate and count the vertices that need mutable state */
    for (int32_t i = 0; i < live->layer_count; i++) {
        pe_live_result_t res = pe_read_layer(live, r, i, &rec, &triangles, &vertices);
        if (res != PE_LIVE_OK) {
            return res;
        }
        total_vertices += rec->vertex_count;
    }

    if (total_vertices > 0) {
        live->vertex_pool = pe_alloc(live, (size_t)total_vertices * sizeof(pe_vertex_t));
        if (live->vertex_pool == NULL) {
            return PE_LIVE_ERR_NO_MEMORY;
        }
        memset(live->vertex_pool, 0, (size_t)total_vertices * sizeof(pe_vertex_t));
    }

    /* second pass: fill the runtime layers */
    r->pos = start;
    pe_vertex_t *slot = live->vertex_pool;
    for (int32_t i = 0; i < live->layer_count; i++) {
        pe_live_result_t res = pe_read_layer(live, r, i, &rec, &triangles, &vertices);
        if (res != PE_LIVE_OK) {
            return res;
        }
        pe_layer_t *layer = &live->layers[i];
        memset(layer, 0, sizeof(*layer));
        pe_copy_id(layer->id, rec->id);
        layer->texture_override = -1;
        layer->tracer_from = -1;
        layer->triangles = triangles;
        layer->triangle_count = rec->triangle_count;
        layer->vertex_count = rec->vertex_count;
        layer->parent = rec->parent;
        for (int c = 0; c < PE_MAX_CHILDREN && rec->child[c] != -1; c++) {
            layer->child[layer->child_count++] = rec->child[c];
        }
        layer->key_point = rec->key_point;
        layer->current_key_point = rec->key_point;
        layer->link_texture = (rec->link_texture >= 0 && rec->link_texture < live->texture_count) ? rec->link_texture : -1;
        layer->render_texture = layer->link_texture;
        layer->vertices = slot;
        for (int32_t v = 0; v < rec->vertex_count; v++) {
            slot[v].source = vertices[v].source;
            slot[v].current = vertices[v].source;
            slot[v].k = vertices[v].k > 0 ? vertices[v].k : 0;
        }
        slot += rec->vertex_count;
    }
    return PE_LIVE_OK;
}

/* Parents before children, roots first; mirrors the recursive key point update
 * of PainterEngine without recursion. Layers that no root reaches are appended
 * so they still follow their parent. */
static pe_live_result_t pe_build_update_order(pe_live_t *live)
{
    int32_t count = live->layer_count;
    uint8_t visited[PE_MAX_LAYERS];
    int32_t stack[PE_MAX_LAYERS];
    int32_t n = 0;

    memset(visited, 0, sizeof(visited));
    for (int32_t root = 0; root < count; root++) {
        if (live->layers[root].parent != -1 || visited[root]) {
            continue;
        }
        int32_t sp = 0;
        stack[sp++] = root;
        visited[root] = 1;
        while (sp > 0) {
            int32_t index = stack[--sp];
            live->update_order[n++] = index;
            const pe_layer_t *layer = &live->layers[index];
            /* push in reverse so children are visited in list order */
            for (int32_t c = layer->child_count - 1; c >= 0; c--) {
                int32_t child = layer->child[c];
                if (!visited[child]) {
                    if (sp >= count) {
                        return PE_LIVE_ERR_CORRUPT;
                    }
                    visited[child] = 1;
                    stack[sp++] = child;
                }
            }
        }
    }
    for (int32_t i = 0; i < count; i++) {
        if (!visited[i]) {
            live->update_order[n++] = i;
        }
    }
    return PE_LIVE_OK;
}

/* Stable sort by key point z, largest (farthest) first. */
static void pe_build_draw_order(pe_live_t *live)
{
    for (int32_t i = 0; i < live->layer_count; i++) {
        int32_t j = i;
        float z = live->layers[i].key_point.z;
        while (j > 0 && live->layers[live->draw_order[j - 1]].key_point.z < z) {
            live->draw_order[j] = live->draw_order[j - 1];
            j--;
        }
        live->draw_order[j] = i;
    }
}

static pe_live_result_t pe_validate_payload(const pe_live_t *live, const uint8_t *payload, int32_t payload_size)
{
    size_t offset = 0;
    size_t size = (size_t)payload_size;

    for (int32_t i = 0; i < live->layer_count; i++) {
        if (size - offset < sizeof(pe_file_payload_t)) {
            return PE_LIVE_ERR_CORRUPT;
        }
        const pe_file_payload_t *p = (const pe_file_payload_t *)(payload + offset);
        if (p->vertex_count != live->layers[i].vertex_count) {
            return PE_LIVE_ERR_CORRUPT;
        }
        offset += sizeof(pe_file_payload_t);
        size_t translations = (size_t)p->vertex_count * sizeof(pe_vec3_t);
        if (size - offset < translations) {
            return PE_LIVE_ERR_CORRUPT;
        }
        offset += translations;
    }
    return offset == size ? PE_LIVE_OK : PE_LIVE_ERR_CORRUPT;
}

static pe_live_result_t pe_parse_animations(pe_live_t *live, pe_reader_t *r)
{
    for (int32_t i = 0; i < live->animation_count; i++) {
        const pe_file_animation_t *rec = pe_reader_take(r, sizeof(*rec));
        if (rec == NULL) {
            return PE_LIVE_ERR_TRUNCATED;
        }
        if (rec->frame_count < 0 || rec->frame_count > PE_MAX_COUNT) {
            return PE_LIVE_ERR_CORRUPT;
        }
        pe_animation_t *anim = &live->animations[i];
        pe_copy_id(anim->id, rec->id);
        anim->frame_count = rec->frame_count;
        anim->frames = r->base + r->pos;
        anim->total_duration_ms = 0;
        for (int32_t f = 0; f < rec->frame_count; f++) {
            const pe_file_frame_t *frame = pe_reader_take(r, sizeof(*frame));
            if (frame == NULL) {
                return PE_LIVE_ERR_TRUNCATED;
            }
            if (frame->payload_size < 0) {
                return PE_LIVE_ERR_CORRUPT;
            }
            const uint8_t *payload = pe_reader_take(r, (uint64_t)frame->payload_size);
            if (payload == NULL) {
                return PE_LIVE_ERR_TRUNCATED;
            }
            pe_live_result_t res = pe_validate_payload(live, payload, frame->payload_size);
            if (res != PE_LIVE_OK) {
                return res;
            }
            anim->total_duration_ms += frame->duration_ms;
        }
    }
    return PE_LIVE_OK;
}

static pe_live_result_t pe_cache_textures(pe_live_t *live)
{
    uint64_t total = 0;
    for (int32_t i = 0; i < live->texture_count; i++) {
        total += (uint64_t)live->textures[i].width * (uint64_t)live->textures[i].height * 4u;
    }
    if (total == 0) {
        return PE_LIVE_OK;
    }
    if (total > (uint64_t)((size_t)-1)) {
        return PE_LIVE_ERR_NO_MEMORY;
    }
    live->texture_cache = pe_alloc(live, (size_t)total);
    if (live->texture_cache == NULL) {
        return PE_LIVE_ERR_NO_MEMORY;
    }
    uint8_t *dst = live->texture_cache;
    for (int32_t i = 0; i < live->texture_count; i++) {
        pe_texture_t *tex = &live->textures[i];
        size_t bytes = (size_t)tex->width * (size_t)tex->height * 4u;
        memcpy(dst, tex->pixels, bytes);
        tex->pixels = dst;
        dst += bytes;
    }
    return PE_LIVE_OK;
}

pe_live_result_t pe_live_open(const void *data, size_t size, const pe_live_allocator_t *allocator,
                              uint32_t flags, pe_live_t **out)
{
    if (out == NULL) {
        return PE_LIVE_ERR_ARGUMENT;
    }
    *out = NULL;
    if (data == NULL || allocator == NULL || allocator->alloc == NULL || allocator->free == NULL ||
        ((uintptr_t)data & 3u) != 0) {
        return PE_LIVE_ERR_ARGUMENT;
    }

    pe_reader_t reader = { (const uint8_t *)data, size, 0 };
    const char *magic = pe_reader_take(&reader, PE_MAGIC_LEN);
    if (magic == NULL) {
        return PE_LIVE_ERR_TRUNCATED;
    }
    if (memcmp(magic, PE_MAGIC, PE_MAGIC_LEN) != 0) {
        return PE_LIVE_ERR_MAGIC;
    }
    const pe_file_header_t *header = pe_reader_take(&reader, sizeof(*header));
    if (header == NULL) {
        return PE_LIVE_ERR_TRUNCATED;
    }
    if (header->version != PE_VERSION) {
        return PE_LIVE_ERR_VERSION;
    }
    if (header->width <= 0 || header->height <= 0 || header->width > PE_MAX_DIMENSION || header->height > PE_MAX_DIMENSION ||
        header->layer_count < 0 || header->layer_count > PE_MAX_LAYERS ||
        header->texture_count < 0 || header->texture_count > PE_MAX_COUNT ||
        header->animation_count < 0 || header->animation_count > PE_MAX_COUNT) {
        return PE_LIVE_ERR_CORRUPT;
    }

    pe_live_t *live = allocator->alloc(allocator->ctx, sizeof(*live));
    if (live == NULL) {
        return PE_LIVE_ERR_NO_MEMORY;
    }
    memset(live, 0, sizeof(*live));
    live->allocator = *allocator;
    live->memory_usage = sizeof(*live);
    live->width = header->width;
    live->height = header->height;
    live->texture_count = header->texture_count;
    live->layer_count = header->layer_count;
    live->animation_count = header->animation_count;
    live->animation = -1;

    pe_live_result_t res = PE_LIVE_OK;
    if (live->texture_count > 0) {
        live->textures = pe_alloc(live, (size_t)live->texture_count * sizeof(pe_texture_t));
        if (live->textures == NULL) {
            res = PE_LIVE_ERR_NO_MEMORY;
            goto fail;
        }
    }
    if (live->layer_count > 0) {
        live->layers = pe_alloc(live, (size_t)live->layer_count * sizeof(pe_layer_t));
        live->draw_order = pe_alloc(live, (size_t)live->layer_count * sizeof(int32_t));
        live->update_order = pe_alloc(live, (size_t)live->layer_count * sizeof(int32_t));
        if (live->layers == NULL || live->draw_order == NULL || live->update_order == NULL) {
            res = PE_LIVE_ERR_NO_MEMORY;
            goto fail;
        }
    }
    if (live->animation_count > 0) {
        live->animations = pe_alloc(live, (size_t)live->animation_count * sizeof(pe_animation_t));
        if (live->animations == NULL) {
            res = PE_LIVE_ERR_NO_MEMORY;
            goto fail;
        }
    }

    if ((res = pe_parse_textures(live, &reader)) != PE_LIVE_OK ||
        (res = pe_parse_layers(live, &reader)) != PE_LIVE_OK ||
        (res = pe_build_update_order(live)) != PE_LIVE_OK ||
        (res = pe_parse_animations(live, &reader)) != PE_LIVE_OK) {
        goto fail;
    }
    pe_build_draw_order(live);

    if ((flags & PE_LIVE_FLAG_CACHE_TEXTURES) != 0 && (res = pe_cache_textures(live)) != PE_LIVE_OK) {
        goto fail;
    }

    pe_live_stop(live);
    *out = live;
    return PE_LIVE_OK;

fail:
    pe_live_close(live);
    return res;
}

void pe_live_close(pe_live_t *live)
{
    if (live == NULL) {
        return;
    }
    pe_free(live, live->texture_cache);
    pe_free(live, live->animations);
    pe_free(live, live->update_order);
    pe_free(live, live->draw_order);
    pe_free(live, live->vertex_pool);
    pe_free(live, live->layers);
    pe_free(live, live->textures);
    live->allocator.free(live->allocator.ctx, live);
}

const char *pe_live_result_string(pe_live_result_t result)
{
    switch (result) {
        case PE_LIVE_OK:
            return "ok";
        case PE_LIVE_ERR_ARGUMENT:
            return "invalid argument";
        case PE_LIVE_ERR_MAGIC:
            return "not a PainterEngine live model";
        case PE_LIVE_ERR_VERSION:
            return "unsupported live format version";
        case PE_LIVE_ERR_TRUNCATED:
            return "model data truncated";
        case PE_LIVE_ERR_CORRUPT:
            return "model data corrupt";
        case PE_LIVE_ERR_NO_MEMORY:
            return "out of memory";
        default:
            return "unknown error";
    }
}

/* ------------------------------------------------------------------------- */
/* Queries */

int32_t pe_live_get_width(const pe_live_t *live)
{
    return live != NULL ? live->width : 0;
}

int32_t pe_live_get_height(const pe_live_t *live)
{
    return live != NULL ? live->height : 0;
}

size_t pe_live_get_memory_usage(const pe_live_t *live)
{
    return live != NULL ? live->memory_usage : 0;
}

int32_t pe_live_get_animation_count(const pe_live_t *live)
{
    return live != NULL ? live->animation_count : 0;
}

const char *pe_live_get_animation_id(const pe_live_t *live, int32_t index)
{
    if (live == NULL || index < 0 || index >= live->animation_count) {
        return NULL;
    }
    return live->animations[index].id;
}

uint32_t pe_live_get_animation_duration(const pe_live_t *live, int32_t index)
{
    if (live == NULL || index < 0 || index >= live->animation_count) {
        return 0;
    }
    return live->animations[index].total_duration_ms;
}

static bool pe_id_equals_ignore_case(const char *a, const char *b)
{
    for (;; a++, b++) {
        char l = *a;
        char r = *b;
        if (l >= 'a' && l <= 'z') {
            l = (char)(l - 'a' + 'A');
        }
        if (r >= 'a' && r <= 'z') {
            r = (char)(r - 'a' + 'A');
        }
        if (l != r) {
            return false;
        }
        if (l == '\0') {
            return true;
        }
    }
}

int32_t pe_live_find_animation(const pe_live_t *live, const char *id)
{
    if (live == NULL || id == NULL) {
        return -1;
    }
    for (int32_t i = 0; i < live->animation_count; i++) {
        if (pe_id_equals_ignore_case(live->animations[i].id, id)) {
            return i;
        }
    }
    return -1;
}

int32_t pe_live_get_layer_count(const pe_live_t *live)
{
    return live != NULL ? live->layer_count : 0;
}

const char *pe_live_get_layer_id(const pe_live_t *live, int32_t index)
{
    if (live == NULL || index < 0 || index >= live->layer_count) {
        return NULL;
    }
    return live->layers[index].id;
}

int32_t pe_live_find_layer(const pe_live_t *live, const char *id)
{
    if (live == NULL || id == NULL || id[0] == '\0') {
        return -1;
    }
    for (int32_t i = 0; i < live->layer_count; i++) {
        if (strcmp(live->layers[i].id, id) == 0) {
            return i;
        }
    }
    return -1;
}

int32_t pe_live_get_texture_count(const pe_live_t *live)
{
    return live != NULL ? live->texture_count : 0;
}

const char *pe_live_get_texture_id(const pe_live_t *live, int32_t index)
{
    if (live == NULL || index < 0 || index >= live->texture_count) {
        return NULL;
    }
    return live->textures[index].id;
}

int32_t pe_live_find_texture(const pe_live_t *live, const char *id)
{
    if (live == NULL || id == NULL || id[0] == '\0') {
        return -1;
    }
    for (int32_t i = 0; i < live->texture_count; i++) {
        if (strcmp(live->textures[i].id, id) == 0) {
            return i;
        }
    }
    return -1;
}

/* ------------------------------------------------------------------------- */
/* Runtime overrides */

static bool pe_layer_index_valid(const pe_live_t *live, int32_t layer)
{
    return live != NULL && layer >= 0 && layer < live->layer_count;
}

bool pe_live_set_layer_rotation_offset(pe_live_t *live, int32_t layer, float degrees)
{
    if (!pe_layer_index_valid(live, layer) || !pe_is_finite(degrees)) {
        return false;
    }
    live->layers[layer].rotation_offset = degrees;
    return true;
}

bool pe_live_set_layer_translation_offset(pe_live_t *live, int32_t layer, float dx, float dy)
{
    if (!pe_layer_index_valid(live, layer) || !pe_is_finite(dx) || !pe_is_finite(dy)) {
        return false;
    }
    live->layers[layer].translation_offset = pe_vec3(dx, dy, 0.0f);
    return true;
}

bool pe_live_set_layer_texture(pe_live_t *live, int32_t layer, int32_t texture)
{
    if (!pe_layer_index_valid(live, layer) || texture < -1 || texture >= live->texture_count) {
        return false;
    }
    pe_layer_t *l = &live->layers[layer];
    if (texture >= 0 && l->triangle_count == 0 && l->link_texture < 0) {
        return false; /* a bone end: nothing would follow the texture */
    }
    l->texture_override = texture;
    return true;
}

/* The dragged centre stays this fraction of the region's size away from its
 * edges: the warp keeps the vertex order and no triangle folds over. */
#define PE_TRACER_MARGIN 0.1f

static float pe_clampf(float v, float lo, float hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

bool pe_live_set_layer_tracer(pe_live_t *live, int32_t layer, const pe_live_tracer_region_t *region, float dx,
                              float dy)
{
    if (!pe_layer_index_valid(live, layer)) {
        return false;
    }
    pe_layer_t *l = &live->layers[layer];
    if (region == NULL) {
        l->tracer_set = false;
        return true;
    }
    const float x = region->x, y = region->y, w = region->width, h = region->height;
    const float sx = region->center_x, sy = region->center_y;
    if (!pe_is_finite(x) || !pe_is_finite(y) || !pe_is_finite(w) || !pe_is_finite(h) || !pe_is_finite(sx) ||
        !pe_is_finite(sy) || !pe_is_finite(dx) || !pe_is_finite(dy) || !(w > 0.0f) || !(h > 0.0f) ||
        !(sx > x && sx < x + w) || !(sy > y && sy < y + h)) {
        return false;
    }
    l->tracer.x = x;
    l->tracer.y = y;
    l->tracer.width = w;
    l->tracer.height = h;
    l->tracer.sx = sx;
    l->tracer.sy = sy;
    l->tracer.cx = pe_clampf(sx + dx, x + w * PE_TRACER_MARGIN, x + w * (1.0f - PE_TRACER_MARGIN));
    l->tracer.cy = pe_clampf(sy + dy, y + h * PE_TRACER_MARGIN, y + h * (1.0f - PE_TRACER_MARGIN));
    l->tracer_set = true;
    return true;
}

void pe_live_clear_overrides(pe_live_t *live)
{
    if (live == NULL) {
        return;
    }
    for (int32_t i = 0; i < live->layer_count; i++) {
        pe_layer_t *layer = &live->layers[i];
        layer->rotation_offset = 0.0f;
        layer->subtree_rotation = 0.0f;
        layer->translation_offset = pe_vec3(0, 0, 0);
        layer->texture_override = -1;
        layer->tracer_set = false;
        layer->tracer_from = -1;
    }
}

/* ------------------------------------------------------------------------- */
/* Playback control */

bool pe_live_play(pe_live_t *live, int32_t index)
{
    if (live == NULL || index < 0 || index >= live->animation_count) {
        return false;
    }
    /* Like PX_LiveFrameworkPlayAnimation the interpolation registers and the
     * remaining time of the current frame are kept, so the pose glides into
     * the first key frame of the new animation. */
    live->animation = index;
    live->ip = 0;
    live->frame = live->animations[index].frames;
    live->state = PE_LIVE_STATE_PLAYING;
    return true;
}

void pe_live_set_loop(pe_live_t *live, bool loop)
{
    if (live != NULL) {
        live->loop = loop;
    }
}

bool pe_live_get_loop(const pe_live_t *live)
{
    return live != NULL && live->loop;
}

void pe_live_pause(pe_live_t *live)
{
    if (live != NULL && live->state == PE_LIVE_STATE_PLAYING) {
        live->state = PE_LIVE_STATE_PAUSED;
    }
}

void pe_live_resume(pe_live_t *live)
{
    if (live != NULL && live->state == PE_LIVE_STATE_PAUSED) {
        live->state = PE_LIVE_STATE_PLAYING;
    }
}

/* PX_LiveFrameworkReset */
void pe_live_stop(pe_live_t *live)
{
    if (live == NULL) {
        return;
    }
    live->state = PE_LIVE_STATE_STOPPED;
    live->animation = -1;
    live->ip = 0;
    live->frame = NULL;
    live->duration = 0;
    live->elapsed = 0;

    for (int32_t i = 0; i < live->layer_count; i++) {
        pe_layer_t *layer = &live->layers[i];
        layer->begin_rotation = layer->current_rotation = layer->end_rotation = 0.0f;
        layer->begin_stretch = layer->current_stretch = layer->end_stretch = 1.0f;
        layer->begin_translation = layer->current_translation = layer->end_translation = pe_vec3(0, 0, 0);
        layer->impulse = pe_vec3(0, 0, 0);
        layer->panc_x = layer->panc_y = layer->panc_width = layer->panc_height = 0.0f;
        layer->panc_sx = layer->panc_sy = 0.0f;
        layer->panc_begin_x = layer->panc_begin_y = 0.0f;
        layer->panc_current_x = layer->panc_current_y = 0.0f;
        layer->panc_end_x = layer->panc_end_y = 0.0f;
        layer->render_texture = layer->link_texture;
        layer->current_key_point = layer->key_point;
        for (int32_t v = 0; v < layer->vertex_count; v++) {
            pe_vertex_t *vertex = &layer->vertices[v];
            vertex->begin_translation = vertex->current_translation = vertex->end_translation = pe_vec3(0, 0, 0);
            vertex->current = vertex->source;
            vertex->velocity = pe_vec3(0, 0, 0);
        }
    }
    pe_live_clear_overrides(live);
}

pe_live_state_t pe_live_get_state(const pe_live_t *live)
{
    return live != NULL ? live->state : PE_LIVE_STATE_STOPPED;
}

int32_t pe_live_get_current_animation(const pe_live_t *live)
{
    return live != NULL ? live->animation : -1;
}

/* ------------------------------------------------------------------------- */
/* Key-frame program (PX_LiveFrameworkExecuteInstr / UpdateVM) */

static void pe_execute_frame(pe_live_t *live, const uint8_t *frame_ptr)
{
    const pe_file_frame_t *frame = (const pe_file_frame_t *)frame_ptr;
    const uint8_t *p = frame_ptr + sizeof(*frame);

    live->duration = frame->duration_ms;
    for (int32_t i = 0; i < live->layer_count; i++) {
        pe_layer_t *layer = &live->layers[i];
        const pe_file_payload_t *payload = (const pe_file_payload_t *)p;
        const pe_vec3_t *translations = (const pe_vec3_t *)(p + sizeof(*payload));
        p += sizeof(*payload) + (size_t)payload->vertex_count * sizeof(pe_vec3_t);

        layer->render_texture = payload->map_texture;

        layer->begin_rotation = layer->end_rotation;
        layer->current_rotation = layer->begin_rotation;
        layer->end_rotation = payload->rotation;

        layer->begin_stretch = layer->end_stretch;
        layer->current_stretch = layer->begin_stretch;
        layer->end_stretch = payload->stretch;

        layer->begin_translation = layer->end_translation;
        layer->current_translation = layer->begin_translation;
        layer->end_translation = payload->translation;

        layer->impulse = payload->impulse;

        layer->panc_x = payload->panc_x;
        layer->panc_y = payload->panc_y;
        layer->panc_width = payload->panc_width;
        layer->panc_height = payload->panc_height;
        layer->panc_sx = payload->panc_sx;
        layer->panc_sy = payload->panc_sy;
        layer->panc_begin_x = layer->panc_end_x;
        layer->panc_begin_y = layer->panc_end_y;
        layer->panc_current_x = layer->panc_begin_x;
        layer->panc_current_y = layer->panc_begin_y;
        layer->panc_end_x = payload->panc_end_x;
        layer->panc_end_y = payload->panc_end_y;

        for (int32_t v = 0; v < layer->vertex_count; v++) {
            pe_vertex_t *vertex = &layer->vertices[v];
            vertex->begin_translation = vertex->end_translation;
            vertex->current_translation = vertex->begin_translation;
            vertex->end_translation = translations[v];
        }
    }
}

static pe_live_update_result_t pe_update_program(pe_live_t *live, uint32_t elapsed_ms)
{
    pe_live_update_result_t result = PE_LIVE_UPDATE_NONE;
    const pe_animation_t *anim = &live->animations[live->animation];
    uint32_t executed = 0;

    live->elapsed += elapsed_ms;
    while (live->elapsed >= live->duration) {
        if (live->ip >= anim->frame_count) {
            if (live->loop && anim->frame_count > 0 && anim->total_duration_ms > 0) {
                live->ip = 0;
                live->frame = anim->frames;
                result = PE_LIVE_UPDATE_WRAPPED;
            } else {
                live->state = PE_LIVE_STATE_FINISHED;
                return PE_LIVE_UPDATE_FINISHED;
            }
        }
        if (executed++ > (uint32_t)anim->frame_count) {
            break; /* never spin more than one full pass per update */
        }
        const pe_file_frame_t *frame = (const pe_file_frame_t *)live->frame;
        uint32_t previous = live->duration;
        pe_execute_frame(live, live->frame);
        live->elapsed -= previous;
        live->ip++;
        live->frame += sizeof(*frame) + (size_t)frame->payload_size;
    }
    return result;
}

/* ------------------------------------------------------------------------- */
/* Physics (PX_LiveFrameworkUpdatePhysical) */

static void pe_interpolate_layer(pe_layer_t *layer, float schedule)
{
    layer->panc_current_x = pe_lerp(layer->panc_begin_x, layer->panc_end_x, schedule);
    layer->panc_current_y = pe_lerp(layer->panc_begin_y, layer->panc_end_y, schedule);

    for (int32_t v = 0; v < layer->vertex_count; v++) {
        pe_vertex_t *vertex = &layer->vertices[v];
        vertex->current_translation.x = pe_lerp(vertex->begin_translation.x, vertex->end_translation.x, schedule);
        vertex->current_translation.y = pe_lerp(vertex->begin_translation.y, vertex->end_translation.y, schedule);
    }

    if (layer->parent != -1) {
        layer->current_stretch = pe_lerp(layer->begin_stretch, layer->end_stretch, schedule);
    } else {
        layer->current_translation.x = pe_lerp(layer->begin_translation.x, layer->end_translation.x, schedule);
        layer->current_translation.y = pe_lerp(layer->begin_translation.y, layer->end_translation.y, schedule);
        layer->current_translation.z = 0.0f;
    }
    layer->current_rotation = pe_lerp(layer->begin_rotation, layer->end_rotation, schedule);
}

/* Layer rotations are absolute in this format: a child's pivot follows its
 * parent's angle, but its mesh only turns by its own. Turning a subtree
 * rigidly therefore adds the offset to every layer below it, so each layer
 * gets the sum of its own and its ancestors' offsets. Parents come first in
 * update_order. */
static void pe_apply_rotation_offsets(pe_live_t *live)
{
    for (int32_t n = 0; n < live->layer_count; n++) {
        pe_layer_t *layer = &live->layers[live->update_order[n]];
        float offset = layer->rotation_offset;
        if (layer->parent != -1) {
            offset += live->layers[layer->parent].subtree_rotation;
        }
        layer->subtree_rotation = offset;
        layer->current_rotation += offset;
    }
}

/* Which layer's tracer override governs each layer: its own, otherwise the one
 * its parent follows. Parents come first in update_order. */
static void pe_resolve_tracers(pe_live_t *live)
{
    for (int32_t n = 0; n < live->layer_count; n++) {
        const int32_t index = live->update_order[n];
        pe_layer_t *layer = &live->layers[index];
        if (layer->tracer_set) {
            layer->tracer_from = index;
        } else if (layer->parent != -1) {
            layer->tracer_from = live->layers[layer->parent].tracer_from;
        } else {
            layer->tracer_from = -1;
        }
    }
}

static void pe_update_key_points(pe_live_t *live)
{
    for (int32_t n = 0; n < live->layer_count; n++) {
        pe_layer_t *layer = &live->layers[live->update_order[n]];
        if (layer->parent != -1) {
            const pe_layer_t *parent = &live->layers[layer->parent];
            float sin_a, cos_a;
            pe_sincos_deg(parent->current_rotation, &sin_a, &cos_a);
            pe_vec3_t v = pe_vec3_scale(pe_vec3_sub(layer->key_point, parent->key_point), layer->current_stretch);
            v = pe_vec3_rotate_z(v, sin_a, cos_a);
            layer->current_key_point = pe_vec3_add(v, parent->current_key_point);
        } else {
            layer->current_key_point = pe_vec3_add(layer->key_point, layer->current_translation);
        }
        /* children build on this pivot, so the offset moves the whole subtree */
        layer->current_key_point.x += layer->translation_offset.x;
        layer->current_key_point.y += layer->translation_offset.y;
        layer->current_key_point.z = layer->key_point.z;
    }
}

/* Spring towards the target position; PainterEngine integrates in 50 ms
 * steps over one and a half times the elapsed interval. */
static void pe_simulate_spring(pe_vertex_t *vertex, pe_vec3_t target, float key_dir_x, float key_dir_y,
                               pe_vec3_t impulse, uint32_t elapsed_ms)
{
    const float k = (float)vertex->k;
    uint32_t remaining = elapsed_ms + elapsed_ms / 2u;
    float key_len = sqrtf(key_dir_x * key_dir_x + key_dir_y * key_dir_y);

    while (remaining > 0) {
        uint32_t atom = remaining > PE_ATOM_MS ? PE_ATOM_MS : remaining;
        remaining -= atom;
        float dt = (float)atom / 1000.0f;

        float toward_x = target.x - vertex->current.x;
        float toward_y = target.y - vertex->current.y;
        float distance = sqrtf(toward_x * toward_x + toward_y * toward_y);
        float dir_x = 0.0f, dir_y = 0.0f;
        if (distance > 0.0f) {
            dir_x = toward_x / distance;
            dir_y = toward_y / distance;
        }
        if (distance > k) {
            vertex->current.x = target.x - dir_x * k;
            vertex->current.y = target.y - dir_y * k;
            distance = k;
        }

        float inc_x = (dir_x * distance * distance + impulse.x) * dt;
        float inc_y = (dir_y * distance * distance + impulse.y) * dt;
        float vel_x = vertex->velocity.x + inc_x;
        float vel_y = vertex->velocity.y + inc_y;
        /* split into the component along the bone direction and the rest,
         * damped with different time constants */
        float radius = (vel_x * key_dir_x + vel_y * key_dir_y) / key_len;
        float along_x = radius * key_dir_x;
        float along_y = radius * key_dir_y;
        float across_x = vel_x - along_x;
        float across_y = vel_y - along_y;
        float damp_along = 1.0f - (float)atom / (k * 10.0f + 50.0f);
        float damp_across = 1.0f - (float)atom / (k * 30.0f + 50.0f);
        vertex->velocity.x = along_x * damp_along + across_x * damp_across;
        vertex->velocity.y = along_y * damp_along + across_y * damp_across;
        vertex->current.x += vertex->velocity.x * dt;
        vertex->current.y += vertex->velocity.y * dt;
    }
}

/* Action Tracer, PainterEngine's "panc" deformation, which its author's
 * article on the engine ("Live2D动画引擎的图形学原理及实现") presents as the way
 * to turn a face: imagine the face texture on a sphere that turns, so the
 * texture coordinates stay and only the vertices move - the middle of the face
 * shifts towards the turn, that side compresses, the outline stays. The
 * engine approximates the projection piecewise linearly: inside the region the
 * rest centre (sx, sy) is dragged to (cx, cy), the region's edges stay put and
 * each side of the centre is scaled to fit, separately along x and y. Works on
 * rest (source) coordinates; vertices outside the region do not move. */
static void pe_tracer_warp(const pe_tracer_t *t, pe_vec3_t s, pe_vec3_t *result)
{
    if (s.x > t->x && s.x < t->x + t->width) {
        if (s.x < t->sx) {
            float dis = s.x - t->x;
            float den = t->sx - t->x;
            if (den != 0.0f) {
                result->x += dis * (t->cx - t->x) / den - dis;
            }
        } else {
            float right = t->x + t->width;
            float dis = right - s.x;
            float den = right - t->sx;
            if (den != 0.0f) {
                result->x -= dis * (right - t->cx) / den - dis;
            }
        }
    }
    if (s.y > t->y && s.y < t->y + t->height) {
        if (s.y < t->sy) {
            float dis = s.y - t->y;
            float den = t->sy - t->y;
            if (den != 0.0f) {
                result->y += dis * (t->cy - t->y) / den - dis;
            }
        } else {
            float bottom = t->y + t->height;
            float dis = bottom - s.y;
            float den = bottom - t->sy;
            if (den != 0.0f) {
                result->y -= dis * (bottom - t->cy) / den - dis;
            }
        }
    }
}

/* The warp a layer follows this update: a runtime override on the layer or an
 * ancestor wins over the key frames' own. false: the centre is not dragged. */
static bool pe_layer_tracer(const pe_live_t *live, const pe_layer_t *layer, pe_tracer_t *t)
{
    if (layer->tracer_from >= 0) {
        *t = live->layers[layer->tracer_from].tracer;
    } else {
        t->x = layer->panc_x;
        t->y = layer->panc_y;
        t->width = layer->panc_width;
        t->height = layer->panc_height;
        t->sx = layer->panc_sx;
        t->sy = layer->panc_sy;
        t->cx = layer->panc_current_x;
        t->cy = layer->panc_current_y;
    }
    return t->cx != t->sx || t->cy != t->sy;
}

static void pe_update_layer_vertices(pe_live_t *live, pe_layer_t *layer, uint32_t elapsed_ms)
{
    /* Direction of the bone: towards the children, or down for leaves. */
    float key_dir_x = 0.0f, key_dir_y = 1.0f;
    if (layer->child_count > 0) {
        pe_vec3_t sum = pe_vec3(0, 0, 0);
        for (int32_t c = 0; c < layer->child_count; c++) {
            const pe_layer_t *child = &live->layers[layer->child[c]];
            pe_vec3_t v = pe_vec3_normalize(pe_vec3_sub(layer->current_key_point, child->current_key_point));
            sum.x += v.x;
            sum.y += v.y;
        }
        float len = sqrtf(sum.x * sum.x + sum.y * sum.y);
        if (len > 0.0f) {
            key_dir_x = sum.x / len;
            key_dir_y = sum.y / len;
        }
    }

    float sin_a, cos_a;
    pe_sincos_deg(layer->current_rotation, &sin_a, &cos_a);
    pe_tracer_t tracer;
    const bool traced = pe_layer_tracer(live, layer, &tracer);

    for (int32_t i = 0; i < layer->vertex_count; i++) {
        pe_vertex_t *vertex = &layer->vertices[i];
        pe_vec3_t result = pe_vec3_sub(vertex->source, layer->key_point);

        if (traced) {
            pe_tracer_warp(&tracer, vertex->source, &result);
        }

        result = pe_vec3_add(result, vertex->current_translation);

        /* stretch towards children that are being pulled */
        for (int32_t c = 0; c < layer->child_count; c++) {
            const pe_layer_t *child = &live->layers[layer->child[c]];
            if (child->current_stretch == 1.0f) {
                continue;
            }
            pe_vec3_t v1 = pe_vec3_sub(child->key_point, layer->key_point);
            float len1 = pe_vec3_length(v1);
            float len2 = pe_vec3_length(result);
            if (len1 == 0.0f || len2 == 0.0f) {
                continue;
            }
            float cos_v12 = pe_vec3_dot(v1, result) / len1 / len2;
            if (cos_v12 > 0.0f) {
                float distance = cos_v12 * len2;
                result = pe_vec3_add(result, pe_vec3_scale(pe_vec3_scale(v1, 1.0f / len1),
                                                           distance * (child->current_stretch - 1.0f)));
            }
        }

        result = pe_vec3_rotate_z(result, sin_a, cos_a);
        result.x += layer->current_key_point.x;
        result.y += layer->current_key_point.y;

        if (vertex->k == 0) {
            vertex->current = result;
        } else {
            pe_simulate_spring(vertex, result, key_dir_x, key_dir_y, layer->impulse, elapsed_ms);
        }
    }
}

pe_live_update_result_t pe_live_update(pe_live_t *live, uint32_t elapsed_ms)
{
    if (live == NULL || live->state == PE_LIVE_STATE_PAUSED) {
        return PE_LIVE_UPDATE_NONE;
    }
    if (elapsed_ms > PE_MAX_ELAPSED_MS) {
        elapsed_ms = PE_MAX_ELAPSED_MS;
    }

    pe_live_update_result_t result = PE_LIVE_UPDATE_NONE;
    if (live->state == PE_LIVE_STATE_PLAYING) {
        result = pe_update_program(live, elapsed_ms);
    }

    /* Stopped or finished poses snap to the end of the frame (schedule 1). */
    float schedule = 1.0f;
    if (live->state == PE_LIVE_STATE_PLAYING && live->duration != 0) {
        schedule = (float)live->elapsed / (float)live->duration;
        if (schedule > 1.0f) {
            schedule = 1.0f;
        }
    }

    for (int32_t i = 0; i < live->layer_count; i++) {
        pe_interpolate_layer(&live->layers[i], schedule);
    }
    /* current_rotation is re-interpolated every update, so the offsets
     * never accumulate */
    pe_apply_rotation_offsets(live);
    pe_update_key_points(live);
    pe_resolve_tracers(live);
    for (int32_t i = 0; i < live->layer_count; i++) {
        pe_update_layer_vertices(live, &live->layers[i], elapsed_ms);
    }
    return result;
}

/* ------------------------------------------------------------------------- */
/* Rasterization */

typedef struct {
    float x, y; /* target pixels */
    float u, v; /* texels */
} pe_raster_vertex_t;

static inline uint32_t pe_div255(uint32_t v)
{
    return (v * 0x8081u) >> 23; /* exact for v <= 65535 */
}

/* Alpha-weighted bilinear blend of four texels (8-bit fractions fx, fy),
 * composited "over" the premultiplied destination pixel. */
static inline void pe_blend_texels(uint8_t *dst, const uint8_t *p00, const uint8_t *p10, const uint8_t *p01,
                                   const uint8_t *p11, uint32_t fx, uint32_t fy)
{
    const uint32_t w00 = (256u - fx) * (256u - fy);
    const uint32_t w10 = fx * (256u - fy);
    const uint32_t w01 = (256u - fx) * fy;
    const uint32_t w11 = fx * fy;

    const uint32_t a00 = p00[3], a10 = p10[3], a01 = p01[3], a11 = p11[3];
    if ((a00 | a10 | a01 | a11) == 0) {
        return;
    }
    if ((a00 & a10 & a01 & a11) == 255u) {
        /* fully opaque neighbourhood: plain bilinear, replaces the destination */
        dst[0] = (uint8_t)((w00 * p00[0] + w10 * p10[0] + w01 * p01[0] + w11 * p11[0]) >> 16);
        dst[1] = (uint8_t)((w00 * p00[1] + w10 * p10[1] + w01 * p01[1] + w11 * p11[1]) >> 16);
        dst[2] = (uint8_t)((w00 * p00[2] + w10 * p10[2] + w01 * p01[2] + w11 * p11[2]) >> 16);
        dst[3] = 255u;
        return;
    }

    const uint32_t aw00 = w00 * a00, aw10 = w10 * a10, aw01 = w01 * a01, aw11 = w11 * a11;
    const uint32_t alpha = (aw00 + aw10 + aw01 + aw11) >> 16;
    if (alpha == 0) {
        return;
    }
    /* sums stay below 255 * 255 * 65536 < 2^32 */
    const uint32_t r = pe_div255((aw00 * p00[0] + aw10 * p10[0] + aw01 * p01[0] + aw11 * p11[0]) >> 16);
    const uint32_t g = pe_div255((aw00 * p00[1] + aw10 * p10[1] + aw01 * p01[1] + aw11 * p11[1]) >> 16);
    const uint32_t b = pe_div255((aw00 * p00[2] + aw10 * p10[2] + aw01 * p01[2] + aw11 * p11[2]) >> 16);
    const uint32_t inv = 255u - alpha;
    dst[0] = (uint8_t)(r + pe_div255(dst[0] * inv));
    dst[1] = (uint8_t)(g + pe_div255(dst[1] * inv));
    dst[2] = (uint8_t)(b + pe_div255(dst[2] * inv));
    dst[3] = (uint8_t)(alpha + pe_div255(dst[3] * inv));
}

/* Alpha-weighted bilinear sample at 16.16 texel coordinates (already offset by
 * half a texel) of a tw x th texture, composited "over" the premultiplied
 * destination pixel. Texels outside the texture count as transparent so
 * cut-out edges fade smoothly. */
static inline void pe_sample_blend(uint8_t *dst, const uint8_t *pixels, int32_t tw, int32_t th, int32_t u_fx,
                                   int32_t v_fx)
{
    static const uint8_t transparent[4] = { 0, 0, 0, 0 };
    const int32_t x0 = u_fx >> 16; /* arithmetic shift == floor */
    const int32_t y0 = v_fx >> 16;

    if (x0 < -1 || y0 < -1 || x0 >= tw || y0 >= th) {
        return;
    }

    const uint32_t fx = ((uint32_t)u_fx >> 8) & 0xffu;
    const uint32_t fy = ((uint32_t)v_fx >> 8) & 0xffu;

    const uint8_t *p00, *p10, *p01, *p11;
    if (x0 >= 0 && y0 >= 0 && x0 + 1 < tw && y0 + 1 < th) {
        p00 = pixels + ((size_t)y0 * (size_t)tw + (size_t)x0) * 4u;
        p10 = p00 + 4;
        p01 = p00 + (size_t)tw * 4u;
        p11 = p01 + 4;
    } else {
        const bool x0_ok = x0 >= 0;
        const bool x1_ok = x0 + 1 < tw;
        const bool y0_ok = y0 >= 0;
        const bool y1_ok = y0 + 1 < th;
        const uint8_t *row0 = pixels + (size_t)(y0_ok ? y0 : 0) * (size_t)tw * 4u;
        const uint8_t *row1 = pixels + (size_t)(y1_ok ? y0 + 1 : 0) * (size_t)tw * 4u;
        p00 = (x0_ok && y0_ok) ? row0 + (size_t)x0 * 4u : transparent;
        p10 = (x1_ok && y0_ok) ? row0 + (size_t)(x0 + 1) * 4u : transparent;
        p01 = (x0_ok && y1_ok) ? row1 + (size_t)x0 * 4u : transparent;
        p11 = (x1_ok && y1_ok) ? row1 + (size_t)(x0 + 1) * 4u : transparent;
    }
    pe_blend_texels(dst, p00, p10, p01, p11, fx, fy);
}

/* True when 16.16 coordinates running linearly from a to b keep every sample's
 * texel pair x0, x0 + 1 inside 0..size-1. */
static inline bool pe_span_inside(int64_t a, int64_t b, int32_t size)
{
    const int64_t lo = a < b ? a : b;
    const int64_t hi = a < b ? b : a;
    return lo >= 0 && (hi >> 16) + 1 < size;
}

/* Scan converts one textured triangle. Pixels are sampled at their centres and
 * spans are half open, so shared edges are drawn exactly once. */
static void pe_rasterize_triangle(const pe_live_target_t *target, const pe_texture_t *tex,
                                  const pe_raster_vertex_t *a, const pe_raster_vertex_t *b, const pe_raster_vertex_t *c)
{
    const uint8_t *const pixels = tex->pixels;
    const int32_t tw = tex->width;
    const int32_t th = tex->height;

    if (!pe_is_finite(a->x) || !pe_is_finite(a->y) || !pe_is_finite(b->x) || !pe_is_finite(b->y) ||
        !pe_is_finite(c->x) || !pe_is_finite(c->y)) {
        return;
    }
    const float det = (b->x - a->x) * (c->y - a->y) - (c->x - a->x) * (b->y - a->y);
    if (fabsf(det) < 1e-6f) {
        return;
    }
    const float inv_det = 1.0f / det;
    const float dudx = ((b->u - a->u) * (c->y - a->y) - (c->u - a->u) * (b->y - a->y)) * inv_det;
    const float dudy = ((c->u - a->u) * (b->x - a->x) - (b->u - a->u) * (c->x - a->x)) * inv_det;
    const float dvdx = ((b->v - a->v) * (c->y - a->y) - (c->v - a->v) * (b->y - a->y)) * inv_det;
    const float dvdy = ((c->v - a->v) * (b->x - a->x) - (b->v - a->v) * (c->x - a->x)) * inv_det;
    if (fabsf(dudx) > 16384.0f || fabsf(dvdx) > 16384.0f || fabsf(dudy) > 16384.0f || fabsf(dvdy) > 16384.0f) {
        return; /* needle-thin triangle, nothing sensible to draw */
    }

    const pe_raster_vertex_t *top = a, *mid = b, *bot = c, *swap;
    if (mid->y < top->y) { swap = top; top = mid; mid = swap; }
    if (bot->y < top->y) { swap = top; top = bot; bot = swap; }
    if (bot->y < mid->y) { swap = mid; mid = bot; bot = swap; }

    const int32_t y_begin = pe_ceil_clamp(top->y - 0.5f, target->height);
    const int32_t y_end = pe_ceil_clamp(bot->y - 0.5f, target->height);
    if (y_begin >= y_end) {
        return;
    }

    const float long_dxdy = (bot->x - top->x) / (bot->y - top->y);
    const float upper_dxdy = (mid->y > top->y) ? (mid->x - top->x) / (mid->y - top->y) : 0.0f;
    const float lower_dxdy = (bot->y > mid->y) ? (bot->x - mid->x) / (bot->y - mid->y) : 0.0f;
    const int32_t du_fx = (int32_t)lrintf(dudx * 65536.0f);
    const int32_t dv_fx = (int32_t)lrintf(dvdx * 65536.0f);

    for (int32_t iy = y_begin; iy < y_end; iy++) {
        const float cy = (float)iy + 0.5f;
        const float xa = top->x + (cy - top->y) * long_dxdy;
        const float xb = (cy < mid->y) ? top->x + (cy - top->y) * upper_dxdy : mid->x + (cy - mid->y) * lower_dxdy;
        const float xl = xa < xb ? xa : xb;
        const float xr = xa < xb ? xb : xa;
        const int32_t x_begin = pe_ceil_clamp(xl - 0.5f, target->width);
        const int32_t x_end = pe_ceil_clamp(xr - 0.5f, target->width);
        if (x_begin >= x_end) {
            continue;
        }

        const float px = (float)x_begin + 0.5f;
        const float u = a->u + dudx * (px - a->x) + dudy * (cy - a->y);
        const float v = a->v + dvdx * (px - a->x) + dvdy * (cy - a->y);
        if (fabsf(u) > 16384.0f || fabsf(v) > 16384.0f) {
            continue;
        }
        int32_t u_fx = (int32_t)floorf((u - 0.5f) * 65536.0f);
        int32_t v_fx = (int32_t)floorf((v - 0.5f) * 65536.0f);
        uint8_t *dst = target->pixels + (size_t)iy * (size_t)target->stride + (size_t)x_begin * 4u;
        const int64_t last = x_end - x_begin - 1;

        if (pe_span_inside(u_fx, u_fx + last * du_fx, tw) && pe_span_inside(v_fx, v_fx + last * dv_fx, th)) {
            /* both ends sample inside the texture, so every pixel between does */
            const size_t row_bytes = (size_t)tw * 4u;
            for (int32_t ix = x_begin; ix < x_end; ix++) {
                const uint8_t *p00 = pixels + (size_t)(v_fx >> 16) * row_bytes + (size_t)(u_fx >> 16) * 4u;
                pe_blend_texels(dst, p00, p00 + 4, p00 + row_bytes, p00 + row_bytes + 4, ((uint32_t)u_fx >> 8) & 0xffu,
                                ((uint32_t)v_fx >> 8) & 0xffu);
                dst += 4;
                u_fx += du_fx;
                v_fx += dv_fx;
            }
            continue;
        }
        for (int32_t ix = x_begin; ix < x_end; ix++) {
            pe_sample_blend(dst, pixels, tw, th, u_fx, v_fx);
            dst += 4;
            u_fx += du_fx;
            v_fx += dv_fx;
        }
    }
}

void pe_live_render(const pe_live_t *live, const pe_live_target_t *target)
{
    if (live == NULL || target == NULL || target->pixels == NULL || target->width <= 0 || target->height <= 0 ||
        target->stride < target->width * 4) {
        return;
    }

    for (int32_t n = 0; n < live->layer_count; n++) {
        const pe_layer_t *layer = &live->layers[live->draw_order[n]];
        const int32_t texture = layer->texture_override >= 0 ? layer->texture_override : layer->render_texture;
        if (texture < 0 || texture >= live->texture_count) {
            continue;
        }
        const pe_texture_t *tex = &live->textures[texture];

        if (layer->triangle_count > 0 && layer->vertex_count > 0) {
            for (int32_t t = 0; t < layer->triangle_count; t++) {
                pe_raster_vertex_t rv[3];
                for (int k = 0; k < 3; k++) {
                    const pe_vertex_t *vertex = &layer->vertices[layer->triangles[t].index[k]];
                    rv[k].x = target->offset_x + vertex->current.x * target->scale;
                    rv[k].y = target->offset_y + vertex->current.y * target->scale;
                    /* texel coordinates follow the texture the frame selected */
                    rv[k].u = vertex->source.x - (float)tex->offset_x;
                    rv[k].v = vertex->source.y - (float)tex->offset_y;
                }
                pe_rasterize_triangle(target, tex, &rv[0], &rv[1], &rv[2]);
            }
        } else {
            /* mesh-less layer: the texture is drawn unanimated at its offset */
            const float x0 = target->offset_x + (float)tex->offset_x * target->scale;
            const float y0 = target->offset_y + (float)tex->offset_y * target->scale;
            const float x1 = x0 + (float)tex->width * target->scale;
            const float y1 = y0 + (float)tex->height * target->scale;
            const pe_raster_vertex_t q0 = { x0, y0, 0.0f, 0.0f };
            const pe_raster_vertex_t q1 = { x1, y0, (float)tex->width, 0.0f };
            const pe_raster_vertex_t q2 = { x1, y1, (float)tex->width, (float)tex->height };
            const pe_raster_vertex_t q3 = { x0, y1, 0.0f, (float)tex->height };
            pe_rasterize_triangle(target, tex, &q0, &q1, &q2);
            pe_rasterize_triangle(target, tex, &q0, &q2, &q3);
        }
    }
}
