# LVGL PainterEngine LiveD 播放器

[English](README.md)

`lv_painter_live2d` 是一个用于播放 PainterEngine **LiveD** 二进制模型的小型
C 库。它分成两层：

- `pe_live` 负责解析 LiveD v1 格式、推进关键帧动画、叠加运行时姿态控制，
  并把贴图三角形渲染成预乘 RGBA8888 像素；这一层不依赖 LVGL。
- `lv_live2d` 是基于 `lv_img` 的 LVGL 8.3 控件，它通过 LVGL 定时器驱动播放
  内核，并保留透明背景完成合成。

本项目不是 Live2D Cubism 播放器，也不隶属于 Live2D Inc.。仓库不包含模型
文件、编辑器或导出器；请使用 PainterEngine 工具链制作 LiveD 资源。

## 功能

- 播放 PainterEngine LiveD v1 模型，可按索引或不区分大小写的动画 ID 播放。
- 在关键帧动画之上叠加图层旋转、平移、替换贴图和动作追踪（转头）控制。
- 支持弹性顶点、骨骼拉伸、动作追踪形变和贴图三角形光栅化。
- 可以注入自定义内存分配器，用于引擎状态、纹理缓存和 LVGL 帧缓冲。
- 模型可放在 XIP Flash 中；默认原地读取贴图和动画帧，只有设置
  `PE_LIVE_FLAG_CACHE_TEXTURES` 时才复制贴图。
- LVGL 8.3 控件支持暂停、恢复、停止、循环播放，并在动画到达末帧时发送
  `LV_EVENT_READY`；提供每帧回调（逐帧控制模型）、按需立即补一帧，以及上一帧
  的计算耗时。
- 默认输出带透明度；也可以用 `lv_live2d_set_background()` 指定不透明的底色，
  这时 LVGL 直接拷贝画面、不再逐像素混合，帧缓冲也更小。

## 依赖要求

### `pe_live`

- C99 编译器和 C 标准库。
- C 数学库（`sinf`、`cosf`、`sqrtf` 等）。当工具链把 `m` 作为独立库提供
  时，CMake 会自动链接。
- 有效的 `pe_live_allocator_t` 分配器实现。
- 小端 LiveD v1 模型缓冲，起始地址必须按 4 字节对齐；引擎打开期间缓冲必须
  保持有效。

### `lv_live2d`

- `pe_live`。
- LVGL 8.3.x（已按 8.3.11 检查）；LVGL 9 不在当前支持范围内。
- LVGL 配置需要启用 `lv_img`、定时器、内存分配和真彩 Alpha 图片；控件支持
  16 位和 32 位色深。
- 请在 LVGL 任务或同一个固定线程里调用。

## 仓库结构

```text
.
├── pe_live.h              # 播放内核公开接口
├── pe_live.c              # 播放内核实现
├── lv_live2d.h            # LVGL 控件公开接口
├── lv_live2d.c            # LVGL 控件实现
├── CMakeLists.txt         # 可选 CMake 集成
├── LICENSE
└── THIRD_PARTY_NOTICES.md
```

## 使用 CMake 构建

播放内核始终会创建。默认情况下，父工程里已经有 LVGL 目标时会同时构建控件：

```cmake
add_subdirectory(lv_painter_live2d)
target_link_libraries(your_app PRIVATE lv_painter_live2d::lv_painter_live2d)
```

如果 LVGL 目标不叫 `lvgl` 或 `lvgl::lvgl`，请明确指定：

```cmake
set(LV_PAINTER_LIVE2D_LVGL_TARGET my_lvgl_target)
add_subdirectory(lv_painter_live2d)
```

只构建不依赖 LVGL 的播放内核：

```sh
cmake -S . -B build -DLV_PAINTER_LIVE2D_BUILD_WIDGET=OFF
cmake --build build
```

也可以不用 CMake：把 `pe_live.c` 加进工程、把本目录加入头文件路径并链接数学
库；需要使用 LVGL 时再编译 `lv_live2d.c`。

## 快速开始

创建 LVGL 控件，可选地设置分配器，然后加载在控件生命周期内保持有效的模型并
播放动画：

```c
lv_obj_t *avatar = lv_live2d_create(lv_scr_act());
lv_live2d_set_allocator(avatar, &my_allocator);       /* 可选 */
lv_live2d_set_model(avatar, model, model_size, 240, 240,
                    PE_LIVE_FLAG_CACHE_TEXTURES);
lv_live2d_play_by_name(avatar, "action01", true);
```

`lv_live2d_pause`、`lv_live2d_resume` 和 `lv_live2d_stop` 也应在同一个 LVGL
任务里调用。动画到达末帧时会发送 `LV_EVENT_READY`；循环播放时每循环一次发送
一次。

通过 `lv_live2d_get_engine()` 可以使用底层接口做面部追踪、表情控制或其他运行
时控制：

```c
pe_live_t *live = lv_live2d_get_engine(avatar);
int32_t head = pe_live_find_layer(live, "head");
pe_live_set_layer_rotation_offset(live, head, 8.0f);   /* 歪头 */

/* 转头和点头：拖动脸部矩形的中心点 */
static const pe_live_tracer_region_t face = { 110.0f, 36.7f, 98.3f, 115.0f, 159.2f, 94.2f };
pe_live_set_layer_tracer(live, head, &face, 10.0f, 4.0f);
```

动作追踪（Action Tracer）是 PainterEngine 里二维脸转头的做法：矩形边界不动，
只拖动中心点，于是眼睛、鼻子、嘴往转的方向移动，转过去的那一侧被压缩。矩形和
中心请沿用模型作者在 PainterEngine 编辑器里给头部设定的值；拖动量宜小，它只适合
小幅转头。

追踪数据通常比画面帧来得慢、也不均匀。与其每来一个新值就跳过去，不如把它当作目
标，在每帧回调里逐步逼近。回调在每帧渲染之前调用，参数是距上一帧经过的毫秒数：

```c
static float target_dx, shown_dx;

static void on_frame(lv_obj_t *obj, uint32_t elapsed_ms, void *user_data)
{
    shown_dx += (target_dx - shown_dx) * (1.0f - expf(-(float)elapsed_ms / 70.0f));
    pe_live_set_layer_tracer(lv_live2d_get_engine(obj), head, &face, shown_dx, 0.0f);
}

lv_live2d_set_frame_cb(avatar, on_frame, NULL);
```

`lv_live2d_set_frame_period()` 随时可以改，在回调里改也可以（例如只在有东西在动
时用高帧率）。`lv_live2d_request_frame()` 让下一帧在下一次 LVGL 定时器运行时就
画，适合眨眼换贴图这种需要立刻显示的变化。`lv_live2d_get_frame_time()` 返回上一
帧的计算耗时（毫秒，含引擎更新、光栅化和格式转换，不含 LVGL 自己的绘制和刷屏）。

## 不使用 LVGL 直接调用 `pe_live`

```c
pe_live_t *live = NULL;
pe_live_result_t res = pe_live_open(model, model_size, &allocator, 0, &live);
if (res != PE_LIVE_OK) {
    /* pe_live_result_string(res) */
}

pe_live_play(live, pe_live_find_animation(live, "idle"));
pe_live_set_loop(live, true);

/* 每次计时循环：推进姿态并渲染。 */
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

`pe_live_render` 输出预乘 RGBA8888 像素，不会清空目标缓冲。请按上面的方式先
清空，或自行把它合成到目标画面上。

## 模型和内存说明

- `pe_live_open` 会拒绝空指针、未按 4 字节对齐的模型指针，并要求调用方提供
  分配器。
- 引擎会保留指向模型缓冲内部的指针；在 `pe_live_close` 返回前不要移动、释放
  或修改模型缓冲。
- `PE_LIVE_FLAG_CACHE_TEXTURES` 会通过分配器复制贴图。它会占用更多 RAM，但
  通常比直接从 XIP Flash 采样更快。
- 可变状态、绘制顺序、更新顺序和动画元数据都通过分配器申请；
  `pe_live_get_memory_usage` 返回申请总量。
- LVGL 控件会分配一块预乘 RGBA 工作缓冲和一块真彩 Alpha LVGL 帧缓冲。
  240x240 控件约需 400 KB，再加上引擎状态；如果开启纹理缓存，还要加上贴图
  副本。在加载模型之前调用 `lv_live2d_set_background()` 后，帧缓冲只存不带透明度
  的 `lv_color_t`，16 位色下两块缓冲合计约 345 KB。

## 线程和时序

引擎不是线程安全的。`pe_live_update`、`pe_live_render`、运行时控制接口和
LVGL 控件接口都应放在同一个线程里，通常是 LVGL 任务。`pe_live_update` 接收以
毫秒为单位的时间增量，单次更新最多按 1000 ms 处理。弹性模拟按固定时间步长推
进，所以不管帧率多少，头发和衣服的动态都一样。控件的每帧回调在它自己的 LVGL
定时器里运行，回调里不能删除控件。

## 许可证和致谢

本项目以 [MIT License](LICENSE) 发布。代码包含从 PainterEngine 衍生的部分，
并把 LVGL 作为外部依赖使用；对应的版权和许可证全文见
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)。
