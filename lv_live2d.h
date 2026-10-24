/*
 * lv_live2d.h - LVGL widget that plays PainterEngine "LiveD" models
 *
 * SPDX-License-Identifier: MIT
 *
 * The widget extends lv_img: every frame the model is rendered by the pe_live
 * engine into a private RGB565+A8 (LV_IMG_CF_TRUE_COLOR_ALPHA) buffer that the
 * image widget then composites onto the screen, so the transparent background
 * of the character is preserved and all lv_img styling/positioning works.
 * With lv_live2d_set_background() the character is drawn onto an opaque
 * colour instead (LV_IMG_CF_TRUE_COLOR), which LVGL copies rather than blends
 * pixel by pixel.
 *
 * Typical use:
 *     lv_obj_t *avatar = lv_live2d_create(lv_scr_act());
 *     lv_live2d_set_allocator(avatar, &psram_allocator);    // optional
 *     lv_live2d_set_model(avatar, model, model_size, 240, 240, PE_LIVE_FLAG_CACHE_TEXTURES);
 *     lv_live2d_play_by_name(avatar, "action01", true);
 *
 * LV_EVENT_READY is sent whenever an animation reaches its last frame (once per
 * loop iteration when looping).
 */
#ifndef LV_LIVE2D_H
#define LV_LIVE2D_H

#include "lvgl.h"
#include "pe_live.h"

#ifdef __cplusplus
extern "C" {
#endif

#define LV_LIVE2D_DEFAULT_FRAME_PERIOD_MS 33

/* Called on every frame before the model advances by elapsed_ms: the place to
 * steer it frame by frame (e.g. ease a pose towards a target). It may change
 * overrides and the frame period but must not delete the widget. */
typedef void (*lv_live2d_frame_cb_t)(lv_obj_t *obj, uint32_t elapsed_ms, void *user_data);

typedef struct {
    lv_img_t img;
    lv_img_dsc_t dsc;               /* image source handed to lv_img */
    lv_timer_t *timer;
    pe_live_t *live;
    pe_live_allocator_t allocator;  /* used for the engine and the frame buffers */
    pe_live_target_t target;        /* premultiplied RGBA8888 work buffer */
    uint8_t *frame;                 /* LVGL pixels referenced by dsc */
    uint32_t last_tick;
    uint32_t frame_period;
    uint32_t frame_time;            /* ms the last frame took to compute */
    lv_live2d_frame_cb_t frame_cb;
    void *frame_cb_data;
    bool opaque;                    /* frame is LV_IMG_CF_TRUE_COLOR over background */
    lv_color_t background;
    uint8_t background_rgb[3];      /* the background in 8-bit R, G, B */
    uint32_t inv_alpha[256];        /* 255 / alpha in 16.16, for un-premultiplying */
} lv_live2d_t;

extern const lv_obj_class_t lv_live2d_class;

lv_obj_t *lv_live2d_create(lv_obj_t *parent);

/* Must be called before lv_live2d_set_model(). NULL restores lv_mem_alloc/free.
 * A 240x240 widget needs about 400 KB for its two frame buffers (345 KB with
 * an opaque background at 16-bit colour) plus the model state (and the
 * texture cache when PE_LIVE_FLAG_CACHE_TEXTURES is used). */
void lv_live2d_set_allocator(lv_obj_t *obj, const pe_live_allocator_t *allocator);

/* Must be called before lv_live2d_set_model(). Draws the character onto this
 * opaque colour instead of keeping its transparency: the frame buffer shrinks
 * to one lv_color_t per pixel and LVGL copies it instead of blending every
 * pixel, which pays off whenever the widget sits on a plain background. */
void lv_live2d_set_background(lv_obj_t *obj, lv_color_t color);

/* Loads a model that stays valid for the lifetime of the widget and renders it
 * scaled to fit width x height, centred. flags are pe_live_open() flags. */
pe_live_result_t lv_live2d_set_model(lv_obj_t *obj, const void *model, size_t model_size,
                                     lv_coord_t width, lv_coord_t height, uint32_t flags);
void lv_live2d_set_frame_period(lv_obj_t *obj, uint32_t period_ms);
/* NULL removes the callback. */
void lv_live2d_set_frame_cb(lv_obj_t *obj, lv_live2d_frame_cb_t cb, void *user_data);
/* Draws the next frame at the next LVGL timer run instead of after the rest
 * of the period, e.g. right after an override changed. */
void lv_live2d_request_frame(lv_obj_t *obj);
/* Milliseconds the last frame took: engine update, rasterisation and
 * conversion (LVGL's own drawing and flushing come on top). */
uint32_t lv_live2d_get_frame_time(const lv_obj_t *obj);

bool lv_live2d_play(lv_obj_t *obj, int32_t index, bool loop);
bool lv_live2d_play_by_name(lv_obj_t *obj, const char *name, bool loop);
void lv_live2d_pause(lv_obj_t *obj);
void lv_live2d_resume(lv_obj_t *obj);
void lv_live2d_stop(lv_obj_t *obj);

int32_t lv_live2d_get_animation_count(const lv_obj_t *obj);
const char *lv_live2d_get_animation_name(const lv_obj_t *obj, int32_t index);
int32_t lv_live2d_find_animation(const lv_obj_t *obj, const char *name);
int32_t lv_live2d_get_current_animation(const lv_obj_t *obj);
bool lv_live2d_is_playing(const lv_obj_t *obj);
pe_live_t *lv_live2d_get_engine(lv_obj_t *obj);

#ifdef __cplusplus
}
#endif

#endif /* LV_LIVE2D_H */
