#include "pipeline/renderer.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>

#include "core/color.hpp"
#include "pipeline/shading.hpp"

namespace termesh {

const char* shadingModeName(ShadingMode mode) {
    switch (mode) {
        case ShadingMode::Flat:    return "flat";
        case ShadingMode::Gouraud: return "gouraud";
        default:                   return "phong";
    }
}

namespace {

// ---------------------------------------------------------------- varyings ---
//
// Layout of the per-vertex varyings array. Texture coordinates always come
// first. What follows depends on where lighting is evaluated:
//
//   Flat / Gouraud  lighting is already computed, so the varyings carry its
//                   result: diffuse light RGB and specular light RGB.
//   Phong           lighting happens per pixel, so the varyings carry its
//                   inputs: normal, world position and tangent.
constexpr int kUV = 0;
constexpr int kDiffuse = 2, kSpecular = 5;          // Flat / Gouraud
constexpr int kNormal = 2, kWorld = 5, kTangent = 8;  // Phong
constexpr int kLitVaryingCount = 8;
constexpr int kPhongVaryingCount = 11;

constexpr int kTrianglesPerChunk = 512;
constexpr int kVerticesPerChunk = 2048;

void store(float* dst, Vec3 v) {
    dst[0] = v.x;
    dst[1] = v.y;
    dst[2] = v.z;
}
Vec3 load3(const float* src) { return {src[0], src[1], src[2]}; }

double msSince(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}

// ---------------------------------------------------------- fragment stage ---

struct FragmentShader {
    const Model* model;
    Lights lights;
    Vec3 eye;
    Color8 background;
    int lineWidth;
    bool perPixelLighting, wireframe, textures, normalMaps;

    int varyingCount() const { return perPixelLighting ? kPhongVaryingCount : kLitVaryingCount; }

    // In wireframe mode the triangles themselves only lay down depth (pushed
    // back a little, see rasterizeTile) and the edges are drawn over them.
    float depthSlopeBias() const { return wireframe ? static_cast<float>(lineWidth) * 0.5f + 1.0f : 0.0f; }

    // Triangle interiors.
    bool operator()(const RasterTriangle& tri, const Fragment& frag, Color8& out) const {
        if (!wireframe) return shade(tri, frag, out);
        // Paint background, but still occupy the depth buffer so that lines
        // behind this surface stay hidden. Cut-outs remain see-through.
        if (!passesAlphaTest(tri, frag)) return false;
        out = background;
        return true;
    }

    // Wireframe lines: shaded like the surface they belong to, then
    // lightened so they read against a dark background.
    bool shadeEdge(const RasterTriangle& tri, const Fragment& frag, Color8& out) const {
        return shade(tri, frag, out);
    }

    bool passesAlphaTest(const RasterTriangle& tri, const Fragment& frag) const {
        const Material& material = model->materials[tri.material];
        if (material.alphaMode != AlphaMode::Mask) return true;
        float alpha = material.baseColor.w;
        if (material.baseColorTexture >= 0)
            alpha *= model->textures[material.baseColorTexture].sample({frag.varyings[kUV], frag.varyings[kUV + 1]}, tri.lod).w;
        return alpha >= material.alphaCutoff;
    }

    bool shade(const RasterTriangle& tri, const Fragment& frag, Color8& out) const {
        const Material& material = model->materials[tri.material];
        const float* v = frag.varyings;
        const Vec2 uv{v[kUV], v[kUV + 1]};

        // Base colour, and alpha cut-outs (leaves, fences, decals).
        Vec4 base = material.baseColor;
        if (material.baseColorTexture >= 0 && (textures || material.alphaMode == AlphaMode::Mask)) {
            const Vec4 texel = model->textures[material.baseColorTexture].sample(uv, tri.lod);
            if (textures) base = base * texel;
            else base.w *= texel.w;
        }
        if (material.alphaMode == AlphaMode::Mask && base.w < material.alphaCutoff) return false;

        float metallic = material.metallic, roughness = material.roughness, occlusion = 1.0f;
        if (textures && material.metallicRoughnessTexture >= 0) {
            const Vec4 mr = model->textures[material.metallicRoughnessTexture].sample(uv, tri.lod);
            roughness *= mr.y;
            metallic *= mr.z;
            if (material.occlusionInRed) occlusion = mr.x;
        }

        Lighting light;
        if (perPixelLighting) {
            // Interpolating unit normals shortens them, so renormalise.
            Vec3 n = normalize(load3(v + kNormal));
            if (tri.backFacing) n = -n;

            if (normalMaps && material.normalTexture >= 0) {
                // A normal map stores directions relative to the surface: x
                // along the tangent (texture u), y along the bitangent, z
                // along the surface normal. Rebuild that frame at this pixel
                // and use it to rotate the sampled normal into world space.
                Vec3 t = load3(v + kTangent);
                t = t - n * dot(n, t);  // re-orthogonalise against n
                if (lengthSquared(t) > 1e-12f) {
                    t = normalize(t);
                    const Vec3 b = cross(n, t) * tri.tangentSign;
                    const Vec4 s = model->textures[material.normalTexture].sample(uv, tri.lod);
                    const float nx = (s.x * 2.0f - 1.0f) * material.normalScale;
                    const float ny = (s.y * 2.0f - 1.0f) * material.normalScale;
                    const float nz = s.z * 2.0f - 1.0f;
                    n = normalize(t * nx + b * ny + n * nz);
                }
            }
            const Vec3 toEye = normalize(eye - load3(v + kWorld));
            light = computeLighting(lights, n, toEye, roughness);
        } else {
            light.diffuse = load3(v + kDiffuse);
            light.specular = load3(v + kSpecular);
        }

        // Metals have no diffuse reflection; their colour tints the specular
        // reflection instead. Non-metals reflect about 4% specularly.
        const Vec3 albedo = base.xyz();
        const Vec3 diffuseColor = albedo * (1.0f - metallic);
        const Vec3 specularColor = lerp(Vec3(0.04f), albedo, metallic);

        Vec3 color = diffuseColor * light.diffuse * occlusion + specularColor * light.specular;

        Vec3 emissive = material.emissive;
        if (textures && material.emissiveTexture >= 0)
            emissive *= model->textures[material.emissiveTexture].sample(uv, tri.lod).xyz();
        color += emissive;

        if (wireframe) color = lerp(color, Vec3(1.0f), 0.3f);

        out = linearToSrgb(toneMap(color));
        return true;
    }
};

// --------------------------------------------------------- primitive setup ---

struct SetupContext {
    float width, height;         // framebuffer size
    std::int32_t maxX, maxY;     // ...in fixed point
    bool cullBackfaces;
    int varyingCount;
};

struct TriangleInfo {
    std::uint16_t material;
    float tangentSign;
    bool twoSided, needsLod;
};

// Project three clip-space vertices to the screen and append the resulting
// triangle, unless it is back-facing or degenerate.
void emitTriangle(const ClipVertex* verts[3], std::uint8_t edgeMask, const TriangleInfo& info,
                  const SetupContext& ctx, std::vector<RasterTriangle>& out) {
    std::int32_t x[3], y[3];
    float z[3], invW[3];
    for (int i = 0; i < 3; ++i) {
        const Vec4 p = verts[i]->position;
        // The perspective divide: clip space -> normalised device coordinates
        // in [-1, 1]. Clipping has already guaranteed w > 0.
        invW[i] = 1.0f / p.w;
        const float ndcX = p.x * invW[i], ndcY = p.y * invW[i], ndcZ = p.z * invW[i];

        // Viewport transform: NDC -> pixels. NDC +y is up but row 0 of the
        // framebuffer is the top, so y flips here.
        const float sx = (ndcX * 0.5f + 0.5f) * ctx.width;
        const float sy = (0.5f - ndcY * 0.5f) * ctx.height;

        // Snap to the subpixel grid. The clamp only absorbs rounding error.
        x[i] = std::clamp(static_cast<std::int32_t>(std::floor(sx * kSubpixelScale + 0.5f)), 0, ctx.maxX);
        y[i] = std::clamp(static_cast<std::int32_t>(std::floor(sy * kSubpixelScale + 0.5f)), 0, ctx.maxY);
        z[i] = std::clamp(ndcZ * 0.5f + 0.5f, 0.0f, 1.0f);
    }

    // Back-face culling. The sign of the screen-space area tells which way
    // the triangle winds on screen. Front faces are counter-clockwise in the
    // model, but the viewport's y flip mirrors the image, so on screen they
    // come out clockwise - which is a negative area here.
    const std::int64_t area = edgeFunction(x[0], y[0], x[1], y[1], x[2], y[2]);
    if (area == 0) return;  // edge-on or smaller than the subpixel grid
    const bool backFacing = area > 0;
    if (backFacing && ctx.cullBackfaces && !info.twoSided) return;

    // The rasterizer wants positive area: swap two vertices if necessary.
    int order[3] = {0, 1, 2};
    if (area < 0) {
        std::swap(order[1], order[2]);
        // Edges are named after the opposite vertex, so edges 1 and 2 swap too.
        edgeMask = static_cast<std::uint8_t>((edgeMask & 1) | ((edgeMask & 2) << 1) | ((edgeMask & 4) >> 1));
    }

    RasterTriangle& tri = out.emplace_back();
    for (int i = 0; i < 3; ++i) {
        const int s = order[i];
        tri.x[i] = x[s];
        tri.y[i] = y[s];
        tri.z[i] = z[s];
        tri.invW[i] = invW[s];
        std::copy_n(verts[s]->varyings, ctx.varyingCount, tri.varyings[i]);
    }
    tri.material = info.material;
    tri.tangentSign = info.tangentSign;
    tri.backFacing = backFacing;
    tri.edgeMask = edgeMask;

    // Mip level. Compare the triangle's area in texture space with its area
    // on screen: their ratio is how much texture one pixel covers. One level
    // of detail per triangle is an approximation (it ignores how perspective
    // varies the ratio across the triangle) but needs no per-pixel
    // derivatives.
    tri.lod = 0.0f;
    if (info.needsLod) {
        const float* uv0 = tri.varyings[0];
        const float* uv1 = tri.varyings[1];
        const float* uv2 = tri.varyings[2];
        const float uvArea = std::abs((uv1[0] - uv0[0]) * (uv2[1] - uv0[1]) -
                                      (uv1[1] - uv0[1]) * (uv2[0] - uv0[0]));
        const float pixelArea =
            static_cast<float>(area < 0 ? -area : area) / (kSubpixelScale * kSubpixelScale);
        tri.lod = uvArea > 0.0f ? 0.5f * std::log2(uvArea / pixelArea) : 0.0f;
    }
}

}  // namespace

RenderStats Renderer::render(const Model& model, const Camera& camera, const RenderSettings& settings,
                             Framebuffer& fb) {
    RenderStats stats;
    const int width = fb.width(), height = fb.height();
    if (width <= 0 || height <= 0) return stats;

    const bool phong = settings.shading == ShadingMode::Phong;
    const bool flat = settings.shading == ShadingMode::Flat;
    const Mat4 viewProjection = camera.projection * camera.view;

    // The light rig is defined relative to the camera; bring it into world
    // space, where lighting is computed.
    Lights lights;
    const Mat4 cameraToWorld = inverse(camera.view);
    lights.keyDirection = normalize(transformDirection(cameraToWorld, settings.lightDirection));
    lights.fillDirection = normalize(transformDirection(cameraToWorld, {0.8f, -0.1f, 0.4f}));

    SetupContext ctx;
    ctx.width = static_cast<float>(width);
    ctx.height = static_cast<float>(height);
    ctx.maxX = width * kSubpixelScale;
    ctx.maxY = height * kSubpixelScale;
    ctx.cullBackfaces = settings.cullBackfaces;
    ctx.varyingCount = phong ? kPhongVaryingCount : kLitVaryingCount;

    std::size_t chunkCount = 0;

    for (const Mesh& mesh : model.meshes) {
        const std::size_t triangleCount = mesh.indices.size() / 3;
        if (triangleCount == 0) continue;
        stats.trianglesSubmitted += triangleCount;

        const Material& material = model.materials[mesh.material];
        const bool twoSided = material.doubleSided || !settings.cullBackfaces;

        // ------------------------------------------------------ vertex stage
        // Every vertex is transformed exactly once, however many triangles
        // share it.
        auto t0 = std::chrono::steady_clock::now();
        clipVertices_.resize(mesh.vertices.size());
        const int vertexChunks =
            static_cast<int>((mesh.vertices.size() + kVerticesPerChunk - 1) / kVerticesPerChunk);
        pool_.parallelFor(vertexChunks, [&](int chunk) {
            const std::size_t begin = static_cast<std::size_t>(chunk) * kVerticesPerChunk;
            const std::size_t end = std::min(begin + kVerticesPerChunk, mesh.vertices.size());
            for (std::size_t i = begin; i < end; ++i) {
                const Vertex& in = mesh.vertices[i];
                ClipVertex& out = clipVertices_[i];
                out.position = viewProjection * Vec4(in.position, 1.0f);
                out.varyings[kUV] = in.uv.x;
                out.varyings[kUV + 1] = in.uv.y;
                if (phong) {
                    store(out.varyings + kNormal, in.normal);
                    store(out.varyings + kWorld, in.position);
                    store(out.varyings + kTangent, in.tangent.xyz());
                } else if (!flat) {
                    // Gouraud: light the vertex now; the rasterizer will
                    // interpolate the result.
                    const Vec3 toEye = normalize(camera.position - in.position);
                    Vec3 n = in.normal;
                    if (twoSided && dot(n, toEye) < 0.0f) n = -n;
                    const Lighting l = computeLighting(lights, n, toEye, material.roughness);
                    store(out.varyings + kDiffuse, l.diffuse);
                    store(out.varyings + kSpecular, l.specular);
                }
            }
        });
        stats.vertexMs += msSince(t0);

        // --------------------------------------------------- primitive setup
        t0 = std::chrono::steady_clock::now();
        const int meshChunks = static_cast<int>((triangleCount + kTrianglesPerChunk - 1) / kTrianglesPerChunk);
        if (chunks_.size() < chunkCount + meshChunks) chunks_.resize(chunkCount + meshChunks);

        TriangleInfo meshInfo{};
        meshInfo.material = static_cast<std::uint16_t>(mesh.material);
        meshInfo.twoSided = twoSided;
        meshInfo.needsLod = mesh.hasUVs &&
                        (material.baseColorTexture >= 0 || material.normalTexture >= 0 ||
                         material.emissiveTexture >= 0 || material.metallicRoughnessTexture >= 0);

        pool_.parallelFor(meshChunks, [&](int chunk) {
            TriangleInfo info = meshInfo;  // per-thread copy; tangentSign varies per triangle
            std::vector<RasterTriangle>& out = chunks_[chunkCount + chunk];
            out.clear();
            const std::size_t begin = static_cast<std::size_t>(chunk) * kTrianglesPerChunk;
            const std::size_t end = std::min(begin + kTrianglesPerChunk, triangleCount);

            for (std::size_t tri = begin; tri < end; ++tri) {
                const std::uint32_t i0 = mesh.indices[tri * 3];
                const std::uint32_t i1 = mesh.indices[tri * 3 + 1];
                const std::uint32_t i2 = mesh.indices[tri * 3 + 2];

                // Trivial rejection: all three vertices outside one plane.
                const unsigned c0 = clipOutcode(clipVertices_[i0].position);
                const unsigned c1 = clipOutcode(clipVertices_[i1].position);
                const unsigned c2 = clipOutcode(clipVertices_[i2].position);
                if (c0 & c1 & c2) continue;

                info.tangentSign = mesh.vertices[i0].tangent.w;

                ClipVertex polygon[kMaxClipVertices];
                const ClipVertex* verts[3] = {&clipVertices_[i0], &clipVertices_[i1], &clipVertices_[i2]};

                if (flat) {
                    // Flat shading: one normal - the true geometric normal of
                    // the face - and one lighting evaluation at its centre,
                    // copied to all three corners.
                    const Vec3 p0 = mesh.vertices[i0].position;
                    const Vec3 p1 = mesh.vertices[i1].position;
                    const Vec3 p2 = mesh.vertices[i2].position;
                    Vec3 n = normalize(cross(p1 - p0, p2 - p0), mesh.vertices[i0].normal);
                    const Vec3 toEye = normalize(camera.position - (p0 + p1 + p2) * (1.0f / 3.0f));
                    if (twoSided && dot(n, toEye) < 0.0f) n = -n;
                    const Lighting l = computeLighting(lights, n, toEye, material.roughness);
                    for (int k = 0; k < 3; ++k) {
                        polygon[k] = *verts[k];
                        store(polygon[k].varyings + kDiffuse, l.diffuse);
                        store(polygon[k].varyings + kSpecular, l.specular);
                        verts[k] = &polygon[k];
                    }
                }

                const unsigned crossed = c0 | c1 | c2;
                if (crossed == 0) {
                    // Entirely inside the frustum - the common case.
                    emitTriangle(verts, 0b111, info, ctx, out);
                    continue;
                }

                // Straddles the frustum: clip, then triangulate the resulting
                // convex polygon as a fan around its first vertex.
                if (!flat)
                    for (int k = 0; k < 3; ++k) polygon[k] = *verts[k];
                const int n = clipPolygon(polygon, 3, crossed, ctx.varyingCount);
                for (int k = 1; k + 1 < n; ++k) {
                    const ClipVertex* fan[3] = {&polygon[0], &polygon[k], &polygon[k + 1]};
                    // Only the polygon's outline counts as an edge for
                    // wireframe; the fan's internal diagonals do not.
                    std::uint8_t edges = 0b001;
                    if (k + 1 == n - 1) edges |= 0b010;
                    if (k == 1) edges |= 0b100;
                    emitTriangle(fan, edges, info, ctx, out);
                }
            }
        });
        chunkCount += meshChunks;
        stats.setupMs += msSince(t0);
    }

    // ------------------------------------------------------------------ binning
    // Record, for every tile, the triangles whose bounding box touches it.
    // Walking the chunks in order keeps the result deterministic regardless
    // of which thread produced which chunk.
    auto t0 = std::chrono::steady_clock::now();
    const int tilesX = (width + kTileSize - 1) / kTileSize;
    const int tilesY = (height + kTileSize - 1) / kTileSize;
    bins_.resize(static_cast<std::size_t>(tilesX) * tilesY);
    for (auto& bin : bins_) bin.clear();

    const int tileShift = kSubpixelBits + 5;  // fixed point -> tile index (32 = 2^5)
    static_assert(kTileSize == 32, "tileShift assumes 32-pixel tiles");
    for (std::size_t c = 0; c < chunkCount; ++c) {
        stats.trianglesDrawn += chunks_[c].size();
        for (const RasterTriangle& tri : chunks_[c]) {
            const int tx0 = std::min({tri.x[0], tri.x[1], tri.x[2]}) >> tileShift;
            const int ty0 = std::min({tri.y[0], tri.y[1], tri.y[2]}) >> tileShift;
            const int tx1 = std::min(std::max({tri.x[0], tri.x[1], tri.x[2]}) >> tileShift, tilesX - 1);
            const int ty1 = std::min(std::max({tri.y[0], tri.y[1], tri.y[2]}) >> tileShift, tilesY - 1);
            for (int ty = ty0; ty <= ty1; ++ty)
                for (int tx = tx0; tx <= tx1; ++tx)
                    bins_[static_cast<std::size_t>(ty) * tilesX + tx].push_back(&tri);
        }
    }
    stats.setupMs += msSince(t0);

    // ------------------------------------------------------------ rasterization
    t0 = std::chrono::steady_clock::now();
    FragmentShader shader;
    shader.model = &model;
    shader.lights = lights;
    shader.eye = camera.position;
    shader.background = settings.background;
    shader.background.a = 0;  // alpha 0 marks "no geometry here"
    shader.lineWidth = std::max(static_cast<int>(std::lround(settings.wireWidth)), 1);
    shader.perPixelLighting = phong;
    shader.wireframe = settings.wireframe;
    shader.textures = settings.textures;
    shader.normalMaps = settings.normalMaps;

    pool_.parallelFor(static_cast<int>(bins_.size()), [&](int index) {
        TileRect tile;
        tile.x0 = (index % tilesX) * kTileSize;
        tile.y0 = (index / tilesX) * kTileSize;
        tile.x1 = std::min(tile.x0 + kTileSize, width);
        tile.y1 = std::min(tile.y0 + kTileSize, height);

        // Each tile clears its own pixels just before drawing into them,
        // while they are about to be in this thread's cache anyway.
        for (int y = tile.y0; y < tile.y1; ++y) {
            std::fill(fb.colorRow(y) + tile.x0, fb.colorRow(y) + tile.x1, shader.background);
            std::fill(fb.depthRow(y) + tile.x0, fb.depthRow(y) + tile.x1, 1.0f);
        }

        const auto& bin = bins_[index];
        rasterizeTile(fb, tile, bin.data(), bin.size(), shader);
        // Lines go on after every triangle in the tile has written its depth.
        if (shader.wireframe) rasterizeTileEdges(fb, tile, bin.data(), bin.size(), shader, shader.lineWidth);
    });
    stats.rasterMs = msSince(t0);

    return stats;
}

}  // namespace termesh
