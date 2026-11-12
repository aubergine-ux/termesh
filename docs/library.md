# libtermesh: the renderer as a library

Everything termesh draws is available to other programs through a small C
API, [`include/termesh.h`](../include/termesh.h). Load or build a scene, then
render it to RGBA pixels or to the text a terminal would display.

```c
#include <stdio.h>
#include <termesh.h>

int main(void) {
    termesh_scene* scene = termesh_scene_open("model.glb");
    if (!scene) {
        fprintf(stderr, "%s\n", termesh_last_error());
        return 1;
    }
    termesh_renderer* renderer = termesh_renderer_create(0); /* 0: use every core */

    termesh_camera camera;
    termesh_camera_frame(scene, 30.0f, 15.0f, 1.0f, 40.0f, &camera);

    size_t length;
    char* text = termesh_render_text(renderer, scene, &camera, NULL, TERMESH_BACKEND_HALFBLOCK,
                                     TERMESH_COLOR_TRUECOLOR, 80, 24, &length);
    fwrite(text, 1, length, stdout);
    termesh_free(text);

    termesh_renderer_destroy(renderer);
    termesh_scene_destroy(scene);
    return 0;
}
```

A longer example is in [`examples/c/render.c`](../examples/c/render.c).

## Building and installing

The library is built along with the program:

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
cmake --install build --prefix ~/.local      # or sudo cmake --install build
```

That installs `libtermesh.so` (`.dylib` on macOS, `libtermesh.dll` on
Windows), `termesh.h`, and what build systems need to find them. Pass
`-DTERMESH_LIBRARY_STATIC=ON` for a static library instead.

| Build system | How                                                                                   |
| ------------ | ------------------------------------------------------------------------------------- |
| pkg-config   | `cc app.c $(pkg-config --cflags --libs termesh)`                                      |
| CMake        | `find_package(termesh REQUIRED)` then `target_link_libraries(app PRIVATE termesh::termesh)` |
| By hand      | `cc app.c -I<prefix>/include -L<prefix>/lib -ltermesh`                                |

The static library is C++ inside, so a C program linking it also needs the
C++ runtime: `pkg-config --static --libs termesh` adds it.

## The API in brief

| Area      | Functions                                                                                              |
| --------- | ------------------------------------------------------------------------------------------------------ |
| Scenes    | `termesh_scene_create`, `_open`, `_destroy`, `_add_file`, `_add_demo`, `_add_mesh`                       |
| Objects   | `termesh_scene_object_count`, `_object_name`, `_set_transform`, `_set_visible`, `_bounds`                |
| Animation | `termesh_scene_animation_count`, `_animation_name`, `_animation_duration`, `_play`, `_seek`, `_set_speed`, `_advance` |
| Lighting  | `termesh_light_init`, `termesh_scene_add_light`, `_clear_lights`, `_set_environment`                     |
| Camera    | `termesh_camera_frame`, `termesh_scene_camera`                                                           |
| Settings  | `termesh_settings_init`, `termesh_settings_from_scene`                                                   |
| Rendering | `termesh_renderer_create`, `_destroy`, `termesh_render_pixels`, `termesh_render_text`                    |
| Picking   | `termesh_pick`                                                                                           |
| Misc      | `termesh_version`, `termesh_api_version`, `termesh_last_error`, `termesh_free`                           |

The header documents each one. The conventions:

- **Errors.** A function that can fail returns `NULL` or a negative number,
  and `termesh_last_error()` then says why. The message is kept per thread.
- **Structs.** `termesh_mesh`, `termesh_light` and `termesh_settings` must be
  initialised with their `_init` function before you set fields. That is
  what lets later versions add fields without breaking your program.
- **Memory.** Text from `termesh_render_text` is yours: release it with
  `termesh_free`. Other strings belong to the library.
- **Units.** Right-handed, +y up; column-major matrices; angles in degrees;
  linear RGB colours (the background, being a screen colour, is sRGB bytes).
- **Threads.** One scene or renderer may be used by one thread at a time;
  separate ones are independent. A renderer brings its own worker threads.

## Geometry from memory

```c
static const float positions[] = {-1, -1, 0,   1, -1, 0,   0, 1, 0};
static const uint32_t indices[] = {0, 1, 2};

termesh_mesh mesh;
termesh_mesh_init(&mesh);
mesh.positions = positions;
mesh.vertex_count = 3;
mesh.indices = indices;
mesh.index_count = 3;
mesh.base_color[0] = 1.0f, mesh.base_color[1] = 0.3f, mesh.base_color[2] = 0.2f;
int object = termesh_scene_add_mesh(scene, &mesh);
```

Leave `indices` `NULL` and the vertices are drawn as a point cloud.

## Pixels

```c
uint8_t* rgba = malloc(640 * 480 * 4);
termesh_settings settings;
termesh_settings_init(&settings);
settings.shading = TERMESH_SHADING_PBR;
settings.shadows = 1;
termesh_render_pixels(renderer, scene, &camera, &settings, 640, 480, rgba);
```

The alpha channel is coverage - 0 on the background - so the image can be
composited over something else.

## Compatibility

`TERMESH_API_VERSION` (and `termesh_api_version()` at run time) names the
interface. Within a version, functions are only ever added; the shared
library's soname carries the same number.
