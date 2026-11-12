#include "post/post.hpp"

#include <algorithm>
#include <cmath>

#include "core/color.hpp"

namespace termesh {

namespace {

constexpr int kRowsPerJob = 16;

// Run body(y) for every row, in parallel.
template <typename Body>
void forEachRow(ThreadPool& pool, int height, const Body& body) {
    pool.parallelFor((height + kRowsPerJob - 1) / kRowsPerJob, [&](int job) {
        const int end = std::min((job + 1) * kRowsPerJob, height);
        for (int y = job * kRowsPerJob; y < end; ++y) body(y);
    });
}

// Depth buffer value -> view-space position.
//
// The projection matrix turned view-space (x, y, z) into clip space and the
// divide by w turned that into NDC; this runs both backwards. For the
// perspective() matrix, clip.z = A z + B and clip.w = -z, so
//
//     ndc.z = (A z + B) / -z     =>     z = -B / (ndc.z + A)
//
// and x, y follow by undoing their scale and the divide.
struct Unprojector {
    float a, b, invP00, invP11, width, height;
    bool perspective;
    float offsetX, offsetY;  // orthographic only

    Unprojector(const Mat4& projection, int w, int h)
        : a(projection.c[2].z),
          b(projection.c[3].z),
          invP00(1.0f / projection.c[0].x),
          invP11(1.0f / projection.c[1].y),
          width(static_cast<float>(w)),
          height(static_cast<float>(h)),
          perspective(projection.c[2].w != 0.0f),
          offsetX(projection.c[3].x),
          offsetY(projection.c[3].y) {}

    float viewZ(float depth) const {
        const float ndcZ = depth * 2.0f - 1.0f;
        return perspective ? -b / (ndcZ + a) : (ndcZ - b) / a;
    }

    Vec3 position(int x, int y, float depth) const {
        const float ndcX = (static_cast<float>(x) + 0.5f) / width * 2.0f - 1.0f;
        const float ndcY = 1.0f - (static_cast<float>(y) + 0.5f) / height * 2.0f;
        const float z = viewZ(depth);
        if (perspective) return {ndcX * -z * invP00, ndcY * -z * invP11, z};
        return {(ndcX - offsetX) * invP00, (ndcY - offsetY) * invP11, z};
    }
};

}  // namespace

void PostProcessor::run(HdrFramebuffer& hdr, const Camera& camera, const PostSettings& settings,
                        const Backdrop& backdrop, float pixelScale, Framebuffer& out) {
    if (backdrop.sky) fillSky(hdr, camera, backdrop);
    if (settings.ssao || settings.outline) reconstructGeometry(hdr, camera);
    if (settings.ssao) ambientOcclusion(hdr, camera, settings);
    if (settings.outline) outline(hdr, settings, pixelScale);
    if (settings.bloom) bloom(hdr, settings, pixelScale);

    const int width = hdr.width(), height = hdr.height();
    out.resize(width, height);
    forEachRow(pool_, height, [&](int y) { presentRect(hdr, settings, backdrop, out, 0, y, width, y + 1); });
}

// Every pixel is a ray from the eye. For the ones that hit nothing, look the
// ray's direction up in the environment.
void PostProcessor::fillSky(HdrFramebuffer& hdr, const Camera& camera, const Backdrop& backdrop) {
    const int width = hdr.width(), height = hdr.height();
    const Unprojector unproject(camera.projection, width, height);
    const Mat4 cameraToWorld = inverse(camera.view);
    forEachRow(pool_, height, [&](int y) {
        Vec4* row = hdr.colorRow(y);
        for (int x = 0; x < width; ++x) {
            if (row[x].w >= 1.0f) continue;
            // Any depth gives a point on the ray; its direction is what matters.
            const Vec3 direction = normalize(transformDirection(cameraToWorld, unproject.position(x, y, 0.5f)));
            const Vec3 sky = backdrop.sky->background(direction, backdrop.skyBlur);
            // Translucent surfaces let (1 - coverage) of the sky through.
            row[x] = Vec4(row[x].xyz() + sky * (1.0f - row[x].w), row[x].w);
        }
    });
}

// Recover the view-space position and surface normal of every pixel from
// the depth buffer. The normal is the cross product of the position's change
// across the screen: two vectors lying in the surface.
//
// At an edge, the neighbour on one side belongs to a different surface and
// would give a nonsense normal. So for each direction there are two
// candidates - the step to the previous pixel and the step to the next -
// and the one to use is the side on which the surface carries on in a
// straight line: where 1/z, which is linear across the screen for a flat
// surface, bends least over the next two pixels.
void PostProcessor::reconstructGeometry(const HdrFramebuffer& hdr, const Camera& camera) {
    const int width = hdr.width(), height = hdr.height();
    const std::size_t count = static_cast<std::size_t>(width) * height;
    positions_.resize(count);
    normals_.resize(count);
    const Unprojector unproject(camera.projection, width, height);

    forEachRow(pool_, height, [&](int y) {
        const float* depth = hdr.depthRow(y);
        Vec3* out = positions_.data() + static_cast<std::size_t>(y) * width;
        for (int x = 0; x < width; ++x) out[x] = unproject.position(x, y, depth[x]);
    });

    // `step` is the distance between neighbours in the array: 1 across a
    // row, `width` down a column. `before` / `after` say how many
    // neighbours exist on each side.
    auto slope = [&](std::size_t index, std::size_t step, int before, int after) {
        const Vec3* p = positions_.data() + index;
        auto inverseZ = [&](const Vec3& q) { return 1.0f / std::min(q.z, -1e-6f); };
        const Vec3 backward = before >= 1 ? p[0] - *(p - step) : Vec3{};
        const Vec3 forward = after >= 1 ? *(p + step) - p[0] : Vec3{};
        if (before < 1) return forward;
        if (after < 1) return backward;
        const float here = inverseZ(p[0]);
        // How far 1/z strays from a straight line on each side. With only
        // one neighbour on a side there is nothing to judge it by.
        const float bendBackward =
            before >= 2 ? std::abs(inverseZ(*(p - 2 * step)) - 2.0f * inverseZ(*(p - step)) + here) : 1e30f;
        const float bendForward =
            after >= 2 ? std::abs(inverseZ(*(p + 2 * step)) - 2.0f * inverseZ(*(p + step)) + here) : 1e30f;
        return bendBackward < bendForward ? backward : forward;
    };
    forEachRow(pool_, height, [&](int y) {
        Vec3* out = normals_.data() + static_cast<std::size_t>(y) * width;
        for (int x = 0; x < width; ++x) {
            const std::size_t index = static_cast<std::size_t>(y) * width + x;
            const Vec3 dx = slope(index, 1, x, width - 1 - x);
            const Vec3 dy = slope(index, static_cast<std::size_t>(width), y, height - 1 - y);
            // Screen y runs downwards, so (down, right) is a right-handed
            // pair whose cross product points back towards the camera.
            out[x] = normalize(cross(dy, dx), {0.0f, 0.0f, 1.0f});
        }
    });
}

// Screen-space ambient occlusion.
//
// Ambient light comes from all around, so a point at the bottom of a crease
// receives less of it than a point on open ground: its neighbours are in the
// way. SSAO estimates "how much is in the way" using only the depth buffer.
// For each pixel it tries a few sample points scattered in the hemisphere
// above the surface, projects each back to the screen, and asks the depth
// buffer whether something nearer to the camera was drawn there. The
// fraction of samples that end up *inside* geometry is the occlusion.
//
// Eight samples per pixel would be noisy, so each pixel rotates the sample
// pattern by a different angle from a repeating 4x4 table, and a 4x4 blur
// afterwards averages exactly one full table's worth of rotations.
void PostProcessor::ambientOcclusion(HdrFramebuffer& hdr, const Camera& camera, const PostSettings& settings) {
    const int width = hdr.width(), height = hdr.height();
    const std::size_t count = static_cast<std::size_t>(width) * height;
    scratchA_.resize(count);
    scratchB_.resize(count);
    const Unprojector unproject(camera.projection, width, height);

    // The scene's extent in depth, recovered from the near and far planes
    // the camera fitted around it, gives the radius a scale-free meaning.
    const float zNear = std::abs(unproject.viewZ(0.0f)), zFar = std::abs(unproject.viewZ(1.0f));
    const float radius = std::max(settings.ssaoRadius * (zFar - zNear) * 0.5f, 1e-6f);

    // Points in the unit hemisphere around +z, at assorted distances so that
    // nearby occluders count for more than distant ones.
    static const Vec3 kKernel[8] = {
        {0.20f, 0.05f, 0.12f}, {-0.18f, 0.22f, 0.25f}, {0.10f, -0.35f, 0.30f}, {-0.42f, -0.20f, 0.33f},
        {0.48f, 0.38f, 0.42f}, {-0.25f, 0.62f, 0.50f}, {0.66f, -0.40f, 0.48f}, {-0.60f, -0.55f, 0.52f},
    };

    const Mat4& projection = camera.projection;
    forEachRow(pool_, height, [&](int y) {
        const Vec4* color = hdr.colorRow(y);
        float* ao = scratchA_.data() + static_cast<std::size_t>(y) * width;
        for (int x = 0; x < width; ++x) {
            ao[x] = 1.0f;
            if (color[x].w <= 0.0f) continue;
            const std::size_t index = static_cast<std::size_t>(y) * width + x;
            const Vec3 p = positions_[index], n = normals_[index];

            // A tangent frame around the normal, turned by this pixel's angle.
            const float angle = static_cast<float>(((x & 3) * 4 + (y & 3)) * 7 % 16) * (2.0f * kPi / 16.0f);
            const Vec3 random{std::cos(angle), std::sin(angle), 0.0f};
            Vec3 t = random - n * dot(random, n);
            if (lengthSquared(t) < 1e-6f) t = cross(n, {0.0f, 1.0f, 0.0f});
            t = normalize(t);
            const Vec3 b = cross(n, t);

            float occlusion = 0.0f;
            for (const Vec3& k : kKernel) {
                const Vec3 sample = p + (t * k.x + b * k.y + n * k.z) * radius;
                const Vec4 clip = projection * Vec4(sample, 1.0f);
                if (clip.w <= 0.0f) continue;
                const int sx = static_cast<int>((clip.x / clip.w * 0.5f + 0.5f) * static_cast<float>(width));
                const int sy = static_cast<int>((0.5f - clip.y / clip.w * 0.5f) * static_cast<float>(height));
                if (sx < 0 || sy < 0 || sx >= width || sy >= height) continue;
                const float sceneZ = unproject.viewZ(hdr.depthRow(sy)[sx]);
                // View space looks down -z: larger z is nearer the camera.
                if (sceneZ < sample.z + radius * 0.05f) continue;
                // Something far in front of the sample is a different object
                // altogether, not a neighbour; fade its contribution out.
                const float range = radius / std::max(std::abs(p.z - sceneZ), 1e-6f);
                occlusion += std::min(range, 1.0f);
            }
            ao[x] = 1.0f - occlusion / static_cast<float>(std::size(kKernel));
        }
    });

    // 4x4 box blur (two passes of 4), ignoring background pixels.
    forEachRow(pool_, height, [&](int y) {
        const float* in = scratchA_.data() + static_cast<std::size_t>(y) * width;
        float* out = scratchB_.data() + static_cast<std::size_t>(y) * width;
        for (int x = 0; x < width; ++x) {
            float sum = 0.0f;
            for (int k = -2; k < 2; ++k) sum += in[std::clamp(x + k, 0, width - 1)];
            out[x] = sum * 0.25f;
        }
    });
    const float strength = std::clamp(settings.ssaoStrength, 0.0f, 4.0f);
    forEachRow(pool_, height, [&](int y) {
        Vec4* color = hdr.colorRow(y);
        for (int x = 0; x < width; ++x) {
            if (color[x].w <= 0.0f) continue;
            float sum = 0.0f;
            for (int k = -2; k < 2; ++k)
                sum += scratchB_[static_cast<std::size_t>(std::clamp(y + k, 0, height - 1)) * width + x];
            const float ao = std::clamp(1.0f - strength * (1.0f - sum * 0.25f), 0.0f, 1.0f);
            color[x] = Vec4(color[x].xyz() * ao, color[x].w);
        }
    });
}

// Ink outlines.
//
// An illustrator draws a line where one thing ends and another begins. In
// the buffers that is wherever, between a pixel and its neighbour,
//
//   * coverage changes        the silhouette against the background
//   * depth jumps             one object in front of another
//   * the normal turns sharply  a crease, such as the edge of a box
//
// Depth is tested through its second difference (is this pixel far from
// the average of its neighbours?) rather than the first, because a flat
// surface seen at a glancing angle changes depth quickly from pixel to
// pixel and would otherwise be inked all over. And the quantity compared
// is 1/z rather than z: under perspective it is 1/z, not z, that varies
// in a straight line across the screen along a flat surface (the same fact
// that perspective-correct interpolation rests on), so for a plane the
// second difference of 1/z is exactly zero however steeply it recedes.
void PostProcessor::outline(HdrFramebuffer& hdr, const PostSettings& settings, float pixelScale) {
    const int width = hdr.width(), height = hdr.height();
    const std::size_t count = static_cast<std::size_t>(width) * height;
    scratchA_.resize(count);
    scratchB_.resize(count);
    constexpr float kCreaseCos = 0.5f;        // normals more than 60 degrees apart
    constexpr float kDepthThreshold = 0.02f;  // a step of 2% of the distance from the camera

    forEachRow(pool_, height, [&](int y) {
        float* edge = scratchA_.data() + static_cast<std::size_t>(y) * width;
        const int y0 = std::max(y - 1, 0), y1 = std::min(y + 1, height - 1);
        for (int x = 0; x < width; ++x) {
            edge[x] = 0.0f;
            const bool covered = hdr.colorRow(y)[x].w > 0.0f;
            if (!covered) continue;  // lines are drawn on the object's side
            const int x0 = std::max(x - 1, 0), x1 = std::min(x + 1, width - 1);
            const std::size_t index = static_cast<std::size_t>(y) * width + x;
            const std::size_t neighbours[4] = {
                static_cast<std::size_t>(y) * width + x0, static_cast<std::size_t>(y) * width + x1,
                static_cast<std::size_t>(y0) * width + x, static_cast<std::size_t>(y1) * width + x};
            const float coverage[4] = {hdr.colorRow(y)[x0].w, hdr.colorRow(y)[x1].w, hdr.colorRow(y0)[x].w,
                                       hdr.colorRow(y1)[x].w};

            bool isEdge = false;
            for (int k = 0; k < 4 && !isEdge; ++k) isEdge = coverage[k] <= 0.0f;
            if (!isEdge) {
                auto inverseZ = [&](std::size_t i) { return 1.0f / std::min(positions_[i].z, -1e-6f); };
                const float w = inverseZ(index);
                const float dx = inverseZ(neighbours[0]) + inverseZ(neighbours[1]) - 2.0f * w;
                const float dy = inverseZ(neighbours[2]) + inverseZ(neighbours[3]) - 2.0f * w;
                isEdge = std::max(std::abs(dx), std::abs(dy)) > kDepthThreshold * std::abs(w);
            }
            if (!isEdge) {
                const Vec3 n = normals_[index];
                // Only the right and lower neighbours: testing all four
                // would draw every crease two pixels wide.
                isEdge = dot(n, normals_[neighbours[1]]) < kCreaseCos || dot(n, normals_[neighbours[3]]) < kCreaseCos;
            }
            edge[x] = isEdge ? 1.0f : 0.0f;
        }
    });

    // Thicken the one-pixel lines to the requested width: a pixel is inked
    // if any pixel within `radius` of it is an edge (horizontally, then
    // vertically).
    const int radius = std::max(static_cast<int>(std::lround(settings.outlineWidth * pixelScale)) / 2, 0);
    const std::vector<float>* mask = &scratchA_;
    if (radius > 0) {
        forEachRow(pool_, height, [&](int y) {
            const float* in = scratchA_.data() + static_cast<std::size_t>(y) * width;
            float* out = scratchB_.data() + static_cast<std::size_t>(y) * width;
            for (int x = 0; x < width; ++x) {
                float m = 0.0f;
                for (int k = std::max(x - radius, 0); k <= std::min(x + radius, width - 1); ++k) m = std::max(m, in[k]);
                out[x] = m;
            }
        });
        forEachRow(pool_, height, [&](int y) {
            float* out = scratchA_.data() + static_cast<std::size_t>(y) * width;
            for (int x = 0; x < width; ++x) {
                float m = 0.0f;
                for (int k = std::max(y - radius, 0); k <= std::min(y + radius, height - 1); ++k)
                    m = std::max(m, scratchB_[static_cast<std::size_t>(k) * width + x]);
                out[x] = m;
            }
        });
    }
    forEachRow(pool_, height, [&](int y) {
        const float* edge = mask->data() + static_cast<std::size_t>(y) * width;
        Vec4* color = hdr.colorRow(y);
        for (int x = 0; x < width; ++x)
            if (edge[x] > 0.0f) color[x] = Vec4(settings.outlineColor, 1.0f);
    });
}

// Bloom.
//
// A real lens (and a real eye) scatters a little of the light from a very
// bright source over its surroundings. Reproducing that glow is also the
// only way to say "this is brighter than white" on a display that cannot
// show it.
//
//   1. keep only what exceeds the threshold;
//   2. blur it. A wide blur done directly is expensive, so the image is
//      halved repeatedly instead - each halving doubles the reach of a
//      small filter - and the levels are then added back up, largest blur
//      first, each one enlarged with bilinear filtering;
//   3. add the result to the picture.
void PostProcessor::bloom(HdrFramebuffer& hdr, const PostSettings& settings, float pixelScale) {
    const int width = hdr.width(), height = hdr.height();
    // Enough levels that the glow reaches about the same distance on screen
    // whatever the supersampling factor.
    const int wanted = 4 + static_cast<int>(std::lround(std::log2(std::max(pixelScale, 1.0f))));
    bloomLevels_.resize(static_cast<std::size_t>(std::clamp(wanted, 1, 8)));

    // Level 0: the bright part of the frame at half resolution.
    const float threshold = std::max(settings.bloomThreshold, 0.0f);
    int levels = 0;
    for (std::size_t i = 0; i < bloomLevels_.size(); ++i) {
        BloomLevel& level = bloomLevels_[i];
        const int srcWidth = i == 0 ? width : bloomLevels_[i - 1].width;
        const int srcHeight = i == 0 ? height : bloomLevels_[i - 1].height;
        if (srcWidth < 2 || srcHeight < 2) break;
        level.width = srcWidth / 2;
        level.height = srcHeight / 2;
        level.pixels.resize(static_cast<std::size_t>(level.width) * level.height);
        ++levels;

        forEachRow(pool_, level.height, [&](int y) {
            Vec3* out = level.pixels.data() + static_cast<std::size_t>(y) * level.width;
            for (int x = 0; x < level.width; ++x) {
                Vec3 sum;
                for (int dy = 0; dy < 2; ++dy)
                    for (int dx = 0; dx < 2; ++dx) {
                        if (i == 0) {
                            const Vec3 c = hdr.colorRow(y * 2 + dy)[x * 2 + dx].xyz();
                            // Subtract the threshold from the brightness but
                            // keep the hue: scale the colour, do not clip it.
                            const float brightness = std::max({c.x, c.y, c.z});
                            if (brightness > threshold) sum += c * ((brightness - threshold) / brightness);
                        } else {
                            const BloomLevel& src = bloomLevels_[i - 1];
                            sum += src.pixels[static_cast<std::size_t>(y * 2 + dy) * src.width + (x * 2 + dx)];
                        }
                    }
                out[x] = sum * 0.25f;
            }
        });
    }
    if (levels == 0) return;

    auto sampleBilinear = [](const BloomLevel& level, float u, float v) {
        const float x =
            std::clamp(u * static_cast<float>(level.width) - 0.5f, 0.0f, static_cast<float>(level.width - 1));
        const float y =
            std::clamp(v * static_cast<float>(level.height) - 0.5f, 0.0f, static_cast<float>(level.height - 1));
        const int x0 = static_cast<int>(x), y0 = static_cast<int>(y);
        const int x1 = std::min(x0 + 1, level.width - 1), y1 = std::min(y0 + 1, level.height - 1);
        const float tx = x - static_cast<float>(x0), ty = y - static_cast<float>(y0);
        const Vec3* p = level.pixels.data();
        const Vec3 top = lerp(p[static_cast<std::size_t>(y0) * level.width + x0],
                              p[static_cast<std::size_t>(y0) * level.width + x1], tx);
        const Vec3 bottom = lerp(p[static_cast<std::size_t>(y1) * level.width + x0],
                                 p[static_cast<std::size_t>(y1) * level.width + x1], tx);
        return lerp(top, bottom, ty);
    };

    // Add each level into the next larger one, smallest first.
    for (int i = levels - 1; i > 0; --i) {
        const BloomLevel& small = bloomLevels_[static_cast<std::size_t>(i)];
        BloomLevel& large = bloomLevels_[static_cast<std::size_t>(i - 1)];
        forEachRow(pool_, large.height, [&](int y) {
            Vec3* out = large.pixels.data() + static_cast<std::size_t>(y) * large.width;
            const float v = (static_cast<float>(y) + 0.5f) / static_cast<float>(large.height);
            for (int x = 0; x < large.width; ++x)
                out[x] += sampleBilinear(small, (static_cast<float>(x) + 0.5f) / static_cast<float>(large.width), v);
        });
    }

    const float scale = settings.bloomStrength / static_cast<float>(levels);
    const BloomLevel& glow = bloomLevels_[0];
    forEachRow(pool_, height, [&](int y) {
        Vec4* color = hdr.colorRow(y);
        const float v = (static_cast<float>(y) + 0.5f) / static_cast<float>(height);
        for (int x = 0; x < width; ++x) {
            const Vec3 add =
                sampleBilinear(glow, (static_cast<float>(x) + 0.5f) / static_cast<float>(width), v) * scale;
            color[x] = Vec4(color[x].xyz() + add, color[x].w);
        }
    });
}

// The last step: exposure, the tone curve, and the sRGB encoding.
//
// A flat-colour background is given as a finished sRGB colour and should
// come out exactly as given, so it is added *after* tone mapping, weighted
// by how much of the pixel the geometry left uncovered. (A sky, in
// contrast, is light like any other and has already been mixed in.)
void PostProcessor::presentRect(const HdrFramebuffer& hdr, const PostSettings& settings, const Backdrop& backdrop,
                                Framebuffer& out, int x0, int y0, int x1, int y1) {
    const float exposure = std::exp2(settings.exposure);
    const ToneMapper mapper = settings.toneMapper;
    const bool sky = backdrop.sky != nullptr;
    const Vec3 background = srgbToLinear(backdrop.color);
    Color8 empty = backdrop.color;
    empty.a = 0;  // alpha 0 marks "no geometry here"

    for (int y = y0; y < y1; ++y) {
        const Vec4* in = hdr.colorRow(y);
        Color8* dst = out.colorRow(y);
        for (int x = x0; x < x1; ++x) {
            const Vec4 p = in[x];
            const std::uint8_t alpha = p.w > 0.0f ? 255 : 0;
            if (sky) {
                dst[x] = linearToSrgb(toneMap(p.xyz() * exposure, mapper), alpha);
            } else if (p.w >= 1.0f) {
                dst[x] = linearToSrgb(toneMap(p.xyz() * exposure, mapper), alpha);
            } else if (p.w <= 0.0f && p.x == 0.0f && p.y == 0.0f && p.z == 0.0f) {
                dst[x] = empty;
            } else {
                dst[x] = linearToSrgb(toneMap(p.xyz() * exposure, mapper) + background * (1.0f - p.w), alpha);
            }
        }
    }
}

// FXAA (after Timothy Lottes' algorithm, in its compact form).
//
// Supersampling fixes aliasing by brute force. FXAA is the cheap
// alternative: look at the finished image, find pixels that sit on a
// high-contrast edge, work out which way the edge runs from the brightness
// of the neighbours, and blur *along* the edge only. Jagged steps are
// smoothed while detail across the edge stays sharp.
void applyFxaa(Image& image, ThreadPool& pool) {
    const int width = image.width, height = image.height;
    if (width < 3 || height < 3) return;
    const std::vector<Color8> source = image.pixels;
    auto at = [&](int x, int y) -> const Color8& {
        return source[static_cast<std::size_t>(std::clamp(y, 0, height - 1)) * width + std::clamp(x, 0, width - 1)];
    };
    auto lumaAt = [&](int x, int y) { return static_cast<float>(luma(at(x, y))) * (1.0f / 255.0f); };
    auto sample = [&](float x, float y) {
        x = std::clamp(x, 0.0f, static_cast<float>(width - 1));
        y = std::clamp(y, 0.0f, static_cast<float>(height - 1));
        const int x0 = static_cast<int>(x), y0 = static_cast<int>(y);
        const float tx = x - static_cast<float>(x0), ty = y - static_cast<float>(y0);
        const Color8 &a = at(x0, y0), &b = at(x0 + 1, y0), &c = at(x0, y0 + 1), &d = at(x0 + 1, y0 + 1);
        auto mix = [&](float p, float q, float r, float s) {
            return (p + (q - p) * tx) * (1.0f - ty) + (r + (s - r) * tx) * ty;
        };
        return Vec3{mix(a.r, b.r, c.r, d.r), mix(a.g, b.g, c.g, d.g), mix(a.b, b.b, c.b, d.b)};
    };
    auto lumaOf = [](Vec3 c) { return (c.x * 77.0f + c.y * 150.0f + c.z * 29.0f) * (1.0f / (256.0f * 255.0f)); };

    forEachRow(pool, height, [&](int y) {
        Color8* out = image.row(y);
        for (int x = 0; x < width; ++x) {
            const float nw = lumaAt(x - 1, y - 1), ne = lumaAt(x + 1, y - 1);
            const float sw = lumaAt(x - 1, y + 1), se = lumaAt(x + 1, y + 1);
            const float m = lumaAt(x, y);
            const float lo = std::min({m, nw, ne, sw, se}), hi = std::max({m, nw, ne, sw, se});
            // Not an edge worth touching: leave flat areas and fine texture alone.
            if (hi - lo < std::max(0.0312f, hi * 0.125f)) continue;

            // The edge runs perpendicular to the brightness gradient.
            float dx = -((nw + ne) - (sw + se)), dy = (nw + sw) - (ne + se);
            const float reduce = std::max((nw + ne + sw + se) * 0.03125f, 1.0f / 128.0f);
            const float scale = 1.0f / (std::min(std::abs(dx), std::abs(dy)) + reduce);
            dx = std::clamp(dx * scale, -8.0f, 8.0f);
            dy = std::clamp(dy * scale, -8.0f, 8.0f);

            const float fx = static_cast<float>(x), fy = static_cast<float>(y);
            // Two taps close to the pixel, then two further out. If the wide
            // average strays outside the local brightness range it has
            // reached across the edge, so fall back to the narrow one.
            const Vec3 narrow = (sample(fx + dx * (1.0f / 3.0f - 0.5f), fy + dy * (1.0f / 3.0f - 0.5f)) +
                                 sample(fx + dx * (2.0f / 3.0f - 0.5f), fy + dy * (2.0f / 3.0f - 0.5f))) *
                                0.5f;
            const Vec3 wide = narrow * 0.5f +
                              (sample(fx - dx * 0.5f, fy - dy * 0.5f) + sample(fx + dx * 0.5f, fy + dy * 0.5f)) * 0.25f;
            const float wideLuma = lumaOf(wide);
            const Vec3 result = (wideLuma < lo || wideLuma > hi) ? narrow : wide;
            out[x] = {static_cast<std::uint8_t>(result.x + 0.5f), static_cast<std::uint8_t>(result.y + 0.5f),
                      static_cast<std::uint8_t>(result.z + 0.5f), out[x].a};
        }
    });
}

}  // namespace termesh
