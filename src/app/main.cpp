// termesh - milestone 1: one flat-shaded triangle, printed with half-blocks.
#include <cstdio>

#include "core/framebuffer.hpp"
#include "math/mat.hpp"
#include "math/quat.hpp"
#include "output/halfblock.hpp"
#include "raster/rasterizer.hpp"

using namespace termesh;

int main() {
    // 80x24 terminal cells; half-blocks give two pixels per cell vertically.
    const int width = 80, height = 48;
    Framebuffer fb(width, height);
    fb.clear({18, 18, 24, 255});

    // A triangle in model space, turned a little so the lighting is visible.
    const Vec3 positions[3] = {{-1.0f, -0.8f, 0.0f}, {1.0f, -0.8f, 0.0f}, {0.0f, 0.9f, 0.0f}};
    const Mat4 model = toMat4(Quat::fromAxisAngle({0.0f, 1.0f, 0.0f}, radians(35.0f)));
    const Mat4 view = lookAt({0.0f, 0.0f, 3.0f}, {0.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f});
    const Mat4 projection =
        perspective(radians(45.0f), static_cast<float>(width) / height, 0.1f, 100.0f);

    // Flat shading: one normal for the whole face, one Lambert term.
    const Vec3 w0 = transformPoint(model, positions[0]);
    const Vec3 w1 = transformPoint(model, positions[1]);
    const Vec3 w2 = transformPoint(model, positions[2]);
    const Vec3 normal = normalize(cross(w1 - w0, w2 - w0));
    const Vec3 toLight = normalize(Vec3{0.4f, 0.6f, 1.0f});
    const float diffuse = std::max(dot(normal, toLight), 0.0f);
    const Vec3 albedo{1.0f, 0.55f, 0.15f};
    const Vec3 lit = saturate(albedo * (0.15f + 0.85f * diffuse));
    const Color8 color{static_cast<std::uint8_t>(lit.x * 255.0f + 0.5f),
                       static_cast<std::uint8_t>(lit.y * 255.0f + 0.5f),
                       static_cast<std::uint8_t>(lit.z * 255.0f + 0.5f), 255};

    // Model space -> clip space -> NDC -> screen space.
    Vec3 screen[3];
    const Mat4 mvp = projection * view * model;
    for (int i = 0; i < 3; ++i) {
        const Vec4 clip = mvp * Vec4(positions[i], 1.0f);
        const Vec3 ndc = clip.xyz() / clip.w;  // the perspective divide
        screen[i] = {(ndc.x * 0.5f + 0.5f) * width,
                     (1.0f - (ndc.y * 0.5f + 0.5f)) * height,  // screen y points down
                     ndc.z * 0.5f + 0.5f};
    }

    fillTriangle(fb, screen[0], screen[1], screen[2], color);

    const std::string out = encodeHalfBlocks(fb);
    std::fwrite(out.data(), 1, out.size(), stdout);
    return 0;
}
