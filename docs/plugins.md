# Plugins

A plugin is a shared library that adds something to termesh at run time:

- a **loader**, so termesh can open another file format, or
- a **backend**, another way of drawing the picture with text.

```sh
termesh --plugin ./xyz.so scan.xyz                          # a format termesh does not read itself
termesh --plugin ./quadrant.so --backend quadrant model.glb # a backend from a plugin
termesh --list-plugins                                      # what is loaded, and from where
```

## Installing a plugin

Name it with `--plugin <file>` (as often as needed), or put it in a plugin
directory, from which everything is loaded on every start:

| Platform       | Directory                                                         |
| -------------- | ----------------------------------------------------------------- |
| Linux, macOS   | `$XDG_CONFIG_HOME/termesh/plugins`, or `~/.config/termesh/plugins` |
| Windows        | `%APPDATA%\termesh\plugins`                                        |
| anywhere       | each directory listed in `TERMESH_PLUGIN_PATH`                     |

`--no-plugins` skips the directories. A plugin is code that runs as you, with
no sandbox: install only plugins you trust, as you would any program.

The statically linked Linux binary from the releases page cannot load
plugins (a static program cannot load shared libraries); build from source,
or use the Python package's `termesh` command.

## The examples

Both are in [`examples/plugins`](../examples/plugins) and are built with the
project, into `build/plugins/`.

| Plugin        | Adds                                                                                 |
| ------------- | ------------------------------------------------------------------------------------ |
| `xyz.c`       | a loader for `.xyz` point clouds: `x y z` or `x y z r g b` per line                   |
| `quadrant.c`  | the `quadrant` backend: 2 × 2 pixels per cell using Unicode quadrant blocks (`▚ ▛ ▟`) |

```sh
cmake -B build && cmake --build build
build/termesh --plugin build/plugins/quadrant.so --backend quadrant --demo knot
```

## Writing one

A plugin is one C (or C++) file that includes
[`termesh_plugin.h`](../include/termesh_plugin.h). It links against nothing:
everything it needs from termesh arrives as a table of functions, the
*host*, passed to its one exported function.

```sh
cc -shared -fPIC -I/path/to/termesh/include myplugin.c -o myplugin.so     # Linux
cc -dynamiclib   -I/path/to/termesh/include myplugin.c -o myplugin.dylib  # macOS
cl /LD /I\path\to\termesh\include myplugin.c                              # Windows
```

### A loader

```c
#include <termesh_plugin.h>

static int load(void* user, const char* path, termesh_plugin_model* model, const termesh_host* host) {
    /* ...read the file into arrays... */
    termesh_plugin_mesh mesh;
    host->mesh_init(&mesh);            /* always initialise with the host */
    mesh.positions = positions;        /* x y z per vertex */
    mesh.vertex_count = count;
    mesh.indices = indices;            /* three per triangle; NULL for a point cloud */
    mesh.index_count = index_count;
    if (failed) {
        host->set_error(model, "what went wrong");
        return 1;
    }
    return host->add_mesh(model, &mesh);   /* copies the arrays; call once per mesh */
}

TERMESH_PLUGIN_EXPORT int termesh_plugin_init(const termesh_host* host) {
    return host->register_loader(host, "xyz,pts", load, NULL);
}
```

Optional per-vertex `normals`, `uvs` and `colors`, and the material's
`base_color`, `metallic`, `roughness` and `double_sided`, are fields of the
mesh. Normals are computed if you leave them out. A loader registered for an
extension termesh already reads replaces the built-in one.

### A backend

A backend says how many picture pixels each character cell holds, and
supplies a function that turns a frame into rows of text:

```c
static void draw(void* user, const termesh_plugin_frame* frame, termesh_plugin_output* out,
                 const termesh_host* host) {
    for (int row = 0; row < frame->rows; ++row) {
        /* frame->rgba holds frame->width x frame->height pixels, 4 bytes each;
           build this row's characters and colour escapes in `line`... */
        host->write_row(out, row, line, length);
    }
}

TERMESH_PLUGIN_EXPORT int termesh_plugin_init(const termesh_host* host) {
    termesh_plugin_backend backend;
    host->backend_init(&backend);
    backend.name = "mine";              /* what --backend takes */
    backend.description = "one line for --list-plugins";
    backend.cell_pixels_x = 2;          /* the frame is cols * 2 ... */
    backend.cell_pixels_y = 2;          /* ...by rows * 2 pixels */
    backend.draw = draw;
    return host->register_backend(host, &backend);
}
```

termesh does the rest: it sizes the picture, corrects for the shape of the
pixels, positions each row, resets colours at the end of it, and skips rows
that have not changed since the previous frame. `frame->color` says whether
the terminal has 24-bit colour, 256 colours or none; honour it. Set
`wants_wireframe` for a backend that can only show "on" and "off" (as
Braille does).

### Rules

- `termesh_plugin_init` returns 0 on success. Check `host->api_version` if
  you need something newer than version 1.
- Initialise structs with `host->mesh_init` / `host->backend_init`. That is
  what lets later versions of termesh add fields without breaking your
  plugin.
- Strings and `user` pointers you register must stay valid for the life of
  the program. Arrays passed to `add_mesh` are copied and can be freed
  straight away.
- Do not let C++ exceptions escape from your functions.
- `draw` is called from one thread at a time, and loaders likewise.
