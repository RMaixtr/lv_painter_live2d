# LVGL PainterEngine LiveD Player

[简体中文](README_zh-CN.md)

`lv_painter_live2d` is a small C library for playing PainterEngine **LiveD**
binary models. It has two layers:

- `pe_live` parses LiveD format v1, advances key-frame animations, applies
  runtime pose overrides and renders textured triangles to premultiplied
  RGBA8888 pixels. It does not depend on LVGL.
- `lv_live2d` is an `lv_img`-based LVGL 8.3 widget that drives the engine from
  an LVGL timer and composites the result with transparency.

This is not a Live2D Cubism player and is not affiliated with Live2D Inc. The
repository does not include model files, an editor or an exporter; produce
LiveD assets with the PainterEngine tools.

## Features

- PainterEngine LiveD format v1 playback, with animations addressed by index
  or case-insensitive ID.
- Runtime layer rotation, translation, texture replacement and Action Tracer
  (head turn) overrides on top of key-frame animation.
- Elastic vertex simulation, bone stretch, Action Tracer deformation and
  texture-mapped triangle rasterization.
- Allocator injection for engines, texture caches and LVGL frame buffers.
- XIP-friendly model access: texture pixels and animation frames are read in
  place unless `PE_LIVE_FLAG_CACHE_TEXTURES` requests a texture copy.
- LVGL 8.3 widget with pause/resume/stop, loop control and `LV_EVENT_READY`,
  a per-frame callback for steering the model frame by frame, on-demand
  frames and the compute time of the last frame.
- Transparent output by default, or an opaque background colour
  (`lv_live2d_set_background()`), which LVGL copies instead of blending every
  pixel and which needs a smaller frame buffer.

## Requirements

### `pe_live`

- C99 compiler and standard library.
- C math library (`sinf`, `cosf`, `sqrtf`, ...). CMake links `m` when the
  toolchain provides it as a separate library.
- A valid `pe_live_allocator_t` implementation.
- A little-endian LiveD v1 model buffer aligned to 4 bytes. The buffer must
  remain valid while the engine is open.

### `lv_live2d`

- `pe_live`.
- LVGL 8.3.x (tested against 8.3.11). LVGL 9 is outside the supported scope.
- An LVGL configuration that provides `lv_img`, timers, memory allocation and
  true-color-alpha images. The widget supports 16-bit and 32-bit color depths.
- Calls from the LVGL task or another single owner thread.

## Repository layout

```text
.
├── pe_live.h              # Engine public API
├── pe_live.c              # Engine implementation
├── lv_live2d.h            # LVGL widget public API
├── lv_live2d.c            # LVGL widget implementation
├── CMakeLists.txt         # Optional CMake integration
├── LICENSE
└── THIRD_PARTY_NOTICES.md
```

## Build with CMake

The core library is always available. The widget is built by default when an
LVGL target is available in the parent project:

```cmake
add_subdirectory(lv_painter_live2d)
target_link_libraries(your_app PRIVATE lv_painter_live2d::lv_painter_live2d)
```

If the LVGL target is not named `lvgl` or `lvgl::lvgl`, pass it explicitly:

```cmake
set(LV_PAINTER_LIVE2D_LVGL_TARGET my_lvgl_target)
add_subdirectory(lv_painter_live2d)
```

To build only the LVGL-free engine:

```sh
cmake -S . -B build -DLV_PAINTER_LIVE2D_BUILD_WIDGET=OFF
cmake --build build
```

The library can also be added manually: compile `pe_live.c`, add the directory
to the include path and link the math library; compile `lv_live2d.c` as well
when using LVGL.

## Quick start

Create an LVGL widget, optionally choose an allocator, load a model that stays
valid for the widget lifetime and start an animation:

```c
lv_obj_t *avatar = lv_live2d_create(lv_scr_act());
lv_live2d_set_allocator(avatar, &my_allocator);       /* optional */
lv_live2d_set_model(avatar, model, model_size, 240, 240,
                    PE_LIVE_FLAG_CACHE_TEXTURES);
lv_live2d_play_by_name(avatar, "action01", true);
```

Call `lv_live2d_pause`, `lv_live2d_resume` and `lv_live2d_stop` from the same
LVGL task. `LV_EVENT_READY` is sent when an animation reaches its last frame,
once for every loop iteration when looping.

Use `lv_live2d_get_engine()` to reach the lower-level API for face tracking,
expression control or other runtime steering:

```c
pe_live_t *live = lv_live2d_get_engine(avatar);
int32_t head = pe_live_find_layer(live, "head");
pe_live_set_layer_rotation_offset(live, head, 8.0f);   /* tilt */

/* turn and nod: drag the centre of a rectangle round the face */
static const pe_live_tracer_region_t face = { 110.0f, 36.7f, 98.3f, 115.0f, 159.2f, 94.2f };
pe_live_set_layer_tracer(live, head, &face, 10.0f, 4.0f);
```

The Action Tracer is how a PainterEngine 2D face turns: inside the rectangle
the centre moves while the edges stay, so the eyes, nose and mouth shift
towards the turn and that side of the face compresses. Use the rectangle and
centre the model's author used for the head in the PainterEngine editor; keep
the drag small, it is meant for slight turns.

Tracking data usually arrives slower and less evenly than the frames are
drawn. Instead of jumping to every new value, set it as a target and ease
towards it in the frame callback, which runs before every frame with the time
since the previous one:

```c
static float target_dx, shown_dx;

static void on_frame(lv_obj_t *obj, uint32_t elapsed_ms, void *user_data)
{
    shown_dx += (target_dx - shown_dx) * (1.0f - expf(-(float)elapsed_ms / 70.0f));
    pe_live_set_layer_tracer(lv_live2d_get_engine(obj), head, &face, shown_dx, 0.0f);
}

lv_live2d_set_frame_cb(avatar, on_frame, NULL);
```

`lv_live2d_set_frame_period()` may be changed at any time, also from the
callback (e.g. a high rate only while something moves).
`lv_live2d_request_frame()` draws the next frame at the next LVGL timer run,
which suits immediate changes such as a blink texture.
`lv_live2d_get_frame_time()` reports how many milliseconds the last frame took
to compute (engine update, rasterisation and conversion; LVGL's drawing and
flushing come on top).

## Using `pe_live` without LVGL

```c
pe_live_t *live = NULL;
pe_live_result_t res = pe_live_open(model, model_size, &allocator, 0, &live);
if (res != PE_LIVE_OK) {
    /* pe_live_result_string(res) */
}

pe_live_play(live, pe_live_find_animation(live, "idle"));
pe_live_set_loop(live, true);

/* On every tick: advance the pose and render it. */
pe_live_update(live, elapsed_ms);

pe_live_target_t target = {
    .pixels = rgba_buffer,
    .width = 240,
    .height = 240,
    .stride = 240 * 4,
    .scale = 1.0f,
    .offset_x = 0.0f,
    .offset_y = 0.0f,
};
memset(rgba_buffer, 0, (size_t)target.stride * target.height);
pe_live_render(live, &target);
```

`pe_live_render` writes premultiplied RGBA8888 pixels and does not clear the
target. Composite it over your destination or clear the buffer before each
frame as shown above.

## Model and memory notes

- `pe_live_open` rejects a null or misaligned model pointer and requires a
  caller-supplied allocator.
- The engine keeps pointers into the model buffer. Do not move, free or modify
  it until `pe_live_close` returns.
- `PE_LIVE_FLAG_CACHE_TEXTURES` copies texture pixels through the allocator.
  That uses more RAM but can be faster than reading textures from XIP flash.
- Mutable state, draw/update order and animation metadata are allocated through
  the allocator. `pe_live_get_memory_usage` reports the allocation total.
- The LVGL widget allocates one premultiplied RGBA work buffer and one
  true-color-alpha LVGL frame buffer. A 240x240 widget needs roughly 400 KB
  for those buffers plus engine state, before the optional texture cache.
  With `lv_live2d_set_background()` (called before the model is loaded) the
  frame buffer holds plain `lv_color_t` pixels instead: about 345 KB in total
  at 16-bit colour.

## Threading and timing

The engine is not thread-safe. Keep `pe_live_update`, `pe_live_render`,
runtime override calls and the LVGL widget API on one thread, normally the
LVGL task. `pe_live_update` expects an elapsed time in milliseconds and limits
one update to 1000 ms. The elastic simulation advances in fixed time steps, so
the hair and cloth move the same at any frame rate. The widget's frame callback
runs inside its LVGL timer; it must not delete the widget.

## License and attribution

This project is released under the [MIT License](LICENSE). It contains code
derived from PainterEngine and uses the LVGL API as an external dependency;
see [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) for the corresponding
copyright and license texts.
