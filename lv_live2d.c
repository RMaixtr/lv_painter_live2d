/*
 * lv_live2d.c - LVGL widget that plays PainterEngine "LiveD" models
 *
 * SPDX-License-Identifier: MIT
 */
#include "lv_live2d.h"

#include <string.h>

#define MY_CLASS &lv_live2d_class

static void lv_live2d_constructor(const lv_obj_class_t *class_p, lv_obj_t *obj);
static void lv_live2d_destructor(const lv_obj_class_t *class_p, lv_obj_t *obj);
static void lv_live2d_timer_cb(lv_timer_t *timer);
static void lv_live2d_release(lv_live2d_t *live2d);
static void lv_live2d_draw_frame(lv_live2d_t *live2d);

const lv_obj_class_t lv_live2d_class = {
    .constructor_cb = lv_live2d_constructor,
    .destructor_cb = lv_live2d_destructor,
    .instance_size = sizeof(lv_live2d_t),
    .base_class = &lv_img_class,
};

static void *lv_live2d_default_alloc(void *ctx, size_t size)
{
    LV_UNUSED(ctx);
    return lv_mem_alloc(size);
}

static void lv_live2d_default_free(void *ctx, void *ptr)
{
    LV_UNUSED(ctx);
    lv_mem_free(ptr);
}

lv_obj_t *lv_live2d_create(lv_obj_t *parent)
{
    LV_LOG_INFO("begin");
    lv_obj_t *obj = lv_obj_class_create_obj(MY_CLASS, parent);
    lv_obj_class_init_obj(obj);
    return obj;
}

void lv_live2d_set_allocator(lv_obj_t *obj, const pe_live_allocator_t *allocator)
{
    LV_ASSERT_OBJ(obj, MY_CLASS);
    lv_live2d_t *live2d = (lv_live2d_t *)obj;

    if (live2d->live != NULL) {
        LV_LOG_WARN("allocator must be set before the model");
        return;
    }
    if (allocator == NULL || allocator->alloc == NULL || allocator->free == NULL) {
        live2d->allocator.alloc = lv_live2d_default_alloc;
        live2d->allocator.free = lv_live2d_default_free;
        live2d->allocator.ctx = NULL;
    } else {
        live2d->allocator = *allocator;
    }
}

void lv_live2d_set_background(lv_obj_t *obj, lv_color_t color)
{
    LV_ASSERT_OBJ(obj, MY_CLASS);
    lv_live2d_t *live2d = (lv_live2d_t *)obj;

    if (live2d->live != NULL) {
        LV_LOG_WARN("background must be set before the model");
        return;
    }
    lv_color32_t rgb;
    rgb.full = lv_color_to32(color);
    live2d->opaque = true;
    live2d->background = color;
    live2d->background_rgb[0] = rgb.ch.red;
    live2d->background_rgb[1] = rgb.ch.green;
    live2d->background_rgb[2] = rgb.ch.blue;
}

pe_live_result_t lv_live2d_set_model(lv_obj_t *obj, const void *model, size_t model_size,
                                     lv_coord_t width, lv_coord_t height, uint32_t flags)
{
    LV_ASSERT_OBJ(obj, MY_CLASS);
    lv_live2d_t *live2d = (lv_live2d_t *)obj;

    if (width <= 0 || height <= 0) {
        return PE_LIVE_ERR_ARGUMENT;
    }
    lv_live2d_release(live2d);

    pe_live_result_t res = pe_live_open(model, model_size, &live2d->allocator, flags, &live2d->live);
    if (res != PE_LIVE_OK) {
        LV_LOG_WARN("model rejected: %s", pe_live_result_string(res));
        return res;
    }

    const size_t work_size = (size_t)width * (size_t)height * 4u;
    const size_t frame_size =
        (size_t)width * (size_t)height * (live2d->opaque ? sizeof(lv_color_t) : LV_IMG_PX_SIZE_ALPHA_BYTE);
    live2d->target.pixels = live2d->allocator.alloc(live2d->allocator.ctx, work_size);
    live2d->frame = live2d->allocator.alloc(live2d->allocator.ctx, frame_size);
    if (live2d->target.pixels == NULL || live2d->frame == NULL) {
        LV_LOG_WARN("no memory for %dx%d frame buffers", (int)width, (int)height);
        lv_live2d_release(live2d);
        return PE_LIVE_ERR_NO_MEMORY;
    }
    memset(live2d->target.pixels, 0, work_size);
    memset(live2d->frame, 0, frame_size);

    /* scale the model canvas to fit and centre it */
    const float model_w = (float)pe_live_get_width(live2d->live);
    const float model_h = (float)pe_live_get_height(live2d->live);
    const float scale_x = (float)width / model_w;
    const float scale_y = (float)height / model_h;
    live2d->target.width = width;
    live2d->target.height = height;
    live2d->target.stride = width * 4;
    live2d->target.scale = scale_x < scale_y ? scale_x : scale_y;
    live2d->target.offset_x = ((float)width - model_w * live2d->target.scale) * 0.5f;
    live2d->target.offset_y = ((float)height - model_h * live2d->target.scale) * 0.5f;

    live2d->dsc.header.always_zero = 0;
    live2d->dsc.header.cf = live2d->opaque ? LV_IMG_CF_TRUE_COLOR : LV_IMG_CF_TRUE_COLOR_ALPHA;
    live2d->dsc.header.w = width;
    live2d->dsc.header.h = height;
    live2d->dsc.data_size = (uint32_t)frame_size;
    live2d->dsc.data = live2d->frame;

    /* show the rest pose right away */
    pe_live_update(live2d->live, 0);
    lv_live2d_draw_frame(live2d);
    lv_img_set_src(obj, &live2d->dsc);

    live2d->last_tick = lv_tick_get();
    live2d->timer = lv_timer_create(lv_live2d_timer_cb, live2d->frame_period, obj);
    if (live2d->timer == NULL) {
        lv_live2d_release(live2d);
        return PE_LIVE_ERR_NO_MEMORY;
    }
    return PE_LIVE_OK;
}

void lv_live2d_set_frame_period(lv_obj_t *obj, uint32_t period_ms)
{
    LV_ASSERT_OBJ(obj, MY_CLASS);
    lv_live2d_t *live2d = (lv_live2d_t *)obj;

    live2d->frame_period = period_ms == 0 ? 1 : period_ms;
    if (live2d->timer != NULL) {
        lv_timer_set_period(live2d->timer, live2d->frame_period);
    }
}

void lv_live2d_set_frame_cb(lv_obj_t *obj, lv_live2d_frame_cb_t cb, void *user_data)
{
    LV_ASSERT_OBJ(obj, MY_CLASS);
    lv_live2d_t *live2d = (lv_live2d_t *)obj;

    live2d->frame_cb = cb;
    live2d->frame_cb_data = user_data;
}

void lv_live2d_request_frame(lv_obj_t *obj)
{
    LV_ASSERT_OBJ(obj, MY_CLASS);
    lv_live2d_t *live2d = (lv_live2d_t *)obj;

    if (live2d->timer != NULL && !live2d->timer->paused) {
        lv_timer_ready(live2d->timer);
    }
}

uint32_t lv_live2d_get_frame_time(const lv_obj_t *obj)
{
    return ((const lv_live2d_t *)obj)->frame_time;
}

bool lv_live2d_play(lv_obj_t *obj, int32_t index, bool loop)
{
    LV_ASSERT_OBJ(obj, MY_CLASS);
    lv_live2d_t *live2d = (lv_live2d_t *)obj;

    if (live2d->live == NULL || !pe_live_play(live2d->live, index)) {
        return false;
    }
    pe_live_set_loop(live2d->live, loop);
    if (live2d->timer != NULL && live2d->timer->paused) {
        live2d->last_tick = lv_tick_get(); /* do not replay the time spent paused */
        lv_timer_resume(live2d->timer);
    }
    return true;
}

bool lv_live2d_play_by_name(lv_obj_t *obj, const char *name, bool loop)
{
    return lv_live2d_play(obj, lv_live2d_find_animation(obj, name), loop);
}

void lv_live2d_pause(lv_obj_t *obj)
{
    LV_ASSERT_OBJ(obj, MY_CLASS);
    lv_live2d_t *live2d = (lv_live2d_t *)obj;

    pe_live_pause(live2d->live);
    if (live2d->timer != NULL) {
        lv_timer_pause(live2d->timer);
    }
}

void lv_live2d_resume(lv_obj_t *obj)
{
    LV_ASSERT_OBJ(obj, MY_CLASS);
    lv_live2d_t *live2d = (lv_live2d_t *)obj;

    pe_live_resume(live2d->live);
    live2d->last_tick = lv_tick_get();
    if (live2d->timer != NULL) {
        lv_timer_resume(live2d->timer);
    }
}

void lv_live2d_stop(lv_obj_t *obj)
{
    LV_ASSERT_OBJ(obj, MY_CLASS);
    lv_live2d_t *live2d = (lv_live2d_t *)obj;

    pe_live_stop(live2d->live);
    if (live2d->timer != NULL && live2d->timer->paused) {
        live2d->last_tick = lv_tick_get();
        lv_timer_resume(live2d->timer);
    }
}

int32_t lv_live2d_get_animation_count(const lv_obj_t *obj)
{
    return pe_live_get_animation_count(((const lv_live2d_t *)obj)->live);
}

const char *lv_live2d_get_animation_name(const lv_obj_t *obj, int32_t index)
{
    return pe_live_get_animation_id(((const lv_live2d_t *)obj)->live, index);
}

int32_t lv_live2d_find_animation(const lv_obj_t *obj, const char *name)
{
    return pe_live_find_animation(((const lv_live2d_t *)obj)->live, name);
}

int32_t lv_live2d_get_current_animation(const lv_obj_t *obj)
{
    return pe_live_get_current_animation(((const lv_live2d_t *)obj)->live);
}

bool lv_live2d_is_playing(const lv_obj_t *obj)
{
    return pe_live_get_state(((const lv_live2d_t *)obj)->live) == PE_LIVE_STATE_PLAYING;
}

pe_live_t *lv_live2d_get_engine(lv_obj_t *obj)
{
    LV_ASSERT_OBJ(obj, MY_CLASS);
    return ((lv_live2d_t *)obj)->live;
}

/* ------------------------------------------------------------------------- */

static void lv_live2d_constructor(const lv_obj_class_t *class_p, lv_obj_t *obj)
{
    LV_UNUSED(class_p);
    lv_live2d_t *live2d = (lv_live2d_t *)obj;

    live2d->allocator.alloc = lv_live2d_default_alloc;
    live2d->allocator.free = lv_live2d_default_free;
    live2d->allocator.ctx = NULL;
    live2d->frame_period = LV_LIVE2D_DEFAULT_FRAME_PERIOD_MS;
    live2d->frame_time = 0;
    live2d->frame_cb = NULL;
    live2d->frame_cb_data = NULL;
    live2d->opaque = false;
    live2d->background = lv_color_black();
    memset(live2d->background_rgb, 0, sizeof(live2d->background_rgb));
    live2d->live = NULL;
    live2d->timer = NULL;
    live2d->frame = NULL;
    memset(&live2d->target, 0, sizeof(live2d->target));
    live2d->inv_alpha[0] = 0;
    for (uint32_t a = 1; a < 256; a++) {
        live2d->inv_alpha[a] = (255u * 65536u + a / 2u) / a;
    }
}

static void lv_live2d_destructor(const lv_obj_class_t *class_p, lv_obj_t *obj)
{
    LV_UNUSED(class_p);
    lv_live2d_release((lv_live2d_t *)obj);
}

static void lv_live2d_release(lv_live2d_t *live2d)
{
    if (live2d->timer != NULL) {
        lv_timer_del(live2d->timer);
        live2d->timer = NULL;
    }
    if (live2d->frame != NULL) {
        /* the decoder cache may still point at the old pixel buffer */
        lv_img_cache_invalidate_src(&live2d->dsc);
        live2d->allocator.free(live2d->allocator.ctx, live2d->frame);
        live2d->frame = NULL;
    }
    if (live2d->target.pixels != NULL) {
        live2d->allocator.free(live2d->allocator.ctx, live2d->target.pixels);
        live2d->target.pixels = NULL;
    }
    if (live2d->live != NULL) {
        pe_live_close(live2d->live);
        live2d->live = NULL;
    }
    live2d->dsc.data = NULL;
    live2d->dsc.data_size = 0;
}

/* Converts the premultiplied work buffer into LV_IMG_CF_TRUE_COLOR_ALPHA pixels.
 * The colour bytes are copied one by one: a memcpy() per pixel is a library
 * call where the compiler may not inline it. */
static void lv_live2d_convert_frame(lv_live2d_t *live2d)
{
    const uint8_t *src = live2d->target.pixels;
    uint8_t *dst = live2d->frame;
    const size_t count = (size_t)live2d->target.width * (size_t)live2d->target.height;

    for (size_t i = 0; i < count; i++, src += 4, dst += LV_IMG_PX_SIZE_ALPHA_BYTE) {
        uint32_t a = src[3];
        uint32_t r = src[0], g = src[1], b = src[2];
        if (a == 0) {
            r = g = b = 0;
        } else if (a != 255) {
            /* un-premultiply the (few) edge pixels; 1/alpha in 16.16 */
            const uint32_t inv = live2d->inv_alpha[a];
            r = (r * inv + 32768u) >> 16;
            g = (g * inv + 32768u) >> 16;
            b = (b * inv + 32768u) >> 16;
            r = r > 255 ? 255 : r;
            g = g > 255 ? 255 : g;
            b = b > 255 ? 255 : b;
        }
        union {
            lv_color_t color;
            uint8_t bytes[sizeof(lv_color_t)];
        } px;
        px.color = lv_color_make((uint8_t)r, (uint8_t)g, (uint8_t)b);
#if LV_COLOR_DEPTH == 32
        px.color.ch.alpha = (uint8_t)a;
        dst[0] = px.bytes[0];
        dst[1] = px.bytes[1];
        dst[2] = px.bytes[2];
        dst[3] = px.bytes[3];
#elif LV_COLOR_DEPTH == 16
        dst[0] = px.bytes[0];
        dst[1] = px.bytes[1];
        dst[2] = (uint8_t)a;
#else
        dst[0] = px.bytes[0];
        dst[1] = (uint8_t)a;
#endif
    }
}

/* Converts the premultiplied work buffer into LV_IMG_CF_TRUE_COLOR pixels over
 * the background colour: colour + background * (1 - alpha). */
static void lv_live2d_convert_opaque(lv_live2d_t *live2d)
{
    const uint8_t *src = live2d->target.pixels;
    lv_color_t *dst = (lv_color_t *)live2d->frame;
    const size_t count = (size_t)live2d->target.width * (size_t)live2d->target.height;
    const lv_color_t bg = live2d->background;
    const uint32_t bg_r = live2d->background_rgb[0];
    const uint32_t bg_g = live2d->background_rgb[1];
    const uint32_t bg_b = live2d->background_rgb[2];

    for (size_t i = 0; i < count; i++, src += 4) {
        const uint32_t a = src[3];
        if (a == 0) {
            dst[i] = bg;
            continue;
        }
        uint32_t r = src[0], g = src[1], b = src[2];
        if (a != 255) {
            /* LV_UDIV255() is exact below 65536 */
            const uint32_t k = 255u - a;
            r += LV_UDIV255(bg_r * k);
            g += LV_UDIV255(bg_g * k);
            b += LV_UDIV255(bg_b * k);
            r = r > 255 ? 255 : r;
            g = g > 255 ? 255 : g;
            b = b > 255 ? 255 : b;
        }
        dst[i] = lv_color_make((uint8_t)r, (uint8_t)g, (uint8_t)b);
    }
}

static void lv_live2d_draw_frame(lv_live2d_t *live2d)
{
    memset(live2d->target.pixels, 0, (size_t)live2d->target.stride * (size_t)live2d->target.height);
    pe_live_render(live2d->live, &live2d->target);
    if (live2d->opaque) {
        lv_live2d_convert_opaque(live2d);
    } else {
        lv_live2d_convert_frame(live2d);
    }
}

static void lv_live2d_timer_cb(lv_timer_t *timer)
{
    lv_obj_t *obj = timer->user_data;
    lv_live2d_t *live2d = (lv_live2d_t *)obj;

    if (live2d->live == NULL) {
        return;
    }
    uint32_t now = lv_tick_get();
    uint32_t elapsed = now - live2d->last_tick;
    live2d->last_tick = now;

    if (live2d->frame_cb != NULL) {
        live2d->frame_cb(obj, elapsed, live2d->frame_cb_data);
    }
    pe_live_update_result_t result = pe_live_update(live2d->live, elapsed);
    lv_live2d_draw_frame(live2d);
    live2d->frame_time = lv_tick_elaps(now);
    lv_obj_invalidate(obj);

    /* last: the handler may start another animation or delete the widget */
    if (result != PE_LIVE_UPDATE_NONE) {
        lv_event_send(obj, LV_EVENT_READY, NULL);
    }
}
