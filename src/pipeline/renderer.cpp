#include "pipeline/renderer.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>

#include "core/color.hpp"
#include "pipeline/shading.hpp"

namespace termesh {

const char* shadingModeName(ShadingMode mode) {
    switch (mode) {
        case ShadingMode::Flat: return "flat";
        case ShadingMode::Gouraud: return "gouraud";
        case ShadingMode::Pbr: return "pbr";
        case ShadingMode::Toon: return "toon";
        default: return "phong";
    }
}

// What one pass over the scene's geometry needs to know. The main view and
// each shadow map are separate passes over the same models.
struct Renderer::GeometryPass {
    Mat4 viewProjection;
    Vec3 eye;
    int width = 0, height = 0;
    VaryingLayout layout = VaryingLayout::PerPixel;
    bool flat = false;  // Lit only: light once per face rather than per vertex
    int highlightItem = -1, highlightMesh = -1;
    const std::uint8_t* lodChoice = nullptr;  // per mesh of the draw list, or null for "all original"
    int varyingCount = 0;                     // how many varyings each vertex carries
    bool normalMaps = true;                   // whether the fragment shader will want tangents
    bool textures = true;                     // whether textures are in use at all
    bool points = false;                      // draw vertices as splats instead of triangles
    float pointScale = 1.0f;
    bool thinPoints = false;                       // allowed to draw only some of a distant cloud's points
    float projectionX = 1.0f, projectionY = 1.0f;  // the projection's x and y scale: view units -> clip units
    bool cullBackfaces = true;
    // Draw translucent materials last, back to front, without depth writes.
    bool sortTranslucent = false;
    const LightingContext* lighting = nullptr;  // Lit only
};

namespace {

// ---------------------------------------------------------------- varyings ---
//
// Layout of the per-vertex varyings array. Texture coordinates always come
// first. What follows depends on where lighting is evaluated:
//
//   Flat / Gouraud  lighting is already computed, so the varyings carry its
//                   result: diffuse light RGB and specular light RGB.
//   per pixel       (Phong, PBR, toon) lighting happens in the fragment
//                   shader, so the varyings carry its inputs: normal, world
//                   position and tangent.
constexpr int kUV = 0;
constexpr int kDiffuse = 2, kSpecular = 5;            // Flat / Gouraud
constexpr int kNormal = 2, kWorld = 5, kTangent = 8;  // per pixel
// A per-vertex colour, for meshes that have one. Per pixel it takes the
// tangent's place: a mesh uses the tangent only for normal mapping, and
// where both are wanted the normal map wins (see buildGeometry). In the
// vertex-lit layout it goes on the end, and those frames carry three more
// varyings - but only frames that actually contain coloured vertices.
constexpr int kPixelColor = kTangent, kLitColor = 8;
constexpr int kLitColorVaryingCount = 11;
constexpr int kPointsPerChunk = 256;
constexpr int kDepthVaryingCount = 2;
constexpr int kLitVaryingCount = 8;
constexpr int kPerPixelVaryingCount = 11;

// Below this a mesh is not worth a BVH traversal: the per-triangle clip test
// that would reject its hidden triangles anyway is cheaper.
constexpr std::size_t kMinTrianglesForBvhCulling = 1024;

constexpr int kTrianglesPerChunk = 512;
constexpr int kVerticesPerChunk = 2048;

int varyingCountFor(Renderer::VaryingLayout layout) {
    switch (layout) {
        case Renderer::VaryingLayout::DepthOnly: return kDepthVaryingCount;
        case Renderer::VaryingLayout::Lit: return kLitVaryingCount;
        default: return kPerPixelVaryingCount;
    }
}

void store(float* dst, Vec3 v) {
    dst[0] = v.x;
    dst[1] = v.y;
    dst[2] = v.z;
}
Vec3 load3(const float* src) { return {src[0], src[1], src[2]}; }

double msSince(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}

const Environment& defaultEnvironment() {
    static const Environment environment;
    return environment;
}

// Alpha of the base colour at a pixel, and whether that is enough for the
// surface to count as solid (for depth-only purposes: wireframe fill and
// shadow casting).
bool isSolid(const Renderer::MaterialRef& ref, const RasterTriangle& tri, const Fragment& frag) {
    const Material& material = *ref.material;
    // A splat is a square; only the disc inside it is solid.
    if (tri.splat && frag.varyings[kUV] * frag.varyings[kUV] + frag.varyings[kUV + 1] * frag.varyings[kUV + 1] > 1.0f)
        return false;
    if (material.alphaMode == AlphaMode::Opaque) return true;
    float alpha = material.baseColor.w;
    if (material.baseColorTexture >= 0 && !tri.splat)
        alpha *=
            ref.textures[material.baseColorTexture].sample({frag.varyings[kUV], frag.varyings[kUV + 1]}, tri.lod).w;
    // Translucent surfaces count as solid once they are mostly opaque.
    return alpha >= (material.alphaMode == AlphaMode::Mask ? material.alphaCutoff : 0.5f);
}

// ---------------------------------------------------------- fragment stage ---
//
// Shading produces a linear HDR colour. Where it goes next depends on the
// frame (see Renderer::render), so the shader is written once against an
// "output" and compiled for each of the two:

// Keep the colour as it is, in the float buffer, for post-processing.
struct HdrOutput {
    using Pixel = Vec4;

    // (These run once per pixel: see the note on inlining in shading.hpp.)
    TERMESH_FORCE_INLINE void opaque(Vec4& out, Vec3 color) const { out = Vec4(color, 1.0f); }
    // The "over" operator: this surface covers `alpha` of the pixel and
    // lets the rest of what was already there show through. Coverage
    // accumulates the same way, so that the background can be mixed in
    // afterwards by whatever is left.
    TERMESH_FORCE_INLINE void translucent(Vec4& out, Vec3 color, float alpha) const {
        out = Vec4(color * alpha + out.xyz() * (1.0f - alpha), alpha + out.w * (1.0f - alpha));
    }
    // Looks empty, but is not: see the wireframe fill.
    void nothing(Vec4& out) const { out = Vec4(0.0f); }
};

// Finish the pixel on the spot: tone map and encode straight into the 8-bit
// framebuffer. Only used for frames with nothing translucent in them,
// because blending has to happen in linear light, before tone mapping.
struct DirectOutput {
    using Pixel = Color8;
    float exposure;
    ToneMapper mapper;
    Color8 empty;  // the background colour with alpha 0: "no geometry here"

    TERMESH_FORCE_INLINE void opaque(Color8& out, Vec3 color) const {
        out = linearToSrgb(toneMap(color * exposure, mapper));
    }
    TERMESH_FORCE_INLINE void translucent(Color8& out, Vec3 color, float) const { opaque(out, color); }
    void nothing(Color8& out) const { out = empty; }
};

// `Extras` switches on the parts most frames never need: point splats,
// per-vertex colours, translucency and the selection highlight. Each is only
// a test or two per pixel, but the shader runs for every pixel of every
// frame and together they were a measurable share of the default path. A
// frame that uses none of them gets the version compiled with Extras =
// false, in which those tests do not exist.
template <typename Output, bool Extras>
struct FragmentShader {
    using Pixel = typename Output::Pixel;
    // For the rasterizer: whether this shader ever blends with what is
    // already in the framebuffer (and so needs to be handed it).
    static constexpr bool kMayBlend = Extras;

    Output output;
    const Renderer::MaterialRef* materials;
    LightingContext lighting;
    Vec3 eye;
    ShadingMode mode;
    int lineWidth;
    int varyings;  // how many varyings the triangles carry
    bool perPixelLighting, wireframe, textures, normalMaps;

    int varyingCount() const { return varyings; }

    // In wireframe mode the triangles themselves only lay down depth (pushed
    // back a little, see rasterizeTile) and the edges are drawn over them.
    float depthSlopeBias() const { return wireframe ? static_cast<float>(lineWidth) * 0.5f + 1.0f : 0.0f; }

    // Triangle interiors.
    bool operator()(const RasterTriangle& tri, const Fragment& frag, Pixel& out) const {
        // (Splats have no edges to draw: in wireframe they stay as they are.)
        if (!wireframe || (Extras && tri.splat)) return shade(tri, frag, out);
        // Leave the pixel looking empty, but still occupy the depth buffer so
        // that lines behind this surface stay hidden. Cut-outs remain
        // see-through.
        if (!isSolid(materials[tri.material], tri, frag)) return false;
        output.nothing(out);
        return true;
    }

    // Wireframe lines: shaded like the surface they belong to, then
    // lightened so they read against a dark background.
    bool shadeEdge(const RasterTriangle& tri, const Fragment& frag, Pixel& out) const { return shade(tri, frag, out); }

    TERMESH_NO_SLP bool shade(const RasterTriangle& tri, const Fragment& frag, Pixel& out) const {
        const Renderer::MaterialRef& ref = materials[tri.material];
        const Material& material = *ref.material;
        const float* v = frag.varyings;
        const Vec2 uv{v[kUV], v[kUV + 1]};
        // A splat's "texture coordinates" are its own: -1 to 1 across the
        // square. Keep the disc, and sample no textures with them.
        const bool splat = Extras && tri.splat;
        if (splat && uv.x * uv.x + uv.y * uv.y > 1.0f) return false;
        const bool useTextures = textures && !splat;

        // Base colour, and alpha cut-outs (leaves, fences, decals).
        Vec4 base = material.baseColor;
        const bool vertexColor = Extras && tri.vertexColor;
        if (vertexColor) base = Vec4(base.xyz() * load3(v + (perPixelLighting ? kPixelColor : kLitColor)), base.w);
        if (material.baseColorTexture >= 0 && !splat && (useTextures || material.alphaMode != AlphaMode::Opaque)) {
            const Vec4 texel = ref.textures[material.baseColorTexture].sample(uv, tri.lod);
            if (useTextures) base = base * texel;
            else base.w *= texel.w;
        }
        if (material.alphaMode == AlphaMode::Mask && base.w < material.alphaCutoff) return false;
        const bool translucent = Extras && material.alphaMode == AlphaMode::Blend && tri.blend;
        if (translucent && base.w <= 0.004f) return false;  // invisible: do not even touch the pixel

        float metallic = material.metallic, roughness = material.roughness, occlusion = 1.0f;
        if (useTextures && material.metallicRoughnessTexture >= 0) {
            const Vec4 mr = ref.textures[material.metallicRoughnessTexture].sample(uv, tri.lod);
            roughness *= mr.y;
            metallic *= mr.z;
            if (material.occlusionInRed) occlusion = mr.x;
        }

        Vec3 color;
        if (perPixelLighting) {
            // Interpolating unit normals shortens them, so renormalise.
            Vec3 n = normalize(load3(v + kNormal));
            if (tri.backFacing) n = -n;

            if (normalMaps && material.normalTexture >= 0 && !vertexColor && !splat) {
                // A normal map stores directions relative to the surface: x
                // along the tangent (texture u), y along the bitangent, z
                // along the surface normal. Rebuild that frame at this pixel
                // and use it to rotate the sampled normal into world space.
                Vec3 t = load3(v + kTangent);
                t = t - n * dot(n, t);  // re-orthogonalise against n
                if (lengthSquared(t) > 1e-12f) {
                    t = normalize(t);
                    const Vec3 b = cross(n, t) * tri.tangentSign;
                    const Vec4 s = ref.textures[material.normalTexture].sample(uv, tri.lod);
                    const float nx = (s.x * 2.0f - 1.0f) * material.normalScale;
                    const float ny = (s.y * 2.0f - 1.0f) * material.normalScale;
                    const float nz = s.z * 2.0f - 1.0f;
                    n = normalize(t * nx + b * ny + n * nz);
                }
            }

            const Vec3 position = load3(v + kWorld);
            const Vec3 view = normalize(eye - position);
            if (mode == ShadingMode::Phong) {
                color = combineBlinnPhong(blinnPhong(lighting, position, n, view, roughness, true), base.xyz(),
                                          metallic, occlusion);
            } else {
                // The other models take the surface as one bundle. It is
                // only put together here, off the default path: a struct
                // handed to a function has to exist in memory, which the
                // Phong path above avoids.
                Surface surface;
                surface.position = position;
                surface.normal = n;
                surface.view = view;
                surface.albedo = base.xyz();
                surface.metallic = metallic;
                surface.roughness = roughness;
                surface.occlusion = occlusion;
                color = mode == ShadingMode::Pbr ? shadePbr(lighting, surface) : shadeToon(lighting, surface);
            }
        } else {
            Lighting light;
            light.diffuse = load3(v + kDiffuse);
            light.specular = load3(v + kSpecular);
            color = combineBlinnPhong(light, base.xyz(), metallic, occlusion);
        }

        // An emissive texture says *where* a material glows; its factor only
        // scales that. Without the texture (textures switched off, or a
        // splat with no surface to map it to) the factor alone would make
        // the whole thing glow, so such a material then does not glow at all.
        if (material.emissiveTexture < 0) color += material.emissive;
        else if (useTextures)
            color += material.emissive * ref.textures[material.emissiveTexture].sample(uv, tri.lod).xyz();

        if (wireframe) color = lerp(color, Vec3(1.0f), 0.3f);
        // The selected mesh: mixed towards a warm highlight colour.
        if (Extras && tri.highlight) color = lerp(color, Vec3{1.0f, 0.5f, 0.08f}, 0.4f) + Vec3{0.12f, 0.06f, 0.0f};

        if (translucent) output.translucent(out, color, std::min(base.w, 1.0f));
        else output.opaque(out, color);
        return true;
    }
};

// Shadow maps only need depth: no lighting, no colour.
struct DepthShader {
    const Renderer::MaterialRef* materials;

    int varyingCount() const { return kDepthVaryingCount; }
    // Push the surface away from the light in proportion to its slope, so
    // that it does not shadow itself (see ShadowMap::bias).
    float depthSlopeBias() const { return 1.5f; }
    bool operator()(const RasterTriangle& tri, const Fragment& frag, std::uint8_t&) const {
        return isSolid(materials[tri.material], tri, frag);
    }
    bool shadeEdge(const RasterTriangle&, const Fragment&, std::uint8_t&) const { return false; }  // never drawn
};

// --------------------------------------------------------- primitive setup ---

struct SetupContext {
    float width, height;      // framebuffer size
    std::int32_t maxX, maxY;  // ...in fixed point
    bool cullBackfaces;
    int varyingCount;
};

struct TriangleInfo {
    std::uint32_t material;
    float tangentSign;
    bool twoSided, needsLod, blend;
    bool mirrored;  // the model transform flips the winding
    bool highlight, vertexColor, splat;
};

// Project three clip-space vertices to the screen and append the resulting
// triangle, unless it is back-facing or degenerate.
void emitTriangle(const ClipVertex* verts[3], std::uint8_t edgeMask, const TriangleInfo& info, const SetupContext& ctx,
                  std::vector<RasterTriangle>& out) {
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
    // come out clockwise - which is a negative area here. (A model placed
    // with a mirroring transform is the other way round.)
    const std::int64_t area = edgeFunction(x[0], y[0], x[1], y[1], x[2], y[2]);
    if (area == 0) return;  // edge-on or smaller than the subpixel grid
    const bool backFacing = (area > 0) != info.mirrored;
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
    // (A splat faces the camera by construction, whichever way it winds.)
    tri.backFacing = backFacing && !info.splat;
    tri.vertexColor = info.vertexColor;
    tri.splat = info.splat;
    tri.edgeMask = edgeMask;
    tri.blend = info.blend;
    tri.highlight = info.highlight;

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
        const float uvArea = std::abs((uv1[0] - uv0[0]) * (uv2[1] - uv0[1]) - (uv1[1] - uv0[1]) * (uv2[0] - uv0[0]));
        const float pixelArea = static_cast<float>(area < 0 ? -area : area) / (kSubpixelScale * kSubpixelScale);
        tri.lod = uvArea > 0.0f ? 0.5f * std::log2(uvArea / pixelArea) : 0.0f;
    }
}

}  // namespace

void Renderer::buildGeometry(std::span<const DrawItem> items, const GeometryPass& pass, RenderStats& stats) {
    const bool perPixel = pass.layout == VaryingLayout::PerPixel;
    const bool lit = pass.layout == VaryingLayout::Lit;
    const bool flat = lit && pass.flat;

    SetupContext ctx;
    ctx.width = static_cast<float>(pass.width);
    ctx.height = static_cast<float>(pass.height);
    ctx.maxX = pass.width * kSubpixelScale;
    ctx.maxY = pass.height * kSubpixelScale;
    ctx.cullBackfaces = pass.cullBackfaces;
    ctx.varyingCount = pass.varyingCount;

    std::size_t chunkCount = 0;
    std::uint32_t materialBase = 0;
    std::size_t meshOrdinal = 0;  // counts meshes across the whole draw list
    passHasExtras_ = false;

    for (std::size_t itemIndex = 0; itemIndex < items.size(); ++itemIndex) {
        const DrawItem& item = items[itemIndex];
        const Model& model = *item.model;
        // Most models sit where they were loaded; skip the matrix maths then.
        const bool placed = !(item.transform == Mat4::identity());
        const Mat4& transform = item.transform;
        const Mat4 normalTransform = placed ? normalMatrix(transform) : Mat4::identity();
        const bool mirrored =
            placed && dot(cross(transform.c[0].xyz(), transform.c[1].xyz()), transform.c[2].xyz()) < 0.0f;
        const Mat4 modelViewProjection = placed ? pass.viewProjection * transform : pass.viewProjection;
        auto worldPosition = [&](const Vertex& v) {
            return placed ? transformPoint(transform, v.position) : v.position;
        };
        auto worldNormal = [&](const Vertex& v) {
            return placed ? normalize(transformDirection(normalTransform, v.normal)) : v.normal;
        };

        // Planes of the view volume in the model's own space, to test its
        // boxes against.
        const bool haveAccel = item.accel && item.accel->meshCount() == model.meshes.size();
        const Frustum frustum = haveAccel ? Frustum::fromMatrix(modelViewProjection) : Frustum{};

        for (std::size_t meshIndex = 0; meshIndex < model.meshes.size(); ++meshIndex, ++meshOrdinal) {
            const Mesh& mesh = model.meshes[meshIndex];
            // Points instead of triangles: always for a point cloud, and for
            // any mesh when asked.
            const bool splats = !mesh.vertices.empty() && (mesh.indices.empty() || pass.points);
            if (!splats && mesh.indices.size() < 3) continue;
            stats.trianglesSubmitted += splats ? mesh.vertices.size() * 2 : mesh.indices.size() / 3;

            // Level of detail: a coarser version of the mesh is just another
            // index list over the same vertices.
            const MeshLod* lod = nullptr;
            if (!splats && pass.lodChoice && pass.lodChoice[meshOrdinal] > 0)
                lod = &item.accel->lods(meshIndex)[pass.lodChoice[meshOrdinal] - 1u];
            const std::vector<std::uint32_t>& indices = lod ? lod->indices : mesh.indices;
            const std::size_t triangleCount = indices.size() / 3;
            if (lod) stats.trianglesSimplified += mesh.indices.size() / 3 - triangleCount;

            // ------------------------------------------------------- culling
            // A mesh whose box is entirely outside the view is skipped
            // before any of its vertices are touched. One that straddles
            // the edge of the view asks its BVH which triangles could be
            // inside, as a bitmap in the triangles' own order - so the
            // survivors are still drawn in exactly the order they always
            // were, and the picture does not depend on whether culling ran.
            bool useVisibility = false;
            if (haveAccel) {
                const Frustum::Side side = frustum.classify(item.accel->bounds(meshIndex));
                if (side == Frustum::Side::Outside) {
                    ++stats.meshesCulled;
                    stats.trianglesCulled += triangleCount;
                    continue;
                }
                // (The BVH indexes the original triangles, so it cannot
                // help with a simplified version - which is small anyway.)
                const Bvh* bvh = item.accel->bvh(meshIndex);
                if (side == Frustum::Side::Intersecting && bvh && !lod && !splats &&
                    triangleCount >= kMinTrianglesForBvhCulling) {
                    visible_.assign((triangleCount + 63) / 64, 0);
                    bvh->markVisible(frustum, visible_);
                    std::size_t kept = 0;
                    for (const std::uint64_t word : visible_) kept += static_cast<std::size_t>(std::popcount(word));
                    stats.trianglesCulled += triangleCount - kept;
                    useVisibility = true;
                }
            }

            const Material& material = model.materials[mesh.material];
            const bool twoSided = material.doubleSided || !pass.cullBackfaces;
            // Per-vertex colours ride in the tangent's slot, so a mesh that
            // is being normal-mapped (which needs the tangent) goes without.
            const bool hasColors = mesh.colors.size() == mesh.vertices.size();
            // A textured mesh drawn as points has no surface to map the
            // texture onto; instead each point takes the texture's colour
            // at its own coordinates, which then travels as a vertex colour.
            const Texture* pointTexture =
                splats && !hasColors && pass.textures && mesh.hasUVs && material.baseColorTexture >= 0
                    ? &model.textures[static_cast<std::size_t>(material.baseColorTexture)]
                    : nullptr;
            const bool vertexColor =
                (hasColors || pointTexture) &&
                (lit || (perPixel && !(pass.normalMaps && material.normalTexture >= 0 && !splats)));
            auto colorOf = [&](std::size_t vertex) {
                // A blurred level of the texture: a point stands for a patch
                // of surface, not a single texel.
                return hasColors ? mesh.colors[vertex] : pointTexture->sample(mesh.vertices[vertex].uv, -3.0f).xyz();
            };

            // -------------------------------------------------- vertex stage
            // Every vertex is transformed exactly once, however many
            // triangles share it.
            auto t0 = std::chrono::steady_clock::now();
            clipVertices_.resize(mesh.vertices.size());
            // A simplified mesh uses only some of the vertices; transform
            // just those.
            const std::size_t vertexCount = lod ? lod->vertices.size() : mesh.vertices.size();
            const int vertexChunks = static_cast<int>((vertexCount + kVerticesPerChunk - 1) / kVerticesPerChunk);
            pool_.parallelFor(vertexChunks, [&](int chunk) {
                const std::size_t begin = static_cast<std::size_t>(chunk) * kVerticesPerChunk;
                const std::size_t end = std::min(begin + kVerticesPerChunk, vertexCount);
                for (std::size_t slot = begin; slot < end; ++slot) {
                    const std::size_t i = lod ? lod->vertices[slot] : slot;
                    const Vertex& in = mesh.vertices[i];
                    ClipVertex& out = clipVertices_[i];
                    out.position = modelViewProjection * Vec4(in.position, 1.0f);
                    out.varyings[kUV] = in.uv.x;
                    out.varyings[kUV + 1] = in.uv.y;
                    if (perPixel) {
                        const Vec3 position = worldPosition(in);
                        // A point with no normal of its own (a bare point
                        // cloud) is shaded as if it faced the camera.
                        const bool bare = splats && lengthSquared(in.normal) < 1e-12f;
                        store(out.varyings + kNormal, bare ? normalize(pass.eye - position) : worldNormal(in));
                        store(out.varyings + kWorld, position);
                        if (vertexColor) store(out.varyings + kPixelColor, colorOf(i));
                        else
                            store(out.varyings + kTangent,
                                  placed ? transformDirection(transform, in.tangent.xyz()) : in.tangent.xyz());
                    } else if (lit) {
                        if (vertexColor) store(out.varyings + kLitColor, colorOf(i));
                        // Flat-shaded triangles are lit per face, later.
                        if (flat && !splats) continue;
                        // Gouraud: light the vertex now; the rasterizer will
                        // interpolate the result.
                        const Vec3 position = worldPosition(in);
                        const Vec3 toEye = normalize(pass.eye - position);
                        Vec3 n = splats && lengthSquared(in.normal) < 1e-12f ? toEye : worldNormal(in);
                        if ((twoSided || splats) && dot(n, toEye) < 0.0f) n = -n;
                        const Lighting l = blinnPhong(*pass.lighting, position, n, toEye, material.roughness, false);
                        store(out.varyings + kDiffuse, l.diffuse);
                        store(out.varyings + kSpecular, l.specular);
                    }
                }
            });
            stats.vertexMs += msSince(t0);

            // ----------------------------------------------- primitive setup
            t0 = std::chrono::steady_clock::now();
            // How many points to draw, and how big. A cloud so far away that
            // its splats are smaller than a pixel is thinned: every k-th
            // point is drawn, k times the area. (Clouds are shuffled when
            // loaded, so every k-th point is an even sample.)
            std::size_t pointStride = 1;
            float pointRadius = 0.0f;
            if (splats) {
                const float itemScale = placed ? std::max({length(transform.c[0].xyz()), length(transform.c[1].xyz()),
                                                           length(transform.c[2].xyz())})
                                               : 1.0f;
                pointRadius = std::max(mesh.pointRadius, 1e-9f) * pass.pointScale * itemScale;
                if (pass.thinPoints && haveAccel && mesh.indices.empty() && mesh.vertices.size() > 20000) {
                    const Bounds& box = item.accel->bounds(meshIndex);
                    const float reach = box.radius() * itemScale;
                    const float distance =
                        std::max(length(transformPoint(transform, box.center()) - pass.eye) - reach, reach * 0.05f);
                    const float pixels =
                        pointRadius * 0.5f * static_cast<float>(pass.height) * pass.projectionY / distance;
                    if (pixels < 0.5f) {
                        pointStride = std::clamp(static_cast<std::size_t>(0.25f / (pixels * pixels)), std::size_t{1},
                                                 std::size_t{64});
                        pointRadius *= std::sqrt(static_cast<float>(pointStride));
                    }
                }
            }
            const std::size_t pointCount = splats ? (mesh.vertices.size() + pointStride - 1) / pointStride : 0;

            const int meshChunks =
                splats ? static_cast<int>((pointCount + kPointsPerChunk - 1) / kPointsPerChunk)
                       : static_cast<int>((triangleCount + kTrianglesPerChunk - 1) / kTrianglesPerChunk);
            if (chunks_.size() < chunkCount + meshChunks) chunks_.resize(chunkCount + meshChunks);
            if (chunkBlends_.size() < chunks_.size()) chunkBlends_.resize(chunks_.size());

            TriangleInfo meshInfo{};
            meshInfo.material = materialBase + static_cast<std::uint32_t>(mesh.material);
            meshInfo.twoSided = twoSided;
            meshInfo.mirrored = mirrored;
            meshInfo.blend = pass.sortTranslucent && material.alphaMode == AlphaMode::Blend;
            // A negative mesh index selects every mesh of the item.
            meshInfo.highlight = static_cast<int>(itemIndex) == pass.highlightItem &&
                                 (pass.highlightMesh < 0 || static_cast<int>(meshIndex) == pass.highlightMesh);
            meshInfo.vertexColor = vertexColor;
            meshInfo.splat = splats;
            passHasExtras_ = passHasExtras_ || vertexColor || splats;
            meshInfo.needsLod = !splats && mesh.hasUVs &&
                                (material.baseColorTexture >= 0 || material.normalTexture >= 0 ||
                                 material.emissiveTexture >= 0 || material.metallicRoughnessTexture >= 0);
            if (splats) meshInfo.twoSided = true;
            for (int chunk = 0; chunk < meshChunks; ++chunk) chunkBlends_[chunkCount + chunk] = meshInfo.blend;

            // Point splats.
            //
            // Each point becomes a small square facing the camera, built
            // directly in clip space: the same offset is added to x and y
            // before the perspective divide, so nearer points come out
            // larger, as they should. Its corners carry (-1,-1) .. (1,1)
            // where texture coordinates would be, and the fragment shader
            // keeps only the disc inside the unit circle. Two triangles
            // then go down the ordinary path - clipping, binning, depth
            // test, shading - like any others.
            if (splats)
                pool_.parallelFor(meshChunks, [&](int chunk) {
                    TriangleInfo info = meshInfo;
                    info.tangentSign = 1.0f;
                    std::vector<RasterTriangle>& out = chunks_[chunkCount + chunk];
                    out.clear();
                    const std::size_t begin = static_cast<std::size_t>(chunk) * kPointsPerChunk;
                    const std::size_t end = std::min(begin + kPointsPerChunk, pointCount);
                    for (std::size_t n = begin; n < end; ++n) {
                        const ClipVertex& centre = clipVertices_[n * pointStride];
                        const float w = centre.position.w;
                        if (w <= 0.0f) continue;  // behind the camera
                        // Never less than three quarters of a pixel in radius:
                        // a disc that size always contains a pixel centre,
                        // so distant points cannot flicker in and out.
                        const float dx = std::max(pointRadius * pass.projectionX, 1.5f * w / ctx.width);
                        const float dy = std::max(pointRadius * pass.projectionY, 1.5f * w / ctx.height);

                        static constexpr float cornerX[4] = {-1.0f, 1.0f, 1.0f, -1.0f};
                        static constexpr float cornerY[4] = {-1.0f, -1.0f, 1.0f, 1.0f};
                        ClipVertex quad[4];
                        unsigned codes[4];
                        for (int k = 0; k < 4; ++k) {
                            quad[k] = centre;
                            quad[k].position.x += cornerX[k] * dx;
                            quad[k].position.y += cornerY[k] * dy;
                            quad[k].varyings[kUV] = cornerX[k];
                            quad[k].varyings[kUV + 1] = cornerY[k];
                            codes[k] = clipOutcode(quad[k].position);
                        }
                        if (codes[0] & codes[1] & codes[2] & codes[3]) continue;

                        for (const auto& corners : {std::array<int, 3>{0, 1, 2}, std::array<int, 3>{0, 2, 3}}) {
                            const unsigned crossed = codes[corners[0]] | codes[corners[1]] | codes[corners[2]];
                            if (crossed == 0) {
                                const ClipVertex* verts[3] = {&quad[corners[0]], &quad[corners[1]], &quad[corners[2]]};
                                emitTriangle(verts, 0, info, ctx, out);
                                continue;
                            }
                            ClipVertex polygon[kMaxClipVertices] = {quad[corners[0]], quad[corners[1]],
                                                                    quad[corners[2]]};
                            const int count = clipPolygon(polygon, 3, crossed, ctx.varyingCount);
                            for (int k = 1; k + 1 < count; ++k) {
                                const ClipVertex* fan[3] = {&polygon[0], &polygon[k], &polygon[k + 1]};
                                emitTriangle(fan, 0, info, ctx, out);
                            }
                        }
                    }
                });
            else
                pool_.parallelFor(meshChunks, [&](int chunk) {
                    TriangleInfo info = meshInfo;  // per-thread copy; tangentSign varies per triangle
                    std::vector<RasterTriangle>& out = chunks_[chunkCount + chunk];
                    out.clear();
                    const std::size_t begin = static_cast<std::size_t>(chunk) * kTrianglesPerChunk;
                    const std::size_t end = std::min(begin + kTrianglesPerChunk, triangleCount);

                    for (std::size_t tri = begin; tri < end; ++tri) {
                        if (useVisibility && !((visible_[tri >> 6] >> (tri & 63)) & 1)) continue;
                        const std::uint32_t i0 = indices[tri * 3];
                        const std::uint32_t i1 = indices[tri * 3 + 1];
                        const std::uint32_t i2 = indices[tri * 3 + 2];

                        // Trivial rejection: all three vertices outside one plane.
                        const unsigned c0 = clipOutcode(clipVertices_[i0].position);
                        const unsigned c1 = clipOutcode(clipVertices_[i1].position);
                        const unsigned c2 = clipOutcode(clipVertices_[i2].position);
                        if (c0 & c1 & c2) continue;

                        info.tangentSign = mesh.vertices[i0].tangent.w;

                        ClipVertex polygon[kMaxClipVertices];
                        const ClipVertex* verts[3] = {&clipVertices_[i0], &clipVertices_[i1], &clipVertices_[i2]};

                        if (flat) {
                            // Flat shading: one normal - the true geometric
                            // normal of the face - and one lighting evaluation
                            // at its centre, copied to all three corners.
                            const Vec3 p0 = worldPosition(mesh.vertices[i0]);
                            const Vec3 p1 = worldPosition(mesh.vertices[i1]);
                            const Vec3 p2 = worldPosition(mesh.vertices[i2]);
                            Vec3 n = normalize(cross(p1 - p0, p2 - p0), worldNormal(mesh.vertices[i0]));
                            if (mirrored) n = -n;
                            const Vec3 centre = (p0 + p1 + p2) * (1.0f / 3.0f);
                            const Vec3 toEye = normalize(pass.eye - centre);
                            if (twoSided && dot(n, toEye) < 0.0f) n = -n;
                            const Lighting l = blinnPhong(*pass.lighting, centre, n, toEye, material.roughness, false);
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

                        // Straddles the frustum: clip, then triangulate the
                        // resulting convex polygon as a fan around its first
                        // vertex.
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
        materialBase += static_cast<std::uint32_t>(model.materials.size());
    }

    // ------------------------------------------------------------------ binning
    // Record, for every tile, the triangles whose bounding box touches it.
    // Walking the chunks in order keeps the result deterministic regardless
    // of which thread produced which chunk.
    const auto t0 = std::chrono::steady_clock::now();
    binsX_ = (pass.width + kTileSize - 1) / kTileSize;
    binsY_ = (pass.height + kTileSize - 1) / kTileSize;
    bins_.resize(static_cast<std::size_t>(binsX_) * binsY_);
    for (auto& bin : bins_) bin.clear();

    const int tileShift = kSubpixelBits + 5;  // fixed point -> tile index (32 = 2^5)
    static_assert(kTileSize == 32, "tileShift assumes 32-pixel tiles");
    auto binTriangle = [&](const RasterTriangle& tri) {
        const int tx0 = std::min({tri.x[0], tri.x[1], tri.x[2]}) >> tileShift;
        const int ty0 = std::min({tri.y[0], tri.y[1], tri.y[2]}) >> tileShift;
        const int tx1 = std::min(std::max({tri.x[0], tri.x[1], tri.x[2]}) >> tileShift, binsX_ - 1);
        const int ty1 = std::min(std::max({tri.y[0], tri.y[1], tri.y[2]}) >> tileShift, binsY_ - 1);
        for (int ty = ty0; ty <= ty1; ++ty)
            for (int tx = tx0; tx <= tx1; ++tx) bins_[static_cast<std::size_t>(ty) * binsX_ + tx].push_back(&tri);
    };

    // Opaque triangles first, in submission order: the depth buffer sorts
    // them out. Translucent ones cannot rely on it - each must be blended
    // over whatever is behind it - so they go last, furthest first (the
    // "painter's algorithm").
    blendOrder_.clear();
    for (std::size_t c = 0; c < chunkCount; ++c) {
        stats.trianglesDrawn += chunks_[c].size();
        if (chunkBlends_[c]) {
            for (const RasterTriangle& tri : chunks_[c]) blendOrder_.push_back(&tri);
        } else {
            for (const RasterTriangle& tri : chunks_[c]) binTriangle(tri);
        }
    }
    if (!blendOrder_.empty()) {
        std::stable_sort(blendOrder_.begin(), blendOrder_.end(), [](const RasterTriangle* a, const RasterTriangle* b) {
            return a->z[0] + a->z[1] + a->z[2] > b->z[0] + b->z[1] + b->z[2];
        });
        for (const RasterTriangle* tri : blendOrder_) binTriangle(*tri);
    }
    stats.setupMs += msSince(t0);
}

template <typename Target, typename Shader, typename FinishTile>
void Renderer::rasterize(Target& target, const Shader& shader, typename Target::Pixel clearColor, bool edges,
                         int lineWidth, const FinishTile& finishTile) {
    const int width = target.width(), height = target.height();
    pool_.parallelFor(static_cast<int>(bins_.size()), [&](int index) {
        TileRect tile;
        tile.x0 = (index % binsX_) * kTileSize;
        tile.y0 = (index / binsX_) * kTileSize;
        tile.x1 = std::min(tile.x0 + kTileSize, width);
        tile.y1 = std::min(tile.y0 + kTileSize, height);

        // Each tile clears its own pixels just before drawing into them,
        // while they are about to be in this thread's cache anyway.
        for (int y = tile.y0; y < tile.y1; ++y) {
            std::fill(target.colorRow(y) + tile.x0, target.colorRow(y) + tile.x1, clearColor);
            std::fill(target.depthRow(y) + tile.x0, target.depthRow(y) + tile.x1, 1.0f);
        }

        const auto& bin = bins_[static_cast<std::size_t>(index)];
        rasterizeTile(target, tile, bin.data(), bin.size(), shader);
        // Lines go on after every triangle in the tile has written its depth.
        if (edges) rasterizeTileEdges(target, tile, bin.data(), bin.size(), shader, lineWidth);
        finishTile(tile);
    });
}

void Renderer::renderShadowMap(std::span<const DrawItem> items, const Light& light, Vec3 center, float radius,
                               const RenderSettings& settings, const std::uint8_t* lodChoice, ShadowMap& map) {
    const int size = std::clamp(settings.shadowMapSize, 64, 2048);
    map.target.resize(size, size);
    aimShadowMap(map, light, center, radius);
    map.kernel = std::clamp(settings.shadowSoftness, 1, 7);

    GeometryPass pass;
    pass.viewProjection = map.viewProjection;
    pass.width = pass.height = size;
    pass.layout = VaryingLayout::DepthOnly;
    pass.varyingCount = varyingCountFor(pass.layout);
    pass.cullBackfaces = settings.cullBackfaces;
    pass.lodChoice = lodChoice;
    pass.points = settings.points;
    pass.pointScale = settings.pointScale;
    pass.projectionX = pass.projectionY = map.projectionScale;

    // The shadow pass's triangle counts and timings are reported as one
    // number, not mixed into the main view's.
    RenderStats ignored;
    buildGeometry(items, pass, ignored);
    DepthShader shader{materials_.data()};
    rasterize(map.target, shader, std::uint8_t{0}, false, 1, [](const TileRect&) {});
}

RenderStats Renderer::render(const Model& model, const Camera& camera, const RenderSettings& settings,
                             Framebuffer& fb) {
    const DrawItem item{&model, Mat4::identity()};
    return render(std::span<const DrawItem>(&item, 1), camera, settings, fb);
}

RenderStats Renderer::render(std::span<const DrawItem> items, const Camera& camera, const RenderSettings& settings,
                             Framebuffer& fb) {
    RenderStats stats;
    const int width = fb.width(), height = fb.height();
    if (width <= 0 || height <= 0) return stats;

    const bool flat = settings.shading == ShadingMode::Flat;
    const bool perPixel = !flat && settings.shading != ShadingMode::Gouraud;

    // One table of materials for the whole frame, so a triangle can name
    // its material with a single index whichever model it came from.
    materials_.clear();
    for (const DrawItem& item : items)
        for (const Material& material : item.model->materials)
            materials_.push_back({&material, item.model->textures.data()});

    // --------------------------------------------------------- level of detail
    // Chosen once per frame and shared by every pass, so that a shadow is
    // cast by the same triangles that receive it.
    const std::uint8_t* lodChoice = nullptr;
    // Wireframe is for looking at the mesh itself: always the original.
    if (settings.lodTolerance > 0.0f && !settings.wireframe) {
        lodChoice_.clear();
        bool any = false;
        // How many pixels one world unit covers at distance 1.
        const float pixelsPerUnit = 0.5f * static_cast<float>(height) * camera.projection.c[1].y;
        for (const DrawItem& item : items) {
            const bool haveLods = item.accel && item.accel->meshCount() == item.model->meshes.size();
            // A scaled model's error is scaled with it.
            const float scale = std::max({length(item.transform.c[0].xyz()), length(item.transform.c[1].xyz()),
                                          length(item.transform.c[2].xyz())});
            for (std::size_t m = 0; m < item.model->meshes.size(); ++m) {
                std::uint8_t choice = 0;
                if (haveLods && !item.accel->lods(m).empty()) {
                    const Bounds& box = item.accel->bounds(m);
                    const float radius = box.radius() * scale;
                    // Judge by the nearest part of the mesh: the part where
                    // an error is largest on screen.
                    const float distance =
                        std::max(length(transformPoint(item.transform, box.center()) - camera.position) - radius,
                                 radius * 0.05f);
                    const std::vector<MeshLod>& lods = item.accel->lods(m);
                    for (std::size_t k = 0; k < lods.size() && k < 255; ++k) {
                        if (lods[k].error * scale * pixelsPerUnit / distance > settings.lodTolerance) break;
                        choice = static_cast<std::uint8_t>(k + 1);
                    }
                }
                lodChoice_.push_back(choice);
                any = any || choice > 0;
            }
        }
        if (any) lodChoice = lodChoice_.data();
    }

    // ------------------------------------------------------------------ lights
    // Lighting is computed in world space; lights given relative to the
    // camera are brought there first.
    const Mat4 cameraToWorld = inverse(camera.view);
    std::vector<Light> lights;
    if (settings.lights.empty()) {
        Light key;
        key.direction = -settings.lightDirection;
        key.color = {2.0f, 1.94f, 1.86f};
        key.castsShadow = true;
        key.cameraSpace = true;
        Light fill;
        fill.direction = {-0.8f, 0.1f, -0.4f};
        fill.color = {0.25f, 0.3f, 0.4f};
        fill.specular = 0.0f;
        fill.cameraSpace = true;
        lights = {key, fill};
    } else {
        lights = settings.lights;
    }
    for (Light& light : lights) {
        if (light.cameraSpace) {
            light.direction = transformDirection(cameraToWorld, light.direction);
            light.position = transformPoint(cameraToWorld, light.position);
        }
        light.direction = normalize(light.direction, {0.0f, -1.0f, 0.0f});
    }
    const Environment* environment = settings.environment ? settings.environment : &defaultEnvironment();

    // ----------------------------------------------------------------- shadows
    std::vector<const ShadowMap*> shadows(lights.size(), nullptr);
    if (settings.shadows && perPixel && !settings.wireframe) {
        const auto t0 = std::chrono::steady_clock::now();
        // A sphere around everything, for the lights to aim at.
        Bounds bounds;
        bool any = false;
        for (const DrawItem& item : items) {
            const Bounds b = item.model->bounds();
            for (int corner = 0; corner < 8; ++corner) {
                const Vec3 p = transformPoint(
                    item.transform,
                    {corner & 1 ? b.max.x : b.min.x, corner & 2 ? b.max.y : b.min.y, corner & 4 ? b.max.z : b.min.z});
                bounds.min = any ? min(bounds.min, p) : p;
                bounds.max = any ? max(bounds.max, p) : p;
                any = true;
            }
        }
        int used = 0;
        for (std::size_t i = 0; i < lights.size() && used < kMaxShadowMaps; ++i) {
            if (!lights[i].castsShadow || lights[i].type == LightType::Point) continue;
            renderShadowMap(items, lights[i], bounds.center(), bounds.radius(), settings, lodChoice, shadowMaps_[used]);
            shadows[i] = &shadowMaps_[used++];
        }
        if (used > 0) stats.shadowMs = msSince(t0);
    }

    shadingLights_.clear();
    for (std::size_t i = 0; i < lights.size(); ++i) shadingLights_.push_back(prepareLight(lights[i], shadows[i]));
    LightingContext lighting;
    lighting.lights = shadingLights_.data();
    lighting.lightCount = static_cast<int>(shadingLights_.size());
    lighting.environment = environment;

    // ---------------------------------------------------------------- geometry
    GeometryPass pass;
    pass.viewProjection = camera.projection * camera.view;
    pass.eye = camera.position;
    pass.width = width;
    pass.height = height;
    pass.layout = perPixel ? VaryingLayout::PerPixel : VaryingLayout::Lit;
    pass.flat = flat;
    pass.cullBackfaces = settings.cullBackfaces;
    // In wireframe every surface is just a depth mask plus lines.
    pass.sortTranslucent = !settings.wireframe;
    pass.lighting = &lighting;
    pass.highlightItem = settings.highlightItem;
    pass.highlightMesh = settings.highlightMesh;
    pass.lodChoice = lodChoice;
    pass.normalMaps = settings.normalMaps;
    pass.textures = settings.textures;
    pass.points = settings.points;
    pass.pointScale = settings.pointScale;
    pass.thinPoints = settings.lodTolerance > 0.0f;
    pass.projectionX = camera.projection.c[0].x;
    pass.projectionY = camera.projection.c[1].y;
    // Vertex-lit frames grow three varyings wider only if something in
    // them has per-vertex colours.
    pass.varyingCount = varyingCountFor(pass.layout);
    if (!perPixel)
        for (const DrawItem& item : items)
            for (const Mesh& mesh : item.model->meshes)
                if (!mesh.colors.empty() || settings.points) pass.varyingCount = kLitColorVaryingCount;
    buildGeometry(items, pass, stats);

    // ------------------------------------------------------------ rasterization
    auto t0 = std::chrono::steady_clock::now();
    const int lineWidth = std::max(static_cast<int>(std::lround(settings.wireWidth)), 1);
    // Which version of the shader this frame needs (see FragmentShader).
    const bool extras = passHasExtras_ || !blendOrder_.empty() || settings.highlightItem >= 0;
    // Rasterize the frame into `target`, finishing pixels through `output`.
    auto draw = [&](auto& target, auto output, auto clearColor, const auto& finishTile) {
        auto run = [&](auto shader) {
            shader.output = output;
            shader.materials = materials_.data();
            shader.lighting = lighting;
            shader.eye = camera.position;
            shader.mode = settings.shading;
            shader.lineWidth = lineWidth;
            shader.varyings = pass.varyingCount;
            shader.perPixelLighting = perPixel;
            shader.wireframe = settings.wireframe;
            shader.textures = settings.textures;
            shader.normalMaps = settings.normalMaps;
            rasterize(target, shader, clearColor, settings.wireframe, lineWidth, finishTile);
        };
        if (extras) run(FragmentShader<decltype(output), true>{});
        else run(FragmentShader<decltype(output), false>{});
    };

    Backdrop backdrop;
    backdrop.color = settings.background;
    backdrop.sky = settings.skybox ? environment : nullptr;
    backdrop.skyBlur = settings.skyboxBlur;
    const bool wholeFrame = PostProcessor::needsWholeFrame(settings.post, backdrop);

    // Shading produces linear HDR colour, and the general route is to keep
    // it that way in a float buffer so the post-processing passes can work
    // on real light values. But a float buffer is four times the memory of
    // an 8-bit one, and most frames need none of that: no whole-frame
    // effect is on, and nothing is translucent (blending must happen in
    // linear light, before tone mapping). Those frames skip the HDR buffer
    // and tone map each pixel as it is shaded.
    if (!wholeFrame && blendOrder_.empty()) {
        Color8 empty = settings.background;
        empty.a = 0;
        draw(fb, DirectOutput{std::exp2(settings.post.exposure), settings.post.toneMapper, empty}, empty,
             [](const TileRect&) {});
        stats.rasterMs = msSince(t0);
        return stats;
    }

    // Coverage 0 marks "no geometry here".
    hdr_.resize(width, height);
    draw(hdr_, HdrOutput{}, Vec4(0.0f), [&](const TileRect& tile) {
        // With no whole-frame effects, all that is left to do to a tile is
        // tone mapping; do it now, while its pixels are still in the cache.
        if (!wholeFrame)
            PostProcessor::presentRect(hdr_, settings.post, backdrop, fb, tile.x0, tile.y0, tile.x1, tile.y1);
    });
    stats.rasterMs = msSince(t0);

    // --------------------------------------------------------- post-processing
    if (wholeFrame) {
        t0 = std::chrono::steady_clock::now();
        post_.run(hdr_, camera, settings.post, backdrop, settings.pixelScale, fb);
        stats.postMs = msSince(t0);
    }

    return stats;
}

}  // namespace termesh
