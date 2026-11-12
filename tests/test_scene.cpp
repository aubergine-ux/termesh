#include <doctest/doctest.h>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>

#include <stb/stb_image_write.h>

#include "core/color.hpp"
#include "core/texture.hpp"
#include "pipeline/resolve.hpp"
#include "scene/camera.hpp"
#include "scene/loader.hpp"
#include "scene/primitives.hpp"
#include "support.hpp"

using namespace termesh;
using doctest::Approx;

namespace {

void writePngFile(const std::string& path, int w, int h, const std::vector<Color8>& pixels) {
    REQUIRE(stbi_write_png(path.c_str(), w, h, 4, pixels.data(), w * 4) != 0);
}

}  // namespace

// ----------------------------------------------------------------- colour ---

TEST_CASE("color: sRGB conversion round-trips and is monotonic") {
    CHECK(srgbToLinear(std::uint8_t{0}) == 0.0f);
    CHECK(srgbToLinear(std::uint8_t{255}) == Approx(1.0f));
    CHECK(srgbToLinear(std::uint8_t{128}) == Approx(0.2158f).epsilon(0.01));  // mid grey is ~21% light
    for (int i = 0; i < 256; ++i) {
        CAPTURE(i);
        CHECK(linearToSrgb(srgbToLinear(static_cast<std::uint8_t>(i))) == i);
    }
    CHECK(linearToSrgb(-1.0f) == 0);
    CHECK(linearToSrgb(7.0f) == 255);
    CHECK(linearToSrgb(std::nanf("")) == 0);
}

// ---------------------------------------------------------------- texture ---

TEST_CASE("texture: mip chain") {
    std::vector<Color8> pixels(8 * 4, Color8{255, 255, 255, 255});
    const Texture tex(8, 4, pixels, true);
    CHECK(tex.levelCount() == 4);  // 8x4, 4x2, 2x1, 1x1
    CHECK(tex.sample({0.3f, 0.6f}, 10.0f).x == Approx(1.0f));
}

TEST_CASE("texture: mips are averaged in linear light") {
    // A black and white checkerboard averages to 50% *light*, which is sRGB
    // 188 - not 128, which would look far too dark.
    std::vector<Color8> pixels(4);
    pixels[0] = pixels[3] = {255, 255, 255, 255};
    pixels[1] = pixels[2] = {0, 0, 0, 255};
    const Texture tex(2, 2, pixels, true);
    const Vec4 top = tex.sampleLevel({0.5f, 0.5f}, 1);
    CHECK(top.x == Approx(0.5f).epsilon(0.02));
    CHECK(linearToSrgb(top.x) == Approx(188).epsilon(0.02));
}

TEST_CASE("texture: bilinear filtering and wrap modes") {
    // Two texels: black on the left, white on the right.
    std::vector<Color8> pixels = {{0, 0, 0, 255}, {255, 255, 255, 255}};
    Texture tex(2, 1, pixels, false);

    // Texel centres are at u = 0.25 and 0.75.
    CHECK(tex.sampleLevel({0.25f, 0.5f}, 0).x == Approx(0.0f));
    CHECK(tex.sampleLevel({0.75f, 0.5f}, 0).x == Approx(1.0f));
    CHECK(tex.sampleLevel({0.5f, 0.5f}, 0).x == Approx(0.5f));

    SUBCASE("repeat blends across the seam") {
        tex.setWrap(WrapMode::Repeat, WrapMode::Repeat);
        CHECK(tex.sampleLevel({0.0f, 0.5f}, 0).x == Approx(0.5f));
        CHECK(tex.sampleLevel({1.25f, 0.5f}, 0).x == Approx(0.0f));
        CHECK(tex.sampleLevel({-0.25f, 0.5f}, 0).x == Approx(1.0f));
    }
    SUBCASE("clamp holds the edge texel") {
        tex.setWrap(WrapMode::Clamp, WrapMode::Clamp);
        CHECK(tex.sampleLevel({0.0f, 0.5f}, 0).x == Approx(0.0f));
        CHECK(tex.sampleLevel({5.0f, 0.5f}, 0).x == Approx(1.0f));
    }
    SUBCASE("mirror reflects at each edge") {
        tex.setWrap(WrapMode::Mirror, WrapMode::Mirror);
        CHECK(tex.sampleLevel({1.25f, 0.5f}, 0).x == Approx(1.0f));  // mirrored 0.75
        CHECK(tex.sampleLevel({-0.25f, 0.5f}, 0).x == Approx(0.0f));
    }
}

TEST_CASE("texture: level of detail picks smaller mips when minified") {
    // Left half black, right half white: level 0 is sharp, the last level grey.
    std::vector<Color8> pixels(16 * 16);
    for (int y = 0; y < 16; ++y)
        for (int x = 0; x < 16; ++x) pixels[y * 16 + x] = x < 8 ? Color8{0, 0, 0, 255} : Color8{255, 255, 255, 255};
    const Texture tex(16, 16, pixels, false);

    // lodBias is relative to the whole texture; -4 means one pixel covers
    // 1/256th of it, i.e. exactly one texel of a 16x16 image.
    CHECK(tex.sample({0.1f, 0.5f}, -4.0f).x == Approx(0.0f));
    CHECK(tex.sample({0.9f, 0.5f}, -4.0f).x == Approx(1.0f));
    // One pixel covers the whole texture: everything blurs to the average.
    CHECK(tex.sample({0.1f, 0.5f}, 0.0f).x == Approx(0.5f).epsilon(0.02));
}

TEST_CASE("texture: height maps become normal maps") {
    // A ramp rising to the right: normals lean left (-x), z stays positive.
    std::vector<Color8> pixels(8 * 8);
    for (int y = 0; y < 8; ++y)
        for (int x = 0; x < 8; ++x) {
            const auto v = static_cast<std::uint8_t>(x * 30);
            pixels[y * 8 + x] = {v, v, v, 255};
        }
    const Texture height(8, 8, pixels, false);
    CHECK(height.isGreyscale());
    const Texture normals = heightToNormalMap(height);
    const Color8 n = normals.pixels()[3 * 8 + 3];
    CHECK(n.r < 128);                 // x < 0
    CHECK(std::abs(n.g - 128) <= 1);  // y = 0
    CHECK(n.b > 128);                 // z > 0
    CHECK_FALSE(normals.isGreyscale());
}

// ------------------------------------------------------------------ model ---

TEST_CASE("model: generated normals and tangents") {
    Mesh mesh;
    // A unit quad in the XY plane, facing +Z, with a standard UV layout.
    const Vec3 positions[4] = {{0, 0, 0}, {1, 0, 0}, {1, 1, 0}, {0, 1, 0}};
    const Vec2 uvs[4] = {{0, 1}, {1, 1}, {1, 0}, {0, 0}};
    for (int i = 0; i < 4; ++i) {
        Vertex v;
        v.position = positions[i];
        v.uv = uvs[i];
        v.normal = {0, 0, 0};
        mesh.vertices.push_back(v);
    }
    mesh.indices = {0, 1, 2, 0, 2, 3};

    computeNormals(mesh);
    computeTangents(mesh);
    for (const Vertex& v : mesh.vertices) {
        CHECK(v.normal.z == Approx(1.0f));
        // u increases along +X, and the texture's "up" is +Y.
        CHECK(v.tangent.x == Approx(1.0f));
        CHECK(v.tangent.w == 1.0f);
        const Vec3 bitangent = cross(v.normal, v.tangent.xyz()) * v.tangent.w;
        CHECK(bitangent.y == Approx(1.0f));
    }

    SUBCASE("mirrored UVs flip the handedness") {
        for (Vertex& v : mesh.vertices) v.uv.x = 1.0f - v.uv.x;
        computeTangents(mesh);
        for (const Vertex& v : mesh.vertices) {
            CHECK(v.tangent.x == Approx(-1.0f));
            CHECK(v.tangent.w == -1.0f);
        }
    }
}

TEST_CASE("model: demo models are well-formed") {
    for (const std::string& name : demoModelNames()) {
        CAPTURE(name);
        const Model model = makeDemoModel(name);
        REQUIRE(!model.meshes.empty());
        CHECK(model.triangleCount() > 0);
        for (const Mesh& mesh : model.meshes) {
            CHECK(mesh.indices.size() % 3 == 0);
            bool inRange = true;
            for (std::uint32_t i : mesh.indices) inRange = inRange && i < mesh.vertices.size();
            CHECK(inRange);
            for (const Vertex& v : mesh.vertices) {
                REQUIRE(length(v.normal) == Approx(1.0f).epsilon(1e-3));
                REQUIRE(length(v.tangent.xyz()) == Approx(1.0f).epsilon(1e-3));
            }
        }
        CHECK(model.bounds().radius() > 0.0f);
    }
    CHECK(makeDemoModel("no-such-thing").meshes.empty());
}

// ----------------------------------------------------------------- camera ---

TEST_CASE("camera: framing fits the bounding sphere in view") {
    OrbitCamera cam;
    Bounds bounds{{-1, -2, -3}, {5, 4, 3}};
    cam.frame(bounds);
    CHECK(cam.target == bounds.center());
    CHECK(length(cam.position() - cam.target) == Approx(cam.distance));

    // The centre projects to the middle of the screen, in front of the camera.
    const Camera c = cam.camera(1.0f);
    const Vec4 clip = c.projection * c.view * Vec4(bounds.center(), 1.0f);
    CHECK(clip.x / clip.w == Approx(0.0f).scale(1.0));
    CHECK(clip.y / clip.w == Approx(0.0f).scale(1.0));
    CHECK(clip.w > 0.0f);

    // Every corner of the box is inside the frustum.
    for (int i = 0; i < 8; ++i) {
        const Vec3 corner{i & 1 ? bounds.max.x : bounds.min.x, i & 2 ? bounds.max.y : bounds.min.y,
                          i & 4 ? bounds.max.z : bounds.min.z};
        const Vec4 p = c.projection * c.view * Vec4(corner, 1.0f);
        CAPTURE(i);
        CHECK(std::abs(p.x) <= p.w);
        CHECK(std::abs(p.y) <= p.w);
        CHECK(std::abs(p.z) <= p.w);
    }
}

TEST_CASE("camera: orbit, pan and zoom") {
    OrbitCamera cam;
    cam.frame({{-1, -1, -1}, {1, 1, 1}});
    cam.yaw = cam.pitch = 0.0f;

    // At yaw 0 the camera sits on +Z looking towards -Z.
    CHECK(cam.position().z == Approx(cam.distance));
    cam.orbit(radians(90.0f), 0.0f);
    CHECK(cam.position().x == Approx(cam.distance));
    CHECK(cam.position().z == Approx(0.0f).scale(1.0));

    // Pitch is clamped short of the poles.
    cam.orbit(0.0f, radians(500.0f));
    CHECK(cam.pitch < radians(90.0f));
    CHECK(cam.position().y > 0.0f);

    // Zoom changes the distance but is clamped to something sensible.
    const float before = cam.distance;
    cam.zoom(0.5f);
    CHECK(cam.distance == Approx(before * 0.5f));
    for (int i = 0; i < 100; ++i) cam.zoom(0.5f);
    CHECK(cam.distance > 0.0f);

    // Panning moves the target sideways in the image plane, keeping the
    // viewing direction.
    OrbitCamera pan;
    pan.frame({{-1, -1, -1}, {1, 1, 1}});
    pan.yaw = pan.pitch = 0.0f;
    pan.pan(0.5f, 0.0f);
    CHECK(pan.target.x > 0.0f);
    CHECK(pan.target.y == Approx(0.0f).scale(1.0));
    CHECK(pan.target.z == Approx(0.0f).scale(1.0));
}

// ---------------------------------------------------------------- resolve ---

TEST_CASE("resolve: box filter in linear light, alpha as coverage") {
    ThreadPool pool(2);
    Framebuffer fb(4, 2);
    fb.clear({0, 0, 0, 0});
    // Left 2x2 block: half white geometry, half background.
    fb.at(0, 0) = fb.at(1, 1) = {255, 255, 255, 255};
    Image out;
    resolve(fb, 2, out, pool);
    REQUIRE(out.width == 2);
    REQUIRE(out.height == 1);
    CHECK(out.row(0)[0].r == Approx(188).epsilon(0.02));  // 50% light, not sRGB 128
    CHECK(out.row(0)[0].a == Approx(128).epsilon(0.02));
    CHECK(out.row(0)[1].r == 0);
    CHECK(out.row(0)[1].a == 0);
}

// ---------------------------------------------------------------- loaders ---

TEST_CASE("obj: positions, uvs, normals, quads and materials") {
    TempDir dir;
    writePngFile(dir.file("checker.png"), 2, 2,
                 {{255, 0, 0, 255}, {0, 255, 0, 255}, {0, 0, 255, 255}, {255, 255, 255, 255}});
    dir.write("cube.mtl",
              "# materials\n"
              "newmtl painted\n"
              "Kd 1.0 0.5 0.0\n"
              "Ns 96\n"
              "map_Kd checker.png\n"
              "newmtl plain\n"
              "Kd 0.2 0.2 0.2\n"
              "d 1.0\n");
    const std::string path = dir.write("cube.obj",
                                       "mtllib cube.mtl\r\n"  // Windows line endings must not confuse the parser
                                       "v -1 -1 0\n"
                                       "v  1 -1 0\n"
                                       "v  1  1 0\n"
                                       "v -1  1 0\n"
                                       "v  0  0 2   # apex\n"
                                       "vt 0 0\n"
                                       "vt 1 0\n"
                                       "vt 1 1\n"
                                       "vt 0 1\n"
                                       "vn 0 0 1\n"
                                       "usemtl painted\n"
                                       "f 1/1/1 2/2/1 3/3/1 4/4/1\n"  // a quad: two triangles
                                       "usemtl plain\n"
                                       "f -5 -4 -1\n"          // negative (relative) indices, no uv / normal
                                       "f 2//1 3//1 5//1\n");  // position//normal form

    const LoadResult result = loadModel(path);
    REQUIRE_MESSAGE(result.ok, result.error);
    CHECK(result.warnings.empty());
    const Model& model = result.model;

    REQUIRE(model.meshes.size() == 2);
    REQUIRE(model.materials.size() == 2);
    CHECK(model.triangleCount() == 4);

    const Mesh& painted = model.meshes[0];
    CHECK(model.materials[painted.material].name == "painted");
    CHECK(painted.vertices.size() == 4);
    CHECK(painted.indices.size() == 6);
    CHECK(painted.hasUVs);
    // OBJ's v axis points up; ours points down.
    CHECK(painted.vertices[0].uv == Vec2{0.0f, 1.0f});
    CHECK(painted.vertices[2].uv == Vec2{1.0f, 0.0f});
    CHECK(painted.vertices[0].normal == Vec3{0, 0, 1});

    const Material& mat = model.materials[painted.material];
    REQUIRE(mat.baseColorTexture >= 0);
    CHECK(model.textures[mat.baseColorTexture].width() == 2);
    CHECK(mat.baseColor.x == Approx(1.0f));
    CHECK(mat.baseColor.y == Approx(0.214f).epsilon(0.01));  // 0.5 sRGB in linear
    CHECK(mat.roughness < 0.6f);                             // Ns 96 is fairly glossy

    const Mesh& plain = model.meshes[1];
    CHECK(model.materials[plain.material].name == "plain");
    CHECK(plain.indices.size() == 6);
    CHECK_FALSE(plain.hasUVs);
    // "f -5 -4 -1" resolves to vertices 1, 2 and 5.
    CHECK(plain.vertices[plain.indices[0]].position == Vec3{-1, -1, 0});
    CHECK(plain.vertices[plain.indices[2]].position == Vec3{0, 0, 2});
    // Missing normals were generated.
    for (const Vertex& v : plain.vertices) CHECK(length(v.normal) == Approx(1.0f));
}

TEST_CASE("obj: errors and warnings") {
    TempDir dir;
    CHECK_FALSE(loadModel(dir.file("missing.obj")).ok);

    const LoadResult empty = loadModel(dir.write("empty.obj", "v 0 0 0\nv 1 0 0\n"));
    CHECK_FALSE(empty.ok);
    CHECK(empty.error.find("no faces") != std::string::npos);

    // A missing material library is not fatal.
    const LoadResult noMtl = loadModel(dir.write("tri.obj", "mtllib nope.mtl\nv 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n"));
    CHECK(noMtl.ok);
    CHECK(noMtl.warnings.size() == 1);
    CHECK(noMtl.model.materials.size() == 1);  // the default material

    CHECK_FALSE(loadModel("model.stl").ok);
}

TEST_CASE("obj: comments, malformed lines and odd indices") {
    TempDir dir;
    // A comment ends its own line and nothing else; CRLF endings and a
    // missing final newline are fine.
    const LoadResult commented = loadModel(dir.write(
        "c.obj", "v 0 0 0 # origin\r\nv 1 0 0\r\n# f 1 2 3\r\nv 0 1 0\r\nv 1 1 0\r\nf 1 2 3 # first\r\nf 2 4 3"));
    REQUIRE(commented.ok);
    CHECK(commented.model.triangleCount() == 2);

    // Corners that point outside the arrays are dropped; a face left with
    // fewer than three corners disappears. Negative indices count backwards.
    const LoadResult indices = loadModel(dir.write(
        "i.obj", "v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 9\nf 0 1 2\nf -3 -2 -1\nf 1/7/7 2/7/7 3/7/7\nf x y z\nf\n"));
    REQUIRE(indices.ok);
    CHECK(indices.model.triangleCount() == 2);

    // An empty "mtllib" is ignored rather than reported as a missing file.
    const LoadResult noName = loadModel(dir.write("m.obj", "mtllib\nusemtl\nv 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n"));
    REQUIRE(noName.ok);
    CHECK(noName.warnings.empty());
}

TEST_CASE("obj: a large file loads in linear time") {
    // 200k triangles with no comment anywhere: scanning for '#' beyond the
    // current line used to make this quadratic (minutes instead of a blink).
    const int n = 316;
    std::string text;
    text.reserve(8u << 20);
    for (int j = 0; j <= n; ++j)
        for (int i = 0; i <= n; ++i) text += "v " + std::to_string(i) + " 0 " + std::to_string(j) + "\n";
    for (int j = 0; j < n; ++j)
        for (int i = 0; i < n; ++i) {
            const int a = j * (n + 1) + i + 1, b = a + n + 1;
            text += "f " + std::to_string(a) + " " + std::to_string(b) + " " + std::to_string(a + 1) + "\n";
            text += "f " + std::to_string(a + 1) + " " + std::to_string(b) + " " + std::to_string(b + 1) + "\n";
        }
    TempDir dir;
    const auto start = std::chrono::steady_clock::now();
    const LoadResult big = loadModel(dir.write("big.obj", text));
    const std::chrono::duration<double> elapsed = std::chrono::steady_clock::now() - start;
    REQUIRE(big.ok);
    CHECK(big.model.triangleCount() == static_cast<std::size_t>(2 * n * n));
    CHECK(elapsed.count() < 20.0);  // generous: the quadratic version needs far longer
}

TEST_CASE("gltf: embedded buffer, node transform and material") {
    TempDir dir;
    // One triangle: three float3 positions followed by three uint16 indices.
    // Buffer bytes (base64): positions (0,0,0) (1,0,0) (0,1,0); indices 0 1 2.
    const std::string gltf = R"({
      "asset": {"version": "2.0"},
      "scene": 0,
      "scenes": [{"nodes": [0]}],
      "nodes": [{"mesh": 0, "translation": [10, 0, 0], "scale": [2, 2, 2]}],
      "meshes": [{"primitives": [{"attributes": {"POSITION": 0}, "indices": 1, "material": 0}]}],
      "materials": [{
        "name": "red",
        "doubleSided": true,
        "alphaMode": "MASK",
        "alphaCutoff": 0.25,
        "pbrMetallicRoughness": {"baseColorFactor": [1, 0, 0, 1], "metallicFactor": 0.25, "roughnessFactor": 0.75}
      }],
      "buffers": [{"byteLength": 42, "uri": "data:application/octet-stream;base64,AAAAAAAAAAAAAAAAAACAPwAAAAAAAAAAAAAAAAAAgD8AAAAAAAABAAIA"}],
      "bufferViews": [
        {"buffer": 0, "byteOffset": 0, "byteLength": 36},
        {"buffer": 0, "byteOffset": 36, "byteLength": 6}
      ],
      "accessors": [
        {"bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3", "min": [0, 0, 0], "max": [1, 1, 0]},
        {"bufferView": 1, "componentType": 5123, "count": 3, "type": "SCALAR"}
      ]
    })";
    const LoadResult result = loadModel(dir.write("tri.gltf", gltf));
    REQUIRE_MESSAGE(result.ok, result.error);
    const Model& model = result.model;

    REQUIRE(model.meshes.size() == 1);
    const Mesh& mesh = model.meshes[0];
    REQUIRE(mesh.vertices.size() == 3);
    CHECK(mesh.indices == std::vector<std::uint32_t>{0, 1, 2});

    // The node's scale and translation are baked into the vertices.
    CHECK(mesh.vertices[0].position == Vec3{10, 0, 0});
    CHECK(mesh.vertices[1].position == Vec3{12, 0, 0});
    CHECK(mesh.vertices[2].position == Vec3{10, 2, 0});
    // No normals in the file: generated from the geometry.
    CHECK(mesh.vertices[0].normal.z == Approx(1.0f));

    const Material& m = model.materials[mesh.material];
    CHECK(m.name == "red");
    CHECK(m.baseColor == Vec4{1, 0, 0, 1});
    CHECK(m.metallic == Approx(0.25f));
    CHECK(m.roughness == Approx(0.75f));
    CHECK(m.doubleSided);
    CHECK(m.alphaMode == AlphaMode::Mask);
    CHECK(m.alphaCutoff == Approx(0.25f));
}

TEST_CASE("gltf: a mirroring transform flips the winding back") {
    TempDir dir;
    const std::string gltf = R"({
      "asset": {"version": "2.0"},
      "scenes": [{"nodes": [0]}],
      "nodes": [{"mesh": 0, "scale": [-1, 1, 1]}],
      "meshes": [{"primitives": [{"attributes": {"POSITION": 0}, "indices": 1}]}],
      "buffers": [{"byteLength": 42, "uri": "data:application/octet-stream;base64,AAAAAAAAAAAAAAAAAACAPwAAAAAAAAAAAAAAAAAAgD8AAAAAAAABAAIA"}],
      "bufferViews": [{"buffer": 0, "byteOffset": 0, "byteLength": 36}, {"buffer": 0, "byteOffset": 36, "byteLength": 6}],
      "accessors": [
        {"bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3", "min": [0, 0, 0], "max": [1, 1, 0]},
        {"bufferView": 1, "componentType": 5123, "count": 3, "type": "SCALAR"}
      ]
    })";
    const LoadResult result = loadModel(dir.write("mirror.gltf", gltf));
    REQUIRE_MESSAGE(result.ok, result.error);
    const Mesh& mesh = result.model.meshes[0];
    CHECK(mesh.indices == std::vector<std::uint32_t>{0, 2, 1});
    // Still counter-clockwise seen from +Z, so the generated normal faces +Z.
    const Vec3 a = mesh.vertices[mesh.indices[0]].position, b = mesh.vertices[mesh.indices[1]].position,
               c = mesh.vertices[mesh.indices[2]].position;
    CHECK(cross(b - a, c - a).z > 0.0f);
    // The default material is filled in.
    CHECK(result.model.materials.size() == 1);
}

TEST_CASE("gltf: garbage is rejected with an error") {
    TempDir dir;
    CHECK_FALSE(loadModel(dir.write("bad.gltf", "{ this is not json")).ok);
    CHECK_FALSE(loadModel(dir.write("bad.glb", "glTF but not really")).ok);
    CHECK_FALSE(loadModel(dir.file("missing.glb")).ok);
}
