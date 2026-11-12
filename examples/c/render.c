/*
 * Using termesh from C: put a model on the terminal.
 *
 *     render                 the built-in knot
 *     render model.glb       a model, or a scene file
 *
 * Build it against an installed libtermesh with
 *
 *     cc render.c $(pkg-config --cflags --libs termesh) -o render
 */
#include <stdio.h>

#include <termesh.h>

static int fail(const char* what) {
    fprintf(stderr, "%s: %s\n", what, termesh_last_error());
    return 1;
}

int main(int argc, char** argv) {
    termesh_scene* scene;
    termesh_renderer* renderer;
    termesh_camera camera;
    termesh_settings settings;
    termesh_light sun;
    char* text;
    size_t length;

    if (argc > 1) {
        scene = termesh_scene_open(argv[1]);
        if (!scene) return fail(argv[1]);
    } else {
        scene = termesh_scene_create();
        if (!scene || termesh_scene_add_demo(scene, "knot") < 0) return fail("knot");

        /* A light of our own, casting a shadow. */
        termesh_light_init(&sun);
        sun.direction[0] = -1.0f, sun.direction[1] = -2.0f, sun.direction[2] = -1.5f;
        sun.intensity = 3.0f;
        sun.casts_shadow = 1;
        termesh_scene_add_light(scene, &sun);
    }

    /* Start from what the scene file asked for, if it was one. */
    if (termesh_settings_from_scene(scene, &settings) < 0) return fail("settings");
    settings.shading = TERMESH_SHADING_PBR;
    settings.shadows = 1;

    if (!termesh_scene_camera(scene, &camera)) termesh_camera_frame(scene, 30.0f, 20.0f, 1.0f, 40.0f, &camera);

    renderer = termesh_renderer_create(0);
    if (!renderer) return fail("renderer");

    text = termesh_render_text(renderer, scene, &camera, &settings, TERMESH_BACKEND_HALFBLOCK, TERMESH_COLOR_TRUECOLOR,
                               72, 24, &length);
    if (!text) return fail("render");
    fwrite(text, 1, length, stdout);
    termesh_free(text);

    fprintf(stderr, "termesh %s: %lu triangles\n", termesh_version(),
            (unsigned long)termesh_scene_triangle_count(scene));

    termesh_renderer_destroy(renderer);
    termesh_scene_destroy(scene);
    return 0;
}
