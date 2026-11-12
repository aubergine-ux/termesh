/*
 * A loader plugin: .xyz point clouds.
 *
 * XYZ is the simplest point-cloud format there is - a text file with one
 * point per line:
 *
 *     x y z
 *     x y z r g b        (colour, each 0-255)
 *
 * Lines that are blank or start with '#' are skipped.
 *
 *     cc -shared -fPIC -I<termesh>/include xyz.c -o xyz.so
 *     termesh --plugin ./xyz.so scan.xyz
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <termesh_plugin.h>

/* An sRGB byte as a linear value, which is what the renderer works in. */
static float linear(double byte) {
    const double c = byte < 0.0 ? 0.0 : (byte > 255.0 ? 1.0 : byte / 255.0);
    return (float)(c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4));
}

static int load(void* user, const char* path, termesh_plugin_model* model, const termesh_host* host) {
    FILE* file = fopen(path, "r");
    float* positions = NULL;
    float* colors = NULL;
    size_t count = 0, capacity = 0, colored = 0;
    char line[512];
    int status = 1;
    termesh_plugin_mesh mesh;
    (void)user;

    if (!file) {
        host->set_error(model, "could not open the file");
        return 1;
    }
    while (fgets(line, sizeof(line), file)) {
        double x, y, z, r = 255.0, g = 255.0, b = 255.0;
        const int fields = sscanf(line, "%lf %lf %lf %lf %lf %lf", &x, &y, &z, &r, &g, &b);
        if (line[0] == '#' || fields < 3) continue;
        if (count == capacity) {
            float* grown;
            capacity = capacity ? capacity * 2 : 1024;
            grown = (float*)realloc(positions, capacity * 3 * sizeof(float));
            if (grown) positions = grown;
            grown = grown ? (float*)realloc(colors, capacity * 3 * sizeof(float)) : NULL;
            if (!grown) {
                host->set_error(model, "out of memory");
                goto done;
            }
            colors = grown;
        }
        positions[count * 3] = (float)x, positions[count * 3 + 1] = (float)y, positions[count * 3 + 2] = (float)z;
        colors[count * 3] = linear(r), colors[count * 3 + 1] = linear(g), colors[count * 3 + 2] = linear(b);
        if (fields >= 6) ++colored;
        ++count;
    }
    if (count == 0) {
        host->set_error(model, "no points in the file");
        goto done;
    }

    host->mesh_init(&mesh);
    mesh.positions = positions;
    mesh.vertex_count = count;
    mesh.colors = colored ? colors : NULL; /* no indices: a point cloud */
    mesh.name = "points";
    status = host->add_mesh(model, &mesh);

done:
    fclose(file);
    free(positions);
    free(colors);
    return status;
}

TERMESH_PLUGIN_EXPORT int termesh_plugin_init(const termesh_host* host) {
    if (host->api_version < 1) return 1;
    return host->register_loader(host, "xyz", load, NULL);
}
