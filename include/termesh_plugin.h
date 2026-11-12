/*
 * termesh_plugin.h - writing plugins for termesh.
 *
 * A plugin is a shared library that teaches the termesh program something
 * new: how to read another file format, or another way of drawing a picture
 * with text. It needs this header and nothing else - it does not link
 * against termesh. Everything it needs from the program arrives as a table
 * of functions, the "host".
 *
 *     #include <termesh_plugin.h>
 *
 *     static int load(void* user, const char* path, termesh_plugin_model* model,
 *                     const termesh_host* host) { ... host->add_mesh(model, &mesh) ... }
 *
 *     TERMESH_PLUGIN_EXPORT int termesh_plugin_init(const termesh_host* host) {
 *         return host->register_loader(host, "xyz,pts", load, NULL);
 *     }
 *
 * Build it as a shared library (cc -shared -fPIC plugin.c -o plugin.so) and
 * either name it on the command line (termesh --plugin ./plugin.so file.xyz)
 * or put it in termesh's plugin directory, from where it is loaded every
 * time. docs/plugins.md has the details; examples/plugins/ has two complete
 * plugins.
 *
 * The interface is C, and versioned: a plugin built against this header
 * keeps working with later versions of termesh. Structs carry their size
 * for the same reason; initialise them with the host's *_init functions.
 */
#ifndef TERMESH_PLUGIN_H
#define TERMESH_PLUGIN_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TERMESH_PLUGIN_API_VERSION 1

#if defined(_WIN32)
#define TERMESH_PLUGIN_EXPORT __declspec(dllexport)
#else
#define TERMESH_PLUGIN_EXPORT __attribute__((visibility("default")))
#endif

typedef struct termesh_host termesh_host;

/* ---------------------------------------------------------------- loaders */

/* The model a loader is filling in. */
typedef struct termesh_plugin_model termesh_plugin_model;

/* One mesh: a set of triangles (or points) sharing a material. */
typedef struct termesh_plugin_mesh {
    size_t struct_size;

    const float* positions;  /* x y z per vertex */
    size_t vertex_count;
    const uint32_t* indices; /* three per triangle; NULL makes it a point cloud */
    size_t index_count;

    const float* normals; /* optional: x y z per vertex; computed if NULL */
    const float* uvs;     /* optional: u v per vertex */
    const float* colors;  /* optional: linear r g b per vertex */

    const char* name;     /* optional */
    float base_color[4];  /* linear r g b, and alpha */
    float metallic;
    float roughness;
    int double_sided;
} termesh_plugin_mesh;

/* Read the file at `path` into `model` by calling host->add_mesh for each
 * mesh. Return 0 on success; otherwise call host->set_error first, and
 * return non-zero. `user` is whatever was given when registering. */
typedef int (*termesh_plugin_load_fn)(void* user, const char* path, termesh_plugin_model* model,
                                      const termesh_host* host);

/* --------------------------------------------------------------- backends */

/* Where a backend writes its text. */
typedef struct termesh_plugin_output termesh_plugin_output;

typedef enum termesh_plugin_color {
    TERMESH_PLUGIN_COLOR_TRUECOLOR = 0, /* 24-bit: ESC [ 38;2;r;g;b m */
    TERMESH_PLUGIN_COLOR_256 = 1,       /* the xterm palette: ESC [ 38;5;n m */
    TERMESH_PLUGIN_COLOR_NONE = 2       /* no colour escapes at all */
} termesh_plugin_color;

/* What a backend is given to draw. */
typedef struct termesh_plugin_frame {
    const uint8_t* rgba; /* width * height * 4 bytes: sRGB r g b, and coverage */
    int width, height;   /* cols * cell_pixels_x by rows * cell_pixels_y */
    int cols, rows;      /* the size of the area in character cells */
    int color;           /* termesh_plugin_color: what the terminal can show */
} termesh_plugin_frame;

/* A way of drawing with text: each character cell shows a small block of
 * the picture's pixels. */
typedef struct termesh_plugin_backend {
    size_t struct_size;
    const char* name;        /* what --backend takes */
    const char* description; /* one line, for --list-plugins */
    int cell_pixels_x;       /* picture pixels across one cell... */
    int cell_pixels_y;       /* ...and down it */
    int supersampling;       /* render at this many times the size and average down; 0 = default */
    int wants_wireframe;     /* draw edges only: for backends that can only show "on" or "off" */

    /* Turn the frame into text: call host->write_row once for each row of
     * cells, top to bottom, with that row's characters and escape
     * sequences. termesh positions the rows, resets the colours after each,
     * and skips rows that have not changed since the last frame. */
    void (*draw)(void* user, const termesh_plugin_frame* frame, termesh_plugin_output* out, const termesh_host* host);
    void* user;
} termesh_plugin_backend;

/* ------------------------------------------------------------------- host */

struct termesh_host {
    size_t struct_size;
    int api_version; /* the TERMESH_PLUGIN_API_VERSION of the running program */
    const char* termesh_version;

    /* Offer a loader for files with these extensions: lower case, without
     * dots, separated by commas ("xyz,pts"). Returns 0 on success. */
    int (*register_loader)(const termesh_host* host, const char* extensions, termesh_plugin_load_fn load, void* user);

    /* Offer a backend. The struct is copied; the strings and `user` it
     * points to must stay valid. Returns 0 on success. */
    int (*register_backend)(const termesh_host* host, const termesh_plugin_backend* backend);

    /* For loaders. */
    void (*mesh_init)(termesh_plugin_mesh* mesh);
    /* Copies the arrays. Returns 0, or non-zero (having set the error) if
     * the mesh is malformed. */
    int (*add_mesh)(termesh_plugin_model* model, const termesh_plugin_mesh* mesh);
    void (*set_error)(termesh_plugin_model* model, const char* message);

    /* For backends. */
    void (*backend_init)(termesh_plugin_backend* backend);
    void (*write_row)(termesh_plugin_output* out, int row, const char* utf8, size_t length);

    void* reserved; /* the host's own */
};

/* The one function every plugin exports. Called once, when the plugin is
 * loaded; register what the plugin provides and return 0. A plugin that
 * needs a newer API than host->api_version should return non-zero. */
TERMESH_PLUGIN_EXPORT int termesh_plugin_init(const termesh_host* host);

#ifdef __cplusplus
}
#endif

#endif /* TERMESH_PLUGIN_H */
