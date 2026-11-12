/*
 * termesh.h - the termesh renderer as a C library.
 *
 * Everything termesh draws, without the terminal program around it: load or
 * build a scene, then render it either to pixels or to the text a terminal
 * would display.
 *
 *     termesh_scene* scene = termesh_scene_open("model.glb");
 *     termesh_renderer* renderer = termesh_renderer_create(0);
 *
 *     termesh_camera camera;
 *     termesh_camera_frame(scene, 30.0f, 15.0f, 1.0f, 40.0f, &camera);
 *
 *     size_t length;
 *     char* text = termesh_render_text(renderer, scene, &camera, NULL,
 *                                      TERMESH_BACKEND_HALFBLOCK, TERMESH_COLOR_TRUECOLOR,
 *                                      80, 24, &length);
 *     fwrite(text, 1, length, stdout);
 *     termesh_free(text);
 *
 *     termesh_renderer_destroy(renderer);
 *     termesh_scene_destroy(scene);
 *
 * Conventions
 *
 *   - Functions that can fail return NULL, or a negative int; then
 *     termesh_last_error() says why. Functions returning an index return it
 *     (>= 0) on success.
 *   - Coordinates are right-handed with +y up; a camera looks down -z.
 *     Matrices are 16 floats in column-major order. Angles are in degrees.
 *     Colours are linear RGB unless stated otherwise.
 *   - Strings are UTF-8. Strings returned by the library belong to it and
 *     stay valid until the object they came from changes or is destroyed -
 *     except where a function says its result must be passed to
 *     termesh_free().
 *   - A scene or a renderer may be used from one thread at a time.
 *     Different scenes and renderers can be used from different threads.
 *
 * Compatibility
 *
 *   The API is versioned by TERMESH_API_VERSION. Within a version, functions
 *   are only added, never changed or removed. Structs that the caller fills
 *   in begin with a struct_size field, set by their *_init function: that
 *   lets later versions append fields without breaking programs compiled
 *   against an earlier header. Always initialise such a struct with its
 *   *_init function before setting fields.
 */
#ifndef TERMESH_H
#define TERMESH_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(_WIN32) && defined(TERMESH_SHARED)
#if defined(TERMESH_BUILDING)
#define TERMESH_API __declspec(dllexport)
#else
#define TERMESH_API __declspec(dllimport)
#endif
#elif defined(__GNUC__) || defined(__clang__)
#define TERMESH_API __attribute__((visibility("default")))
#else
#define TERMESH_API
#endif

#define TERMESH_API_VERSION 1

/* The library's version ("0.2.0"), and the API version it implements. */
TERMESH_API const char* termesh_version(void);
TERMESH_API int termesh_api_version(void);

/* Why the last call on this thread failed. Empty if it did not. */
TERMESH_API const char* termesh_last_error(void);

/* Release memory returned by the library (see termesh_render_text). */
TERMESH_API void termesh_free(void* memory);

/* ------------------------------------------------------------------ scenes */

typedef struct termesh_scene termesh_scene;

/* An empty scene. */
TERMESH_API termesh_scene* termesh_scene_create(void);

/* A scene from a file: a scene file (.json), or a model (.obj, .gltf, .glb,
 * .stl, .ply, ...) which becomes a scene of one object. */
TERMESH_API termesh_scene* termesh_scene_open(const char* path);

TERMESH_API void termesh_scene_destroy(termesh_scene* scene);

/* Add a model from a file, or one of the built-in models ("knot", "cube",
 * "sphere", "tentacle", "plane"). Returns the new object's index. */
TERMESH_API int termesh_scene_add_file(termesh_scene* scene, const char* path);
TERMESH_API int termesh_scene_add_demo(termesh_scene* scene, const char* name);

/* Geometry from memory. */
typedef struct termesh_mesh {
    size_t struct_size;

    const float* positions; /* x y z per vertex */
    size_t vertex_count;
    const uint32_t* indices; /* three per triangle; NULL draws the vertices as a point cloud */
    size_t index_count;

    const float* normals; /* optional: x y z per vertex; computed if NULL */
    const float* uvs;     /* optional: u v per vertex, (0, 0) at the top left */
    const float* colors;  /* optional: r g b per vertex */

    const char* name;    /* optional */
    float base_color[4]; /* r g b a; a < 1 is translucent */
    float metallic;
    float roughness;
    int double_sided;
} termesh_mesh;

TERMESH_API void termesh_mesh_init(termesh_mesh* mesh);
/* The arrays are copied; they need not outlive the call. */
TERMESH_API int termesh_scene_add_mesh(termesh_scene* scene, const termesh_mesh* mesh);

TERMESH_API int termesh_scene_object_count(const termesh_scene* scene);
TERMESH_API const char* termesh_scene_object_name(const termesh_scene* scene, int object);
/* Place an object: its model-to-world matrix. */
TERMESH_API int termesh_scene_set_transform(termesh_scene* scene, int object, const float matrix[16]);
TERMESH_API int termesh_scene_set_visible(termesh_scene* scene, int object, int visible);

TERMESH_API size_t termesh_scene_triangle_count(const termesh_scene* scene);
TERMESH_API size_t termesh_scene_vertex_count(const termesh_scene* scene);
/* The box around everything visible. */
TERMESH_API void termesh_scene_bounds(const termesh_scene* scene, float min[3], float max[3]);

/* --------------------------------------------------------------- animation */

/* How many animations an object has (0 for a static model). */
TERMESH_API int termesh_scene_animation_count(const termesh_scene* scene, int object);
TERMESH_API const char* termesh_scene_animation_name(const termesh_scene* scene, int object, int animation);
TERMESH_API float termesh_scene_animation_duration(const termesh_scene* scene, int object, int animation);
/* Switch animation, blending from the current one over fade_seconds. */
TERMESH_API int termesh_scene_play(termesh_scene* scene, int object, int animation, float fade_seconds);
/* Jump to a time in the current animation. */
TERMESH_API int termesh_scene_seek(termesh_scene* scene, int object, float seconds);
TERMESH_API int termesh_scene_set_speed(termesh_scene* scene, int object, float speed);
/* Move every animation on by dt seconds. */
TERMESH_API void termesh_scene_advance(termesh_scene* scene, float dt);

/* ------------------------------------------------------------------ lights */

typedef enum termesh_light_type {
    TERMESH_LIGHT_DIRECTIONAL = 0,
    TERMESH_LIGHT_POINT = 1,
    TERMESH_LIGHT_SPOT = 2
} termesh_light_type;

typedef struct termesh_light {
    size_t struct_size;
    int type;           /* termesh_light_type */
    float direction[3]; /* the way the light travels (directional, spot) */
    float position[3];  /* point, spot */
    float color[3];
    float intensity;
    float range;      /* point, spot: 0 is unlimited */
    float inner_cone; /* spot: full brightness inside this angle from the axis... */
    float outer_cone; /* ...none outside this one */
    int casts_shadow; /* directional and spot; also needs termesh_settings.shadows */
    int attached_to_camera;
} termesh_light;

TERMESH_API void termesh_light_init(termesh_light* light);
/* With no lights, a default pair attached to the camera is used. */
TERMESH_API int termesh_scene_add_light(termesh_scene* scene, const termesh_light* light);
TERMESH_API void termesh_scene_clear_lights(termesh_scene* scene);

/* Ambient light and reflections: "studio", "sky", "sunset", or the path of
 * an HDR panorama (.hdr). */
TERMESH_API int termesh_scene_set_environment(termesh_scene* scene, const char* name_or_path);

/* ------------------------------------------------------------------ camera */

typedef struct termesh_camera {
    float position[3];
    float target[3];
    float fov; /* vertical field of view */
} termesh_camera;

/* A camera that fits the whole scene in view. yaw turns it about the
 * vertical axis and pitch raises it; zoom > 1 moves closer. */
TERMESH_API void termesh_camera_frame(const termesh_scene* scene, float yaw, float pitch, float zoom, float fov,
                                      termesh_camera* camera);

/* The camera a scene file asked for. Returns 1 and fills in *camera if it
 * has one, 0 if not. */
TERMESH_API int termesh_scene_camera(const termesh_scene* scene, termesh_camera* camera);

/* ---------------------------------------------------------------- settings */

typedef enum termesh_shading {
    TERMESH_SHADING_FLAT = 0,
    TERMESH_SHADING_GOURAUD = 1,
    TERMESH_SHADING_PHONG = 2,
    TERMESH_SHADING_PBR = 3,
    TERMESH_SHADING_TOON = 4
} termesh_shading;

typedef enum termesh_tone_mapper {
    TERMESH_TONE_ACES = 0,
    TERMESH_TONE_REINHARD = 1,
    TERMESH_TONE_FILMIC = 2,
    TERMESH_TONE_LINEAR = 3
} termesh_tone_mapper;

typedef enum termesh_dither {
    TERMESH_DITHER_AUTO = 0, /* whatever suits the backend */
    TERMESH_DITHER_NONE = 1,
    TERMESH_DITHER_ORDERED = 2,
    TERMESH_DITHER_FLOYD_STEINBERG = 3
} termesh_dither;

typedef struct termesh_settings {
    size_t struct_size;
    int shading; /* termesh_shading */
    int wireframe;
    int textures;
    int normal_maps;
    int cull_backfaces;
    int points;            /* draw every mesh as points */
    float point_size;      /* scale of point splats */
    uint8_t background[4]; /* sRGB r g b; the fourth byte is unused */
    int shadows;
    int shadow_map_size;
    int shadow_softness; /* 1 (hard) to 7 */
    int skybox;          /* draw the environment as the background; -1 leaves it to the scene */
    float sky_blur;      /* 0 to 1; negative leaves it to the scene */
    int ssao;
    int bloom;
    int outline; /* -1 draws outlines with toon shading only */
    int fxaa;
    int tone_mapper;     /* termesh_tone_mapper */
    float exposure;      /* in stops */
    int supersampling;   /* 1 to 4; 0 picks a default */
    float lod_tolerance; /* level of detail: error allowed, in pixels; 0 is off */
    int dither;          /* termesh_dither; for termesh_render_text */
} termesh_settings;

/* The defaults. */
TERMESH_API void termesh_settings_init(termesh_settings* settings);
/* The defaults, with the "render" block of the scene's file (if it came from
 * one that has it) applied on top. */
TERMESH_API int termesh_settings_from_scene(const termesh_scene* scene, termesh_settings* settings);

/* --------------------------------------------------------------- rendering */

typedef struct termesh_renderer termesh_renderer;

/* threads: how many to render with; 0 uses every core. */
TERMESH_API termesh_renderer* termesh_renderer_create(int threads);
TERMESH_API void termesh_renderer_destroy(termesh_renderer* renderer);

/* Render to pixels: width * height * 4 bytes of sRGB r g b a, row 0 at the
 * top. Alpha is how much of the pixel is covered by geometry: 255 on the
 * model, 0 on the background, in between along edges. settings may be NULL
 * for the defaults. An image can have up to 8 million pixels. */
TERMESH_API int termesh_render_pixels(termesh_renderer* renderer, termesh_scene* scene, const termesh_camera* camera,
                                      const termesh_settings* settings, int width, int height, uint8_t* rgba);

typedef enum termesh_backend {
    TERMESH_BACKEND_HALFBLOCK = 0, /* two pixels per cell, 24-bit or 256 colours */
    TERMESH_BACKEND_BRAILLE = 1,   /* 2 x 4 dots per cell */
    TERMESH_BACKEND_ASCII = 2,     /* one shaded character per cell */
    TERMESH_BACKEND_SIXEL = 3,     /* pixels, via DEC sixel graphics */
    TERMESH_BACKEND_KITTY = 4      /* pixels, via the kitty graphics protocol */
} termesh_backend;

typedef enum termesh_color { TERMESH_COLOR_TRUECOLOR = 0, TERMESH_COLOR_256 = 1, TERMESH_COLOR_NONE = 2 } termesh_color;

/* Render to the text a terminal would display: `rows` lines for a cols x
 * rows area, ready to write to a terminal. Returns a NUL-terminated string
 * that must be released with termesh_free(); its length, if wanted, is
 * stored in *length. */
TERMESH_API char* termesh_render_text(termesh_renderer* renderer, termesh_scene* scene, const termesh_camera* camera,
                                      const termesh_settings* settings, int backend, int color, int cols, int rows,
                                      size_t* length);

/* ----------------------------------------------------------------- picking */

typedef struct termesh_hit {
    int object;        /* index of the object hit */
    int mesh;          /* index of the mesh within it */
    uint32_t triangle; /* index of the triangle within that */
    float distance;    /* from the camera */
    float position[3];
} termesh_hit;

/* What is under a point of the image. (x, y) run from -1 to 1 across the
 * image with +y up; aspect is the image's width / height. Returns 1 and
 * fills in *hit if something is there, 0 if not. */
TERMESH_API int termesh_pick(termesh_scene* scene, const termesh_camera* camera, float aspect, float x, float y,
                             termesh_hit* hit);

#ifdef __cplusplus
}
#endif

#endif /* TERMESH_H */
