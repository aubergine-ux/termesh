// STL, PLY, point clouds, vertex colours and the loader table.
#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "accel/model_accel.hpp"
#include "core/color.hpp"
#include "pipeline/renderer.hpp"
#include "scene/loader.hpp"
#include "scene/primitives.hpp"
#include "support.hpp"

using namespace termesh;
using doctest::Approx;

namespace {

// Raw bytes, for writing binary files.
struct Bytes {
    std::string data;
    Bytes& text(const std::string& s) {
        data += s;
        return *this;
    }
    template <typename T>
    Bytes& put(T value, bool bigEndian = false) {
        char raw[sizeof(T)];
        std::memcpy(raw, &value, sizeof(T));
        if (bigEndian) std::reverse(raw, raw + sizeof(T));
        data.append(raw, sizeof(T));
        return *this;
    }
};

std::string binaryStl(const std::vector<std::array<Vec3, 3>>& triangles, const std::string& header = "binary stl") {
    Bytes out;
    std::string padded = header;
    padded.resize(80, ' ');
    out.text(padded).put<std::uint32_t>(static_cast<std::uint32_t>(triangles.size()));
    for (const auto& t : triangles) {
        for (int k = 0; k < 3; ++k) out.put(0.0f);  // the stored normal: left as zero
        for (const Vec3& p : t) out.put(p.x).put(p.y).put(p.z);
        out.put<std::uint16_t>(0);
    }
    return out.data;
}

Camera lookFrom(Vec3 eye, Vec3 target) {
    Camera camera;
    camera.position = eye;
    camera.view = lookAt(eye, target, {0, 1, 0});
    camera.projection = perspective(radians(40.0f), 1.0f, 0.5f, 50.0f);
    return camera;
}

std::size_t covered(const Framebuffer& fb) {
    std::size_t n = 0;
    for (const Color8& p : fb.pixels()) n += p.a != 0;
    return n;
}

// A point cloud of the given points, as a model.
Model makeCloud(const std::vector<Vec3>& points, float radius, bool withNormals = false) {
    Model model;
    Mesh mesh;
    for (const Vec3 p : points) {
        Vertex v;
        v.position = p;
        v.normal = withNormals ? Vec3{0, 0, 1} : Vec3{0, 0, 0};
        mesh.vertices.push_back(v);
    }
    mesh.pointRadius = radius;
    model.meshes.push_back(std::move(mesh));
    model.materials.emplace_back();
    finalizeModel(model);
    return model;
}

}  // namespace

// --------------------------------------------------------------------- STL ---

TEST_CASE("stl: binary") {
    TempDir dir;
    const std::string path =
        dir.write("two.stl", binaryStl({{{{0, 0, 0}, {1, 0, 0}, {0, 1, 0}}}, {{{0, 0, 1}, {0, 1, 1}, {1, 0, 1}}}}));
    const LoadResult loaded = loadModel(path);
    REQUIRE_MESSAGE(loaded.ok, loaded.error);
    const Model& model = loaded.model;
    CHECK(model.name == "two.stl");
    REQUIRE(model.meshes.size() == 1);
    const Mesh& mesh = model.meshes[0];
    CHECK(model.triangleCount() == 2);
    // No shared vertices in STL: three per triangle.
    CHECK(mesh.vertices.size() == 6);
    CHECK(mesh.vertices[1].position == Vec3{1, 0, 0});
    // The stored normals were zero; they are worked out from the corners.
    CHECK(mesh.vertices[0].normal == Vec3{0, 0, 1});
    CHECK(mesh.vertices[3].normal == Vec3{0, 0, -1});
    CHECK_FALSE(mesh.hasUVs);
    CHECK(mesh.colors.empty());
    CHECK(model.materials.size() == 1);
}

TEST_CASE("stl: a binary file may begin with the word 'solid'") {
    // The header is free text, and exporters do write "solid" there. What
    // settles it is whether the length matches the triangle count.
    TempDir dir;
    const LoadResult loaded =
        loadModel(dir.write("sneaky.stl", binaryStl({{{{0, 0, 0}, {2, 0, 0}, {0, 2, 0}}}}, "solid not really ascii")));
    REQUIRE_MESSAGE(loaded.ok, loaded.error);
    CHECK(loaded.model.triangleCount() == 1);
    CHECK(loaded.model.meshes[0].vertices[1].position == Vec3{2, 0, 0});
}

TEST_CASE("stl: ascii") {
    TempDir dir;
    const std::string
        text =
            "solid wedge\n"
            " facet normal 0 0 1\n  outer loop\n   vertex 0 0 0\n   vertex 1.5 0 0\n   vertex 0 2e0 0\n  endloop\n "
            "endfacet\n"
            " facet normal 0 0 0\n  outer loop\n   vertex 0 0 0\n   vertex 0 0 0\n   vertex 0 0 0\n  endloop\n "
            "endfacet\n"  // no area
            " facet normal 1 0 0\n  outer loop\n   vertex 0 0 1\n   vertex -1 0 1\n   vertex 0 -1 1\n  endloop\n "
            "endfacet\n"
            "endsolid wedge\n";
    const LoadResult loaded = loadModel(dir.write("wedge.STL", text));  // the extension's case does not matter
    REQUIRE_MESSAGE(loaded.ok, loaded.error);
    // The degenerate facet is dropped.
    CHECK(loaded.model.triangleCount() == 2);
    CHECK(loaded.model.meshes[0].vertices[1].position == Vec3{1.5f, 0, 0});
    CHECK(loaded.model.meshes[0].vertices[2].position == Vec3{0, 2, 0});
    // The file's normal for the second facet was wrong; the corners win.
    CHECK(loaded.model.meshes[0].vertices[3].normal == Vec3{0, 0, 1});
    CHECK(loaded.warnings.empty());
}

TEST_CASE("stl: damaged files") {
    TempDir dir;
    CHECK_FALSE(loadModel(dir.file("missing.stl")).ok);
    CHECK_FALSE(loadModel(dir.write("empty.stl", "")).ok);
    CHECK_FALSE(loadModel(dir.write("text.stl", "this is not a model at all")).ok);
    CHECK_FALSE(loadModel(dir.write("none.stl", "solid nothing\nendsolid nothing\n")).ok);

    // A binary file cut short no longer matches its own triangle count.
    std::string cut = binaryStl({{{{0, 0, 0}, {1, 0, 0}, {0, 1, 0}}}, {{{0, 0, 1}, {0, 1, 1}, {1, 0, 1}}}});
    cut.resize(cut.size() - 20);
    CHECK_FALSE(loadModel(dir.write("cut.stl", cut)).ok);

    // A count far larger than the file is not believed (and not allocated).
    Bytes huge;
    huge.text(std::string(80, ' ')).put<std::uint32_t>(0xFFFFFFFFu);
    CHECK_FALSE(loadModel(dir.write("huge.stl", huge.data)).ok);

    // Not-a-number coordinates are skipped rather than drawn.
    const float nan = std::nanf("");
    const LoadResult withNan = loadModel(
        dir.write("nan.stl", binaryStl({{{{nan, 0, 0}, {1, 0, 0}, {0, 1, 0}}}, {{{0, 0, 0}, {1, 0, 0}, {0, 1, 0}}}})));
    REQUIRE(withNan.ok);
    CHECK(withNan.model.triangleCount() == 1);

    // An ASCII file that stops mid-facet loads what it can and says so.
    const LoadResult partial = loadModel(dir.write(
        "partial.stl",
        "solid p\nfacet normal 0 0 1\nouter loop\nvertex 0 0 0\nvertex 1 0 0\nvertex 0 1 0\nendloop\nendfacet\n"
        "facet normal 0 0 1\nouter loop\nvertex 0 0 0\nvertex 1 0"));
    REQUIRE(partial.ok);
    CHECK(partial.model.triangleCount() == 1);
    CHECK_FALSE(partial.warnings.empty());
}

// --------------------------------------------------------------------- PLY ---

TEST_CASE("ply: an ascii mesh with colours, a quad and things to skip") {
    TempDir dir;
    const std::string text =
        "ply\n"
        "format ascii 1.0\n"
        "comment made by hand\n"
        "element vertex 4\n"
        "property float x\nproperty float y\nproperty float z\n"
        "property uchar red\nproperty uchar green\nproperty uchar blue\n"
        "property list uchar float extra\n"  // a list in a vertex: skipped
        "element edge 1\n"                   // an element nothing here uses
        "property int vertex1\nproperty int vertex2\n"
        "element face 1\n"
        "property uchar flags\n"
        "property list uchar int vertex_indices\n"
        "end_header\n"
        "0 0 0  255 0 0    0\n"
        "1 0 0  0 255 0    2 9.5 9.5\n"
        "1 1 0  0 0 255    0\n"
        "0 1 0  128 128 128  1 7\n"
        "0 1\n"
        "5  4 0 1 2 3\n";
    const LoadResult loaded = loadModel(dir.write("quad.ply", text));
    REQUIRE_MESSAGE(loaded.ok, loaded.error);
    const Mesh& mesh = loaded.model.meshes[0];
    REQUIRE(mesh.vertices.size() == 4);
    CHECK(mesh.vertices[2].position == Vec3{1, 1, 0});
    // The quad becomes two triangles.
    CHECK(mesh.indices == std::vector<std::uint32_t>{0, 1, 2, 0, 2, 3});
    // No normals in the file: computed from the faces.
    CHECK(mesh.vertices[0].normal.z == Approx(1.0f));

    // Colours are decoded from sRGB to linear light.
    REQUIRE(mesh.colors.size() == 4);
    CHECK(mesh.colors[0] == Vec3{1, 0, 0});
    CHECK(mesh.colors[1] == Vec3{0, 1, 0});
    CHECK(mesh.colors[3].x == Approx(0.2158f).epsilon(0.01));
    CHECK_FALSE(mesh.isPointCloud());
    CHECK(loaded.model.materials[0].doubleSided);
}

TEST_CASE("ply: binary, in either byte order, reads the same as text") {
    TempDir dir;
    auto build = [&](const char* format, bool big) {
        Bytes out;
        out.text(std::string("ply\nformat ") + format +
                 " 1.0\nelement vertex 3\nproperty double x\nproperty float y\nproperty short z\n"
                 "property float nx\nproperty float ny\nproperty float nz\nproperty float s\nproperty float t\n"
                 "element face 1\nproperty list uint ushort vertex_index\nend_header\n");
        const Vec3 points[3] = {{0, 0, 0}, {2.5f, 0, 0}, {0, 3, -4}};
        for (const Vec3 p : points) {
            out.put<double>(p.x, big).put<float>(p.y, big).put<std::int16_t>(static_cast<std::int16_t>(p.z), big);
            out.put(0.0f, big).put(0.0f, big).put(2.0f, big);  // not unit length: normalised on the way in
            out.put(0.25f, big).put(0.75f, big);
        }
        out.put<std::uint32_t>(3, big).put<std::uint16_t>(0, big).put<std::uint16_t>(1, big).put<std::uint16_t>(2, big);
        return out.data;
    };
    const LoadResult little = loadModel(dir.write("le.ply", build("binary_little_endian", false)));
    const LoadResult big = loadModel(dir.write("be.ply", build("binary_big_endian", true)));
    REQUIRE_MESSAGE(little.ok, little.error);
    REQUIRE_MESSAGE(big.ok, big.error);
    for (const LoadResult* loaded : {&little, &big}) {
        const Mesh& mesh = loaded->model.meshes[0];
        REQUIRE(mesh.vertices.size() == 3);
        CHECK(mesh.vertices[1].position == Vec3{2.5f, 0, 0});
        CHECK(mesh.vertices[2].position == Vec3{0, 3, -4});
        CHECK(mesh.vertices[0].normal == Vec3{0, 0, 1});
        CHECK(mesh.hasUVs);
        CHECK(mesh.vertices[0].uv.x == Approx(0.25f));
        CHECK(mesh.vertices[0].uv.y == Approx(0.25f));  // v is flipped to run downwards
        CHECK(mesh.indices == std::vector<std::uint32_t>{0, 1, 2});
        CHECK(mesh.colors.empty());
    }
}

TEST_CASE("ply: a file with no faces is a point cloud") {
    TempDir dir;
    std::string text =
        "ply\nformat ascii 1.0\nelement vertex 500\nproperty float x\nproperty float y\nproperty float z\n"
        "property uchar red\nproperty uchar green\nproperty uchar blue\nend_header\n";
    // A 25 x 20 grid of points one unit apart; the colour encodes the index.
    for (int i = 0; i < 500; ++i)
        text += std::to_string(i % 25) + " " + std::to_string(i / 25) + " 0 " + std::to_string(i % 256) + " " +
                std::to_string(i / 256) + " 0\n";
    const LoadResult loaded = loadModel(dir.write("cloud.ply", text));
    REQUIRE_MESSAGE(loaded.ok, loaded.error);
    const Mesh& mesh = loaded.model.meshes[0];
    CHECK(mesh.isPointCloud());
    CHECK(mesh.name == "points");
    CHECK(loaded.model.triangleCount() == 0);
    REQUIRE(mesh.vertices.size() == 500);
    REQUIRE(mesh.colors.size() == 500);

    // No normals in the file: recorded as zero, which the renderer reads as
    // "facing the viewer".
    for (const Vertex& v : mesh.vertices) REQUIRE(v.normal == Vec3{0, 0, 0});

    // The points were shuffled, with their colours: every point is still
    // there, each still paired with the colour it came with.
    std::vector<int> seen(500, 0);
    bool inOrder = true;
    for (std::size_t k = 0; k < 500; ++k) {
        const int index =
            static_cast<int>(mesh.vertices[k].position.x) + 25 * static_cast<int>(mesh.vertices[k].position.y);
        REQUIRE(index >= 0);
        REQUIRE(index < 500);
        ++seen[static_cast<std::size_t>(index)];
        inOrder = inOrder && index == static_cast<int>(k);
        const float red = detail::srgbDecode(static_cast<float>(index % 256) / 255.0f);
        REQUIRE(mesh.colors[k].x == Approx(red).epsilon(0.001));
    }
    CHECK_FALSE(inOrder);
    for (const int count : seen) REQUIRE(count == 1);

    // The splat radius is about the spacing of the points.
    CHECK(mesh.pointRadius > 0.7f);
    CHECK(mesh.pointRadius < 3.0f);

    // The same file always loads the same way.
    const LoadResult again = loadModel(dir.file("cloud.ply"));
    REQUIRE(again.ok);
    CHECK(again.model.meshes[0].vertices[17].position == mesh.vertices[17].position);
}

TEST_CASE("ply: colours from Gaussian splatting files") {
    TempDir dir;
    const LoadResult loaded = loadModel(
        dir.write("splat.ply",
                  "ply\nformat ascii 1.0\nelement vertex 2\nproperty float x\nproperty float y\nproperty float z\n"
                  "property float f_dc_0\nproperty float f_dc_1\nproperty float f_dc_2\nproperty float opacity\n"
                  "property float scale_0\nend_header\n"
                  "0 0 0  1.7724 -1.7724 0  0.5 0.1\n"  // 0.5 +- 0.282 * 1.7724 = 1 and 0; 0.5
                  "1 1 1  0 0 0  0.5 0.1\n"));
    REQUIRE_MESSAGE(loaded.ok, loaded.error);
    const Mesh& mesh = loaded.model.meshes[0];
    REQUIRE(mesh.colors.size() == 2);
    // (The two points may have been swapped by the shuffle.)
    const Vec3 a = mesh.vertices[0].position == Vec3{0, 0, 0} ? mesh.colors[0] : mesh.colors[1];
    CHECK(a.x == Approx(1.0f).epsilon(0.01));
    CHECK(a.y == Approx(0.0f).scale(1.0).epsilon(0.01));
    CHECK(a.z == Approx(0.2140f).epsilon(0.02));  // sRGB 0.5
}

TEST_CASE("ply: damaged files") {
    TempDir dir;
    auto fails = [&](const std::string& name, const std::string& text) {
        const LoadResult loaded = loadModel(dir.write(name, text));
        CAPTURE(name);
        CHECK_FALSE(loaded.ok);
        CHECK_FALSE(loaded.error.empty());
    };
    const std::string head =
        "ply\nformat ascii 1.0\nelement vertex 3\nproperty float x\nproperty float y\nproperty float z\n";
    fails("a.ply", "");
    fails("b.ply", "plywood\n");
    fails("c.ply", "ply\nformat ascii 1.0\nelement vertex 3\nproperty float x\n");  // no end_header
    fails("d.ply", "ply\nformat morse 1.0\nelement vertex 1\nproperty float x\nend_header\n");
    fails("e.ply", "ply\nformat ascii 1.0\nproperty float x\nend_header\n");  // property before element
    fails("f.ply", "ply\nformat ascii 1.0\nelement vertex 1\nproperty quad x\nend_header\n1\n");
    fails("g.ply", "ply\nformat ascii 1.0\nelement vertex -4\nend_header\n");
    fails("h.ply", head + "end_header\n0 0 0\n1 0 0\n");                                        // one vertex short
    fails("i.ply", head + "end_header\n0 0 0\n1 zero 0\n0 1 0\n");                              // not a number
    fails("j.ply", "ply\nformat ascii 1.0\nelement vertex 0\nproperty float x\nend_header\n");  // nothing in it
    // A header promising more than the file could hold is refused before
    // anything is allocated for it.
    fails("k.ply", "ply\nformat binary_little_endian 1.0\nelement vertex 4000000000\nproperty float x\nend_header\n");
    Bytes cut;
    cut.text(
        "ply\nformat binary_little_endian 1.0\nelement vertex 2\nproperty float x\nproperty float y\nproperty float "
        "z\nend_header\n");
    cut.put(1.0f).put(2.0f).put(3.0f).put(4.0f);  // the second vertex is incomplete
    fails("l.ply", cut.data);

    // Faces that point at vertices which do not exist are skipped, with a warning.
    const LoadResult bad = loadModel(dir.write(
        "m.ply", head + "element face 3\nproperty list uchar int vertex_indices\nend_header\n0 0 0\n1 0 0\n0 1 0\n"
                        "3 0 1 2\n3 0 1 7\n3 0 -1 2\n"));
    REQUIRE_MESSAGE(bad.ok, bad.error);
    CHECK(bad.model.triangleCount() == 1);
    CHECK(bad.warnings.size() == 1);

    // Windows line endings in the header are fine.
    const LoadResult crlf =
        loadModel(dir.write("n.ply",
                            "ply\r\nformat ascii 1.0\r\nelement vertex 1\r\nproperty float x\r\nproperty float "
                            "y\r\nproperty float z\r\nend_header\r\n1 2 3\r\n"));
    REQUIRE_MESSAGE(crlf.ok, crlf.error);
    CHECK(crlf.model.meshes[0].vertices[0].position == Vec3{1, 2, 3});
}

// ----------------------------------------------------------------- loaders ---

TEST_CASE("loaders: the table can be extended") {
    const std::vector<std::string> extensions = modelExtensions();
    for (const char* builtIn : {"obj", "gltf", "glb", "stl", "ply"})
        CHECK(std::find(extensions.begin(), extensions.end(), builtIn) != extensions.end());
    CHECK(std::is_sorted(extensions.begin(), extensions.end()));

#ifndef TERMESH_HAS_ASSIMP
    // Without a loader, the error says what would have worked.
    const LoadResult unknown = loadModel("thing.xyz");
    CHECK_FALSE(unknown.ok);
    CHECK(unknown.error.find(".stl") != std::string::npos);
#endif

    // A new loader, for a made-up format: one line with a triangle count.
    registerModelLoader({".Tri", "tris"}, [](const std::string& path) {
        LoadResult result;
        result.model.name = "from " + path;
        result.model.meshes = makeDemoModel("cube").meshes;
        result.model.materials.emplace_back();
        result.ok = true;
        return result;
    });
    const LoadResult custom = loadModel("anything.TRI");  // dots and case are normalised
    REQUIRE(custom.ok);
    CHECK(custom.model.name == "from anything.TRI");
    CHECK(loadModel("other.tris").ok);
    const std::vector<std::string> after = modelExtensions();
    CHECK(std::find(after.begin(), after.end(), "tri") != after.end());

    // A fallback catches whatever nothing else claims...
    int calls = 0;
    registerFallbackModelLoader([&calls](const std::string&) {
        ++calls;
        LoadResult result;
        result.error = "the fallback was asked";
        return result;
    });
    CHECK(loadModel("thing.xyz").error == "the fallback was asked");
    CHECK(calls == 1);
    // ...but not what a registered loader does.
    CHECK(loadModel("anything.tri").ok);
    CHECK(calls == 1);

#ifdef TERMESH_HAS_ASSIMP
    registerFallbackModelLoader(loadWithAssimp);
#else
    registerFallbackModelLoader(nullptr);
#endif
}

#ifdef TERMESH_HAS_ASSIMP
TEST_CASE("assimp: formats termesh has no loader of its own for") {
    TempDir dir;
    // OFF, the Object File Format: a vertex count, a face count, then both lists.
    const LoadResult loaded = loadModel(dir.write("square.off", "OFF\n4 1 0\n0 0 0\n2 0 0\n2 2 0\n0 2 0\n4 0 1 2 3\n"));
    REQUIRE_MESSAGE(loaded.ok, loaded.error);
    CHECK(loaded.model.name == "square.off");
    CHECK(loaded.model.triangleCount() == 2);  // the quad is triangulated
    const Bounds box = loaded.model.bounds();
    CHECK(box.min == Vec3{0, 0, 0});
    CHECK(box.max == Vec3{2, 2, 0});
    CHECK(loaded.model.meshes[0].vertices[0].normal.z == Approx(1.0f));
    REQUIRE_FALSE(loaded.model.materials.empty());

    // Its errors come through as ours do.
    const LoadResult bad = loadModel(dir.write("bad.off", "this is not an OFF file"));
    CHECK_FALSE(bad.ok);
    CHECK(bad.error.find("bad.off") != std::string::npos);
    CHECK_FALSE(loadModel(dir.file("missing.fbx")).ok);
}
#endif

// ----------------------------------------------------------- vertex colour ---

TEST_CASE("vertex colours tint the surface in every shading mode") {
    // A square facing the camera: red on the left, green on the right.
    Model model;
    Mesh mesh;
    const Vec3 corners[4] = {{-1, -1, 0}, {1, -1, 0}, {1, 1, 0}, {-1, 1, 0}};
    for (int i = 0; i < 4; ++i) {
        Vertex v;
        v.position = corners[i];
        mesh.vertices.push_back(v);
        mesh.colors.push_back(corners[i].x < 0 ? Vec3{1, 0, 0} : Vec3{0, 1, 0});
    }
    mesh.indices = {0, 1, 2, 0, 2, 3};
    model.meshes.push_back(mesh);
    model.materials.emplace_back();
    finalizeModel(model);

    ThreadPool pool(2);
    Renderer renderer(pool);
    const Camera camera = lookFrom({0, 0, 6}, {0, 0, 0});
    for (ShadingMode mode :
         {ShadingMode::Flat, ShadingMode::Gouraud, ShadingMode::Phong, ShadingMode::Pbr, ShadingMode::Toon}) {
        RenderSettings settings;
        settings.shading = mode;
        Framebuffer fb(96, 96);
        renderer.render(model, camera, settings, fb);
        CAPTURE(shadingModeName(mode));
        // Just inside the left and right edges, and in the middle.
        const Color8 left = fb.at(28, 48), right = fb.at(68, 48), middle = fb.at(48, 48);
        REQUIRE(left.a == 255);
        REQUIRE(right.a == 255);
        // Clearly red on one side and green on the other. (Not purely so:
        // the highlight a light leaves on a surface is the light's colour,
        // whatever colour the surface is.)
        CHECK(left.r > left.g + 60);
        CHECK(right.g > right.r + 60);
        CHECK(left.b < left.r - 60);
        // Colours are interpolated across the surface like any other attribute.
        CHECK(middle.r > 40);
        CHECK(middle.g > 40);
    }

    // The base colour still applies: a blue-less tint of the vertex colours.
    model.materials[0].baseColor = {0.5f, 0.5f, 0.5f, 1.0f};
    Framebuffer bright(96, 96), dim(96, 96);
    Model full = model;
    full.materials[0].baseColor = {1, 1, 1, 1};
    renderer.render(full, camera, {}, bright);
    renderer.render(model, camera, {}, dim);
    CHECK(dim.at(28, 48).r < bright.at(28, 48).r);

    // Colours of the wrong length are dropped rather than read past the end.
    Model broken = model;
    broken.meshes[0].colors.pop_back();
    finalizeModel(broken);
    CHECK(broken.meshes[0].colors.empty());
}

TEST_CASE("gltf: vertex colours and point primitives") {
    GltfBuffer buffer;
    const int positions = buffer.floats({0, 0, 0, 1, 0, 0, 0, 2, 0}, 3, "VEC3", ",\"min\":[0,0,0],\"max\":[1,2,0]");
    const int colors = buffer.floats({1, 0, 0, 1, 0, 1, 0, 1, 0, 0, 1, 1}, 4, "VEC4");  // RGBA: alpha is ignored
    const std::string triangles =
        R"({"asset":{"version":"2.0"},"scene":0,"scenes":[{"nodes":[0]}],"nodes":[{"mesh":0}],
        "meshes":[{"primitives":[{"attributes":{"POSITION":)" +
        std::to_string(positions) + R"(,"COLOR_0":)" + std::to_string(colors) + "}}]}]," + buffer.json() + "}";
    TempDir dir;
    const LoadResult mesh = loadModel(dir.write("colors.gltf", triangles));
    REQUIRE_MESSAGE(mesh.ok, mesh.error);
    REQUIRE(mesh.model.meshes[0].colors.size() == 3);
    CHECK(mesh.model.meshes[0].colors[1] == Vec3{0, 1, 0});
    CHECK(mesh.model.triangleCount() == 1);

    // "mode": 0 is POINTS.
    const std::string points =
        R"({"asset":{"version":"2.0"},"scene":0,"scenes":[{"nodes":[0]}],"nodes":[{"mesh":0,"translation":[5,0,0]}],
        "meshes":[{"primitives":[{"mode":0,"attributes":{"POSITION":)" +
        std::to_string(positions) + R"(,"COLOR_0":)" + std::to_string(colors) + "}}]}]," + buffer.json() + "}";
    const LoadResult cloud = loadModel(dir.write("points.gltf", points));
    REQUIRE_MESSAGE(cloud.ok, cloud.error);
    const Mesh& m = cloud.model.meshes[0];
    CHECK(m.isPointCloud());
    REQUIRE(m.vertices.size() == 3);
    CHECK(m.vertices[1].position == Vec3{6, 0, 0});  // the node's transform still applies
    CHECK(m.colors.size() == 3);
    CHECK(m.vertices[0].normal == Vec3{0, 0, 0});
    CHECK(m.pointRadius > 0.0f);
}

TEST_CASE("emissive: a glow map's factor alone does not light the whole material") {
    Model model = makeDemoModel("cube");
    Material& material = model.materials[0];
    material.emissive = {1, 1, 1};
    // A black emissive map: nothing glows.
    model.textures.push_back(Texture(2, 2, std::vector<Color8>(4, Color8{0, 0, 0, 255}), true));
    material.emissiveTexture = static_cast<int>(model.textures.size()) - 1;

    ThreadPool pool(2);
    Renderer renderer(pool);
    OrbitCamera orbit;
    orbit.frame(model.bounds());
    RenderSettings settings;
    Framebuffer with(64, 64), without(64, 64), reference(64, 64);
    renderer.render(model, orbit.camera(1.0f), settings, with);
    settings.textures = false;
    renderer.render(model, orbit.camera(1.0f), settings, without);
    // With textures off the map cannot be read; the material must not turn
    // solid white because of its factor.
    int blown = 0;
    for (const Color8& p : without.pixels()) blown += p.a && p.r == 255 && p.g == 255 && p.b == 255;
    CHECK(blown < 64 * 64 / 50);

    // A material that glows without a map still does.
    Model plain = makeDemoModel("cube");
    plain.materials[0].emissive = {5, 5, 5};
    renderer.render(plain, orbit.camera(1.0f), settings, reference);
    CHECK(reference.at(32, 32).r > 245);
}

// ------------------------------------------------------------ point splats ---

TEST_CASE("splats: a point is drawn as a disc whose size follows its distance") {
    ThreadPool pool(2);
    Renderer renderer(pool);
    const Camera camera = lookFrom({0, 0, 10}, {0, 0, 0});
    RenderSettings settings;
    Framebuffer fb(200, 200);

    const Model one = makeCloud({{0, 0, 0}}, 1.0f);
    const RenderStats stats = renderer.render(one, camera, settings, fb);
    CHECK(stats.trianglesSubmitted == 2);  // each point is two triangles
    const std::size_t area = covered(fb);
    REQUIRE(area > 0);
    CHECK(fb.at(100, 100).a == 255);

    // Find its extent, and check it is round: a disc fills pi/4 of its
    // bounding square.
    int minX = 200, maxX = -1, minY = 200, maxY = -1;
    for (int y = 0; y < 200; ++y)
        for (int x = 0; x < 200; ++x)
            if (fb.at(x, y).a)
                minX = std::min(minX, x), maxX = std::max(maxX, x), minY = std::min(minY, y), maxY = std::max(maxY, y);
    const int width = maxX - minX + 1, height = maxY - minY + 1;
    CHECK(std::abs(width - height) <= 1);
    CHECK(static_cast<double>(area) / (width * height) == Approx(kPi / 4.0).epsilon(0.06));
    // A radius of 1 at distance 10 with a 40 degree view: 1 / (10 tan 20) of
    // half the image, each way.
    CHECK(width == Approx(200.0 / (10.0 * std::tan(radians(20.0f)))).epsilon(0.06));

    // Nearer is bigger; a larger scale is bigger; both by the expected amount.
    const Model near = makeCloud({{0, 0, 5}}, 1.0f);
    renderer.render(near, camera, settings, fb);
    CHECK(static_cast<double>(covered(fb)) == Approx(4.0 * static_cast<double>(area)).epsilon(0.08));
    settings.pointScale = 0.5f;
    renderer.render(one, camera, settings, fb);
    CHECK(static_cast<double>(covered(fb)) == Approx(0.25 * static_cast<double>(area)).epsilon(0.1));

    // However small or far, a point never vanishes between pixels.
    settings.pointScale = 1e-6f;
    renderer.render(one, camera, settings, fb);
    CHECK(covered(fb) >= 1);
    CHECK(covered(fb) <= 12);

    // Behind the camera: not drawn (and not wrapped round to the front).
    settings.pointScale = 1.0f;
    renderer.render(makeCloud({{0, 0, 30}}, 1.0f), camera, settings, fb);
    CHECK(covered(fb) == 0);
}

TEST_CASE("splats: lit by their normal, or facing the viewer without one") {
    ThreadPool pool(2);
    Renderer renderer(pool);
    const Camera camera = lookFrom({0, 0, 10}, {0, 0, 0});
    Framebuffer bare(64, 64), facing(64, 64), away(64, 64);

    for (ShadingMode mode : {ShadingMode::Flat, ShadingMode::Gouraud, ShadingMode::Phong, ShadingMode::Pbr}) {
        RenderSettings settings;
        settings.shading = mode;
        CAPTURE(shadingModeName(mode));
        // No normal: shaded as if it faced the camera, so it is well lit.
        renderer.render(makeCloud({{0, 0, 0}}, 2.0f), camera, settings, bare);
        renderer.render(makeCloud({{0, 0, 0}}, 2.0f, true), camera, settings, facing);
        CHECK(luma(bare.at(32, 32)) > 90);
        CHECK(std::abs(luma(bare.at(32, 32)) - luma(facing.at(32, 32))) < 12);

        // A normal pointing away from a one-sided light is dark.
        Model back = makeCloud({{0, 0, 0}}, 2.0f, true);
        back.meshes[0].vertices[0].normal = {1, 0, 0};  // edge-on to the camera and its key light
        renderer.render(back, camera, settings, away);
        CHECK(luma(away.at(32, 32)) < luma(facing.at(32, 32)));
    }
}

TEST_CASE("splats: any mesh can be drawn as points") {
    const Model cube = makeDemoModel("cube");
    ThreadPool pool(2);
    Renderer renderer(pool);
    OrbitCamera orbit;
    orbit.frame(cube.bounds());
    const Camera camera = orbit.camera(1.0f);

    RenderSettings settings;
    Framebuffer solid(128, 128), points(128, 128);
    const RenderStats asMesh = renderer.render(cube, camera, settings, solid);
    settings.points = true;
    settings.pointScale = 0.3f;
    const RenderStats asPoints = renderer.render(cube, camera, settings, points);
    CHECK(asMesh.trianglesSubmitted == 12);
    CHECK(asPoints.trianglesSubmitted == cube.vertexCount() * 2);
    // Dots at the corners, not filled faces.
    CHECK(covered(points) > 20);
    CHECK(covered(points) < covered(solid) / 2);
    // The centre of the front face is empty; a corner is not.
    CHECK(points.at(64, 64).a == 0);

    // A textured mesh's points take their colour from the texture: the
    // crate is not grey.
    int coloured = 0;
    for (const Color8& p : points.pixels()) coloured += p.a && std::abs(p.r - p.b) > 25;
    CHECK(coloured > 10);

    // Wireframe leaves points as points.
    settings.wireframe = true;
    Framebuffer wire(128, 128);
    renderer.render(cube, camera, settings, wire);
    CHECK(covered(wire) == covered(points));
    // Points cast shadows like anything else.
    settings.wireframe = false;
    settings.shadows = true;
    CHECK(renderer.render(cube, camera, settings, wire).shadowMs > 0.0);
}

TEST_CASE("splats: a distant cloud is drawn with fewer, larger points") {
    // A flat sheet of 200 x 200 points.
    std::vector<Vec3> points;
    for (int y = 0; y < 200; ++y)
        for (int x = 0; x < 200; ++x)
            points.push_back({static_cast<float>(x) * 0.01f - 1.0f, static_cast<float>(y) * 0.01f - 1.0f, 0});
    Model model = makeCloud(points, 0.01f);
    shufflePoints(model.meshes[0]);
    const ModelAccel accel(model);
    const DrawItem item{&model, Mat4::identity(), &accel};
    ThreadPool pool(4);
    Renderer renderer(pool);
    Framebuffer all(96, 96), thinned(96, 96);

    // Far enough that the sheet is a few dozen pixels across: thousands of
    // points per pixel row.
    const Camera camera = lookFrom({0, 0, 20}, {0, 0, 0});
    RenderSettings settings;
    const RenderStats full = renderer.render(std::span<const DrawItem>(&item, 1), camera, settings, all);
    settings.lodTolerance = 0.5f;
    const RenderStats fewer = renderer.render(std::span<const DrawItem>(&item, 1), camera, settings, thinned);
    CHECK(fewer.trianglesDrawn < full.trianglesDrawn / 4);
    // ...covering the same area.
    CHECK(static_cast<double>(covered(thinned)) == Approx(static_cast<double>(covered(all))).epsilon(0.15));

    // Close up, every point is wanted.
    const Camera close = lookFrom({0, 0, 1.5f}, {0, 0, 0});
    const RenderStats near = renderer.render(std::span<const DrawItem>(&item, 1), close, settings, thinned);
    settings.lodTolerance = 0.0f;
    const RenderStats nearFull = renderer.render(std::span<const DrawItem>(&item, 1), close, settings, all);
    CHECK(near.trianglesDrawn == nearFull.trianglesDrawn);
}

TEST_CASE("points: radius estimates and shuffling") {
    // A grid of points two units apart.
    std::vector<Vec3> grid;
    for (int y = 0; y < 30; ++y)
        for (int x = 0; x < 30; ++x) grid.push_back({static_cast<float>(x) * 2.0f, static_cast<float>(y) * 2.0f, 0});
    Model model = makeCloud(grid, 0.0f);  // 0: work it out
    const float radius = model.meshes[0].pointRadius;
    CHECK(radius > 2.0f);  // at least the spacing, so neighbours touch
    CHECK(radius < 5.0f);

    // Scale the cloud and the estimate scales with it.
    for (Vec3& p : grid) p = p * 100.0f;
    CHECK(makeCloud(grid, 0.0f).meshes[0].pointRadius == Approx(radius * 100.0f).epsilon(0.05));

    // A triangle mesh gets one too (for --points): a fraction of an edge.
    const Model cube = makeDemoModel("cube");
    CHECK(cube.meshes[0].pointRadius > 0.05f);
    CHECK(cube.meshes[0].pointRadius < 1.0f);

    // Degenerate clouds do not divide by zero.
    CHECK(makeCloud({{1, 1, 1}}, 0.0f).meshes[0].pointRadius > 0.0f);
    CHECK(makeCloud({{1, 1, 1}, {1, 1, 1}, {1, 1, 1}}, 0.0f).meshes[0].pointRadius > 0.0f);

    // Shuffling is a permutation, the same one every time.
    Mesh a = model.meshes[0], b = model.meshes[0];
    shufflePoints(a);
    shufflePoints(b);
    bool same = true, moved = false;
    for (std::size_t i = 0; i < a.vertices.size(); ++i) {
        same = same && a.vertices[i].position == b.vertices[i].position;
        moved = moved || !(a.vertices[i].position == model.meshes[0].vertices[i].position);
    }
    CHECK(same);
    CHECK(moved);
}
