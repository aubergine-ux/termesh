# Architecture

This document follows one triangle from a model file to a character on your
screen. It is meant to be read next to the source: every section names the
file that implements it, and the trickiest parts (clipping, perspective-correct
interpolation, the fill rule) carry long comments in the code itself.

- [The big picture](#the-big-picture)
- [Source layout](#source-layout)
- [Conventions](#conventions)
- [1. Loading](#1-loading)
- [2. Vertex stage](#2-vertex-stage)
- [3. Clipping](#3-clipping)
- [4. Projection, culling and triangle setup](#4-projection-culling-and-triangle-setup)
- [5. Binning](#5-binning)
- [6. Rasterization](#6-rasterization)
- [7. Shading](#7-shading)
- [8. Resolve](#8-resolve)
- [9. Terminal output](#9-terminal-output)
- [10. Input and the frame loop](#10-input-and-the-frame-loop)
- [11. Animation](#11-animation)
- [12. Lights and shading models](#12-lights-and-shading-models)
- [13. HDR, blending and the two ways to finish a pixel](#13-hdr-blending-and-the-two-ways-to-finish-a-pixel)
- [14. Shadow mapping](#14-shadow-mapping)
- [15. Environment lighting and skyboxes](#15-environment-lighting-and-skyboxes)
- [16. Post-processing](#16-post-processing)
- [17. Dithering](#17-dithering)
- [18. The BVH: culling and picking](#18-the-bvh-culling-and-picking)
- [19. Level of detail](#19-level-of-detail)
- [20. Holding a frame rate](#20-holding-a-frame-rate)
- [21. Benchmarks and performance regressions](#21-benchmarks-and-performance-regressions)
- [22. Scenes and scene files](#22-scenes-and-scene-files)
- [23. Camera paths](#23-camera-paths)
- [24. The inspector](#24-the-inspector)
- [25. More formats: STL, PLY, point clouds](#25-more-formats-stl-ply-point-clouds)
- [Performance notes](#performance-notes)
- [Testing](#testing)

## The big picture

```
 model.glb ─► loader ─► Model (world-space meshes, materials, textures)
                          │
                          │  ◄── animation: pose ─► skinning rewrites the vertices (§11)
            ┌─────────────┴──────────────── Renderer::render ────────────────┐
            │                                                                │
            │  vertex stage      positions ─► clip space, build varyings     │  parallel
            │  clipping          cut triangles against the view frustum      │  parallel
            │  setup             divide by w, viewport, cull, snap to pixels │  parallel
            │  binning           which triangles touch which 32×32 tile      │
            │  rasterization     coverage ─► depth test ─► interpolate ─►    │  parallel
            │                    fragment shader ─► framebuffer              │
            │  post-processing   sky · SSAO · outlines · bloom · tone map    │  parallel
            └────────────────────────────────────────────────────────────────┘
                          │
                       resolve (average supersamples), FXAA
                          │
              output backend (kitty · sixel · half-block · Braille · ASCII), dithering
                          │
                     your terminal
```

Nothing here touches a GPU or a graphics API. The whole renderer is ordinary
C++ operating on arrays of floats and bytes.

## Source layout

| Directory        | What lives there                                                                 |
| ---------------- | -------------------------------------------------------------------------------- |
| `src/math/`      | `Vec2/3/4`, `Mat4`, `Quat`. Header-only, no dependencies.                        |
| `src/core/`      | Framebuffer, images, textures (mipmaps, filtering), colour space, thread pool, JSON. |
| `src/scene/`     | `Model` / `Mesh` / `Material`, rig data, the orbit camera, OBJ and glTF loaders, demos. |
| `src/anim/`      | Keyframe sampling, pose blending, skinning and morph targets, the timeline.      |
| `src/pipeline/`  | The renderer: vertex stage, clipping, triangle setup, shading models, shadows, resolve. |
| `src/post/`      | Post-processing: sky, SSAO, outlines, bloom, tone mapping, FXAA.                 |
| `src/accel/`     | BVH, frustum culling, ray picking, mesh simplification for level of detail.      |
| `src/stage/`     | Scenes of several models, scene files, camera paths.                             |
| `src/raster/`    | The triangle and line rasterizer and the SIMD wrappers it uses.                  |
| `src/output/`    | Terminal backends and the cell-diffing writer.                                   |
| `src/input/`     | Raw terminal access (POSIX / Windows) and the input parser.                      |
| `src/export/`    | Off-screen rendering, PNG and GIF writing.                                       |
| `src/app/`       | Command line, capability detection, the interactive viewer, inspector, benchmarks, `main`. |
| `tests/`         | doctest unit tests.                                                              |
| `third_party/`   | cgltf, stb_image, stb_image_write, doctest - single headers, vendored.           |

Dependencies point one way: `math` ← `core` ← `scene` ← `pipeline`/`raster` ←
`output`/`export` ← `app`. The rasterizer does not know what a terminal is, and
the backends do not know what a triangle is; they meet at a plain image.
`anim` sits beside the renderer rather than inside it: it reads the rig stored
in a `Model` and writes that model's vertices, and the renderer never knows the
difference between a model that moves and one that does not. (The one loop in
the diagram: the glTF loader calls `anim` once, to put a freshly loaded model
into its rest pose.)

## Conventions

These match OpenGL and glTF, and are stated once in `src/math/mat.hpp`:

- Vectors are columns; transforms compose right to left:
  `clip = projection * view * model * position`.
- World and view space are right-handed. The camera looks down **−Z**, +Y is up.
- Clip space: a point is visible when `-w ≤ x, y, z ≤ w`.
- Front faces wind counter-clockwise.
- Screen space has **y pointing down** (row 0 is the top), pixel centres at
  half-integer coordinates, depth in `[0, 1]` with 0 nearest.
- Texture coordinates have (0, 0) at the **top-left** of the image, as in glTF.
  The OBJ loader flips `v` on the way in.
- Colour maths is done on linear values. Textures are decoded from sRGB when
  sampled and the result is encoded back to sRGB at the very end
  (`src/core/color.hpp`).

## 1. Loading

`src/scene/loader.cpp` · `obj_loader.cpp` · `gltf_loader.cpp`

(This section covers OBJ and glTF; STL, PLY, point clouds and the optional
Assimp backend are in [section 25](#25-more-formats-stl-ply-point-clouds).)

Both loaders produce the same thing: a `Model`, which is a flat list of meshes
whose vertices are already in **world space**, so the renderer never deals
with a scene graph. For glTF the node hierarchy is copied into a `Rig` and the
model is posed once as the file describes it (see [Animation](#11-animation));
if the file has no animations that pose is final, the rig is thrown away, and
the result is exactly as if each node's transform had been baked into its
vertices.

A `Vertex` is position, normal, UV and tangent. Whatever a file leaves out is
generated (`src/scene/model.cpp`): smooth normals by area-weighted averaging of
face normals, tangents by solving for the direction in which `u` increases
across each triangle.

OBJ needs one extra step. A face corner in OBJ indexes positions, UVs and
normals *separately* (`f 3/7/2`), but the renderer wants a single index per
vertex, so every distinct triple becomes one output vertex via a hash map.

Materials are reduced to what a Blinn-Phong renderer can use: base colour,
metallic, roughness, emissive, and base-colour / normal / metallic-roughness /
emissive textures. Textures get a full mip chain at load time
(`src/core/texture.cpp`), averaged in linear light.

## 2. Vertex stage

`Renderer::render` in `src/pipeline/renderer.cpp`

Every vertex is multiplied by the view-projection matrix exactly once, giving
its **clip-space** position, and gets an array of **varyings** - the values the
rasterizer will interpolate across triangles. What goes in that array depends
on where lighting happens, and that is the whole difference between the three
shading modes:

| Mode    | Lighting evaluated…           | Varyings                              |
| ------- | ----------------------------- | ------------------------------------- |
| Flat    | once per triangle             | UV, lit diffuse RGB, lit specular RGB |
| Gouraud | once per vertex (right here)  | UV, lit diffuse RGB, lit specular RGB |
| Phong   | once per pixel                | UV, normal, world position, tangent   |

Flat and Gouraud carry the *result* of lighting; Phong carries its *inputs*.
Flat shading is Gouraud with all three corners given the same value, computed
from the true geometric normal of the face.

The vertex loop is split into chunks that the thread pool runs in parallel.

## 3. Clipping

`src/pipeline/clip.cpp` - read the comment at the top of that file first.

The perspective divide `x/w` is only meaningful for points in front of the
camera. A vertex behind it has negative `w`; dividing flips it through the
origin and it lands on the wrong side of the screen. So a triangle that
crosses the near plane must be cut *before* the divide.

Clip space makes that easy. The frustum is the region `-w ≤ x, y, z ≤ w`, so
each of its six planes has a one-line signed distance:

```
left:   w + x      bottom: w + y      near: w + z
right:  w - x      top:    w - y      far:  w - z        (≥ 0 means inside)
```

For each triangle the renderer computes a 6-bit **outcode** per vertex (one bit
per plane the vertex is outside of):

- AND of the three outcodes non-zero → all three vertices are beyond the same
  plane → the triangle is invisible, drop it.
- OR of the three is zero → entirely inside → no clipping needed. This is the
  overwhelmingly common case and costs three comparisons.
- Otherwise → clip against exactly the planes named in the OR.

The clipper is **Sutherland-Hodgman**: walk the polygon's edges against one
plane, keeping inside vertices and inserting a new vertex wherever an edge
crosses the plane.

```
            plane
   inside     │     outside
              │
      A ──────┼──────► B         A in,  B out  → emit intersection I
              I
      C ◄─────┼─────── B         B out, C in   → emit intersection, then C
              J
      C ─────► A                 both in       → emit A
```

The crossing point sits at `t = dA / (dA - dB)` along the edge, where `d` is
the plane distance above. Positions and all varyings are interpolated with
that same `t` using a plain lerp - valid because clip space is still linear;
nothing has been divided by `w` yet. A triangle clipped by all six planes can
end up with as many as nine vertices; the result is triangulated as a fan.

Two details worth knowing:

- The intersection is always computed *from the inside vertex towards the
  outside one*. Two triangles sharing an edge walk it in opposite directions,
  and this makes them produce bit-identical intersection points, so no crack
  can open along a clip plane.
- Clipping against the side planes is not strictly necessary (a scissor test
  would do), but it guarantees that every screen coordinate lies inside the
  viewport. The rasterizer relies on that bound to keep its integer maths in
  32 bits.

## 4. Projection, culling and triangle setup

`emitTriangle` in `src/pipeline/renderer.cpp`

For each surviving triangle:

1. **Perspective divide.** `ndc = clip.xyz / clip.w`, and `1/w` is kept for
   later.
2. **Viewport transform.** NDC `[-1, 1]` → pixels, flipping y.
3. **Snap to fixed point.** Coordinates are rounded to 1/16 of a pixel and
   stored as integers (see the fill rule below for why).
4. **Back-face culling.** The sign of the triangle's screen-space area says
   which way it winds. Because the viewport flips y, front faces
   (counter-clockwise in the model) come out *clockwise* on screen.
   Back-facing triangles are dropped unless the material is double-sided.
5. **Texture level of detail.** The ratio of the triangle's area in texture
   space to its area on screen is how much texture one pixel covers; half its
   log₂ is the mip level. Computing it once per triangle avoids per-pixel
   derivatives.

The output is a `RasterTriangle` (`src/raster/rasterizer.hpp`): three integer
positions, depths, `1/w` values and varyings, oriented so its area is positive.

## 5. Binning

The screen is divided into 32×32-pixel **tiles**. A serial pass walks the
triangles in submission order and appends a pointer to each tile its bounding
box touches. Tiles are then rasterized in parallel by the thread pool.

Two things fall out of this design:

- **No locks in the hot loop.** A tile is owned by one thread, and tiles do
  not overlap, so threads never write the same pixel.
- **Deterministic output.** Each tile sees its triangles in the original
  order no matter which thread set them up, so the image is identical with 1
  thread or 64. (A unit test checks exactly this.)

Each tile also clears its own colour and depth just before drawing into them,
while that memory is about to be in the thread's cache anyway.

## 6. Rasterization

`rasterizeTile` in `src/raster/rasterizer.hpp`

### Coverage: edge functions

For a triangle with vertices `v0, v1, v2`, define for each edge

```
E(p) = (b - a) × (p - a)        (2D cross product; a → b is the edge)
```

`E` is positive on one side of the edge, negative on the other and zero on
it. A pixel centre is inside the triangle when all three edge functions are
positive. `E` is linear in `p`, so after evaluating it once at the corner of
the bounding box, moving one pixel right is a single add, and one row down is
another.

### The fill rule, and why the maths is in integers

When two triangles share an edge, a pixel centre lying exactly on it must be
drawn by exactly one of them - otherwise you get either a hole or a
double-blended pixel. The standard **top-left rule** assigns it to the
triangle for which the edge is a top edge or a left edge. In code that is a
bias of −1 on the other edges, which turns "`E > 0`, or `E = 0` on a top-left
edge" into a single `E ≥ 0` sign-bit test.

That only works if `E` is *exact*. In floating point the two triangles round
the shared edge differently and the rule falls apart. So vertex positions are
snapped to a 1/16-pixel grid and all edge functions are computed in integers.
Because clipping bounds every coordinate to the viewport, the values fit in
32 bits for framebuffers up to about 8 megapixels.

### Four pixels at a time

The inner loop handles four horizontally adjacent pixels per step with SIMD
(`src/raster/simd.hpp`: SSE2 on x86, NEON on ARM, a scalar fallback
otherwise). For each group it

1. computes the three edge functions for all four pixels,
2. ORs them together and takes the sign bits → a 4-bit coverage mask,
3. interpolates depth for the group and compares against the depth buffer →
   a second mask,
4. and only then, for pixels that survive both, runs the (scalar) shading
   code.

Most of the pixels a triangle's bounding box touches are rejected in step 2
or 3, so the expensive part runs rarely.

### Depth

Depth is NDC `z` remapped to `[0, 1]`. It is interpolated **linearly in screen
space** with the barycentric weights `λi = Ei / area` - no perspective
correction needed, because projection maps planes to planes: a flat triangle
is still flat in (screen x, screen y, z/w).

### Perspective-correct interpolation

Everything else *does* need correcting. The weights `λ` describe a position on
the projected triangle, and projection is not affine: the far end of a
receding surface is squeezed into fewer pixels than the near end, so the
midpoint on screen is not the midpoint on the surface. Blend UVs with `λ`
directly and textures visibly swim.

The fix rests on one fact: if `u` varies linearly over the triangle in 3D,
then **`u/w` and `1/w` vary linearly in screen space**. So interpolate those
two and divide:

```
        λ0·u0/w0 + λ1·u1/w1 + λ2·u2/w2
 u  =  ────────────────────────────────
           λ0/w0  +  λ1/w1  +  λ2/w2
```

which can be rewritten as an ordinary weighted sum with adjusted weights

```
 bi = (λi / wi) / Σj (λj / wj)        u = b0·u0 + b1·u1 + b2·u2
```

The rasterizer computes `b0, b1, b2` once per pixel and reuses them for every
varying. A unit test renders a strip receding from `w = 1` to `w = 4` and
checks that the value halfway across the screen is 0.2, not 0.5.

## 7. Shading

`FragmentShader` in `src/pipeline/renderer.cpp` · `src/pipeline/shading.hpp`

The fragment shader is a template argument of the rasterizer, so it is inlined
into the pixel loop. For each visible pixel it:

1. samples the base colour texture (trilinear, at the triangle's mip level)
   and discards the pixel if the material is an alpha cut-out below its
   threshold;
2. in the per-pixel modes (Phong, PBR, toon), renormalises the interpolated
   normal, optionally perturbs it with the normal map, and evaluates
   lighting; in Flat / Gouraud mode it just reads the already-lit varyings;
3. adds the emissive colour;
4. hands the result - linear HDR colour - to its output (see
   [section 13](#13-hdr-blending-and-the-two-ways-to-finish-a-pixel)).

**Lighting** by default is Blinn-Phong with a key light, a dim fill light and
a hemisphere ambient term (sky colour from above, ground colour from below).
The lights are attached to the camera, which is why orbiting feels like
turning the model in your hands. The other shading models, other lights,
shadows and environments are covered in sections
[12](#12-lights-and-shading-models) to
[15](#15-environment-lighting-and-skyboxes).

**Normal mapping** rebuilds the tangent frame per pixel: Gram-Schmidt the
interpolated tangent against the normal, cross them for the bitangent (times
the handedness sign), and use the three as the columns of a rotation from
tangent space to world space.

**Wireframe** takes two steps per tile. First the triangles are rasterized as
usual but paint only the background colour - their job is to fill the depth
buffer. Then `rasterizeTileEdges` draws each triangle's edges as lines, depth
tested against that buffer, so lines on hidden surfaces disappear without any
extra work. Two details make it robust:

- A line pixel and the surface beneath it are sampled at slightly different
  points, so on a sloped surface the line would randomly lose the depth test.
  The fill pass therefore pushes each triangle back by its depth slope across
  a pixel or so (classic *polygon offset*).
- Each line is always stepped in the same direction along its major axis, so
  the two triangles that share an edge light exactly the same pixels.

Line pixels are shaded like the surface they belong to, using the same
perspective-correct interpolation with two weights instead of three. Edges
introduced by clipping (the diagonals of the triangle fan) are flagged and
skipped.

## 8. Resolve

`src/pipeline/resolve.cpp`

Terminal "pixels" are enormous, so aliasing is very visible. The cell-based
backends therefore render at 3× the output resolution in each direction and
average each 3×3 block down (in linear light). Alpha is averaged as well and
ends up meaning "fraction of this pixel covered by geometry".

## 9. Terminal output

`src/output/`

A backend turns an image into bytes for the terminal. It reports the image
size it wants for a given cell area, and the shape of its pixels so the
projection can compensate (an ASCII "pixel" is twice as tall as it is wide).

| Backend    | Resolution per cell | How                                                              |
| ---------- | ------------------- | ---------------------------------------------------------------- |
| kitty      | real pixels         | RGB data, base64-encoded in chunked APC sequences                 |
| sixel      | real pixels         | 6-pixel bands, one pass per colour, fixed palette + [dithering](#17-dithering) |
| half-block | 1 × 2               | `▀` with foreground = top pixel, background = bottom pixel        |
| Braille    | 2 × 4 (monochrome)  | one of 256 dot patterns, U+2800 + bit per dot                    |
| ASCII      | 1 × 1               | brightness → glyph from a ramp; colour carries the hue            |

### Choosing one

`src/app/capabilities.cpp` asks the terminal what it can do rather than
guessing from `$TERM`. At start-up it sends a kitty graphics query, two size
queries and finally a *primary device attributes* request. Every terminal
answers the last one, so its reply doubles as "no more replies are coming":
whatever arrived before it is supported. Attribute `4` in that reply means
sixel. Environment variables only decide colour depth and Unicode support.

### Only redrawing what changed

The three cell backends share `src/output/cell_grid.cpp`. Each frame is first
encoded into a grid of `{glyph, foreground, background}` cells, then compared
with the grid currently on screen. For each changed cell the writer:

- moves the cursor only if it is not already there (after writing a cell the
  cursor is on the next one, so runs of changes cost nothing; gaps on the same
  row use a short relative move);
- emits a colour code only if the colour differs from the one currently
  selected;
- writes the glyph.

For a model spinning on a dark background most of the screen never changes,
and a typical frame is a few tens of kilobytes instead of a few hundred.

Each frame is wrapped in synchronized-output markers (`CSI ? 2026 h/l`) so the
terminal presents it atomically, and is sent with a single `write`.

## 10. Input and the frame loop

`src/input/` · `src/app/viewer.cpp`

`Terminal` is the only platform-specific code: termios on POSIX, the console
API on Windows (which, once switched to virtual-terminal mode, speaks the same
escape sequences). `InputParser` is a small incremental state machine over the
byte stream: UTF-8 text, CSI/SS3 key sequences, SGR mouse reports and the
replies to capability queries. It is pure and fully unit-tested.

The viewer sleeps inside `read()` with a timeout: until the next frame is due
while something is animating, or indefinitely-ish when idle. Nothing is
rendered unless the camera, a setting or the window size changed, so an idle
viewer uses no CPU.

## 11. Animation

`src/scene/rig.hpp` · `src/anim/`

A static model is a bag of world-space triangles. An animated one also keeps
what those triangles were made from, in a `Rig`:

- **nodes** - a hierarchy of transforms, each stored as translation, rotation
  (a quaternion) and scale rather than as a matrix, because those three can be
  interpolated and a matrix cannot;
- **skins** - lists of nodes acting as bones;
- **animations** - named clips, each a set of keyframe tracks ("channels").

and every mesh keeps its **rest shape** (vertices in the mesh's own space),
its bone weights and its morph targets in a `MeshRig`. Each frame runs four
small steps, all of them before the renderer starts:

```
 clip + time ─► sampleAnimation ─► Pose ─► computeWorldMatrices ─► applyPose ─► Mesh::vertices
                                    ▲                                             (world space)
                              blendPoses (cross-fade)
```

### Sampling keyframes

`sampleChannel` in `src/anim/pose.cpp`

A channel animates one property of one node: a sorted list of times and the
value at each. To evaluate it, binary-search for the two keyframes around the
current time and compute how far between them it falls, `u` in `[0, 1)`. Then,
depending on the channel's interpolation mode:

| Mode        | Result                                                                    |
| ----------- | ------------------------------------------------------------------------- |
| Step        | the earlier keyframe's value, held                                        |
| Linear      | `lerp(p0, p1, u)` - or `slerp` for rotations                              |
| CubicSpline | a Hermite curve through the two values, shaped by a tangent at each end   |

Rotations get `slerp` because linearly blending two quaternions cuts the
corner through the inside of the unit sphere: the result is shorter than unit
length and moves fastest in the middle. `slerp` walks along the arc at constant
angular speed, after first flipping one end if needed - `q` and `-q` are the
same rotation, and only one of them is the short way round.

A cubic spline keyframe stores three things: the slope arriving, the value,
and the slope leaving. Between two keyframes the curve is

```
p(u) = (2u³ - 3u² + 1)·p0 + (u³ - 2u² + u)·dt·m0 + (-2u³ + 3u²)·p1 + (u³ - u²)·dt·m1
```

with `m0` the earlier key's out-tangent, `m1` the later key's in-tangent and
`dt` the time between them (tangents are per second; the curve runs on `u`).
With both tangents zero this is the familiar ease-in/ease-out `3u² - 2u³`.

Times before the first keyframe or after the last clamp to that keyframe.

### Poses, and blending them

A `Pose` is one local transform per node plus the morph weights. Sampling a
clip starts from the rest pose and overwrites only what the clip animates.
Because a pose is plain data, two of them can be mixed: `blendPoses` lerps
translations, scales and weights and slerps rotations. The `AnimationPlayer`
(`src/anim/player.cpp`) uses that to **cross-fade** when you switch clips: the
old clip keeps playing while the new one fades in along a smoothstep curve, so
a fox goes from walking to running instead of teleporting between the two.

The player is also the timeline - current clip, time, speed (negative runs
backwards), looping, pause, seek. It owns no vertices; it only answers "what
is the pose right now?".

### From pose to world matrices

Each node's transform is relative to its parent, so

```
world(node) = world(parent) · local(node)
```

The rig stores its node indices in an order that puts every parent before its
children, which turns the recursion into one flat loop.

### Morph targets

A morph target is a second copy of (some of) a mesh's vertex data stored as
*offsets* from the rest shape. Each target has a weight, and

```
position = rest + Σ weightₖ · targetₖ
```

Normals and tangents are offset the same way. Weights are just another
animated property, which is how a face blends between expressions.

### Skinning

`applyPose` in `src/anim/skinning.cpp`

A skinned vertex does not belong to one node; it follows up to four bones,
each with a weight. For every bone the skin stores an **inverse bind matrix**:
the transform from mesh space into that bone's own space at the moment the
mesh was bound to the skeleton. So

```
jointMatrixⱼ = world(boneⱼ) · inverseBindⱼ
```

reads "express the vertex relative to the bone as it was, then put it where
the bone is now". In the rest pose the two cancel and nothing moves. The
vertex's final position is the weighted blend

```
position = (Σ wⱼ · jointMatrixⱼ) · restPosition
```

This is **linear blend skinning**. Blending matrices is not strictly a valid
way to blend rotations - a joint twisted a long way loses some volume (the
"candy wrapper" effect) - but it is one matrix per vertex, and it is what
nearly every real-time engine does. Normals are transformed by the same
blended matrix and renormalised; rigid (unskinned) meshes use their node's
world matrix and the proper inverse-transpose for normals.

The loop over vertices is split into chunks for the thread pool. Each chunk
writes only its own vertices, so the result is identical for any thread count
(a unit test checks this).

### Where the camera looks

A model's bounding box changes as it moves. Framing the camera on the first
frame would let a jump leave the picture, so at load time
`computeMotionBounds` poses the model at eight evenly spaced times in every
clip and takes the union of the boxes; the camera frames that instead.

### In the viewer

`drawFrame` advances the player by the frame's `dt` and, if that changed the
pose, calls `applyPose` before rendering. Scrubbing (`,` `.`) pauses and seeks;
`m` switches clips with a 0.3 s cross-fade. `--gif` on an animated model
records exactly one loop of the clip so the GIF repeats seamlessly.

## 12. Lights and shading models

`src/scene/light.hpp` · `src/pipeline/shading.hpp`

A `Light` is directional (the sun: parallel rays), point (a bulb) or spot (a
bulb in a cone). For a surface point, each kind answers the same two
questions - *which way is the light?* and *how much arrives?*:

| Kind        | Direction to the light      | Brightness                                              |
| ----------- | --------------------------- | ------------------------------------------------------- |
| Directional | the same everywhere         | constant                                                |
| Point       | towards its position        | `1 / distance²`, optionally faded to zero at a range    |
| Spot        | towards its position        | the same, times a smooth falloff between two cone angles |

Before a frame, `prepareLight` digests every light into a `ShadingLight`:
colour times intensity, the cosines of the cone angles (so the per-pixel test
is a comparison, not an `acos`), its shadow map. If the scene defines no
lights, the renderer supplies the default rig: a key and a fill light attached
to the camera.

Three shading models consume those lights. They differ in the maths between
"light arrives" and "colour leaves":

### Blinn-Phong

Used by flat, Gouraud and Phong shading (which differ only in *where* it is
evaluated: per face, per vertex or per pixel). Diffuse light is Lambert's
cosine law, `max(N·L, 0)`. The highlight is `(N·H)ˢ`, where `H` is the vector
half-way between the light and the eye and the exponent `s` comes from the
material's roughness; a factor of `(s + 8) / 8` keeps the highlight's total
energy constant, so a glossy surface gets a small bright highlight and a rough
one a wide dim one.

### PBR: the microfacet model

glTF materials are defined in terms of a physically based model, and `--shading
pbr` implements it. The idea: a surface is a field of microscopic perfect
mirrors, tilted at random. Light from `L` reaches the eye at `V` only off
facets that face exactly half-way between the two, so the specular reflection
is the product of three questions (Cook-Torrance):

```
              D · G · F             D   how many facets face H?           (GGX)
specular = ───────────────          G   how many are not hidden behind
            4 (N·L) (N·V)               their neighbours?                 (Smith)
                                    F   how much does a facet reflect at
                                        this angle?                       (Fresnel)
```

- **D** (`distributionGgx`) is controlled by roughness: small values give a
  tall, narrow peak. A unit test integrates it over the hemisphere and checks
  the result is 1 - facets cannot add up to more surface than there is.
- **F** (`fresnelSchlick`) rises from `f0` face-on to 100% at a grazing angle.
  `f0` is 4% for anything that is not a metal, and the base colour for metals.
- What is not reflected enters the surface. In a non-metal it scatters and
  re-emerges as the diffuse colour; in a metal it is absorbed, so metals have
  no diffuse term at all. `metallic` blends between the two.

Nothing in that can create light, which is why PBR materials hold up under any
lighting without per-scene tweaking. (One known loss: light that bounces
between facets more than once is ignored, so very rough metals come out a
little dark. A unit test pins down how much.)

### Toon

Cel shading is Blinn-Phong with the smoothness removed: `N·L` is snapped to
three flat tones and the highlight to on-or-off, each through a `softStep` a
few hundredths wide so the boundaries are anti-aliased rather than jagged.
Combined with [outlines](#16-post-processing) it gives the look of ink and
flat paint.

### A note on the inner loop

`blinnPhong` is forced inline into the fragment shader and queues its `pow`
calls for a tiny second loop; PBR and toon are kept *out* of line. Both
decisions are about the same thing - the shader runs for every pixel, and the
default path through it must stay small enough for the compiler to keep its
working values in registers. The comments in `shading.hpp` have the details.

## 13. HDR, blending and the two ways to finish a pixel

`src/core/framebuffer.hpp` · `Renderer::render` in `src/pipeline/renderer.cpp`

Shading produces **linear, unbounded** light values: a lamp can be a hundred
times brighter than a white wall. A display cannot show that, so the last step
of every frame is *tone mapping* - compressing the range into `[0, 1]` - and
encoding as 8-bit sRGB.

Where that step happens depends on what the frame needs:

```
                    ┌─ nothing translucent, no whole-frame effects ─► tone map each pixel as it
 shaded colour ─────┤                                                 is shaded, straight into
 (linear HDR)       │                                                 the 8-bit framebuffer
                    └─ otherwise ─► float HDR buffer ─► post-processing ─► tone map ─► 8-bit
```

The float buffer costs four times the memory traffic, so frames that do not
need it skip it; the fragment shader is a template over an `Output` type
(`DirectOutput` or `HdrOutput`) and is compiled once for each route. A unit
test renders the same scene down both and checks they agree.

### Alpha blending

A translucent surface cannot simply win or lose the depth test: what is behind
it must show through. So triangles of `BLEND` materials are held back, sorted
furthest-first, and drawn after everything opaque (the *painter's algorithm*).
They are depth-*tested*, so opaque objects in front still hide them, but do not
*write* depth, so the next layer can blend over them. Each one is combined with
what is already in the pixel by the "over" operator,

```
colour   = source · α + destination · (1 − α)
coverage = α + coverage · (1 − α)
```

in linear light - which is why any translucency sends the frame down the HDR
route. Coverage is tracked the same way so the background can be mixed in at
the very end by however much of the pixel is still uncovered. Sorting by
triangle is not perfect (two intersecting translucent surfaces cannot be
ordered), but it is right for the usual cases: glass in front of things.

### Drawing several models

The renderer takes a list of `DrawItem`s - a model and a transform. The vertex
stage multiplies by the item's matrix (skipped when it is the identity),
normals go through the inverse transpose, and a mirroring transform flips the
sense of the back-face test. Materials from all the models are gathered into
one table per frame so a triangle needs only a single index.

## 14. Shadow mapping

`src/pipeline/shadow.hpp` · `Renderer::renderShadowMap`

A point is in shadow if something sits between it and the light. Rather than
trace a ray per pixel, the scene is rendered once more **from the light**,
keeping only depth. That image - the shadow map - records how far the light's
rays travel in every direction before hitting something.

```
      light                    The map stores the depth of A. B projects to
        \                      the same texel but is further from the light,
         \   A  ← lit          so B is in shadow.
          \  │
           \ B  ← shadowed
```

The shadow pass reuses the whole pipeline with a depth-only shader, through an
orthographic projection for a directional light and a perspective one matching
the cone for a spot light. Then, for each shaded pixel, the world position is
transformed into the light's clip space and its depth compared with the map's.

Three details make it look right:

- **Bias.** The map's texels are coarser than screen pixels, so a surface
  compares against a slightly different depth than its own and shadows itself
  in stripes ("acne"). The fix is to treat the point as a little nearer the
  light: a slope-scaled offset when drawing the map plus a constant when
  reading it. The constant is kept in world units and converted per sample,
  because in a perspective map equal distances are *not* equal depth steps.
- **Soft edges (PCF).** One comparison per pixel gives a hard, blocky edge.
  *Percentage-closer filtering* makes the comparison at several neighbouring
  texels and averages the **results** (averaging depths would be meaningless).
  A box of `k × k` texels at the exact sub-texel position touches `(k + 1)²`
  texels; the border ones are weighted by how much of them the box covers, so
  the edge moves smoothly rather than in texel-sized steps. `--shadow-softness`
  sets `k`.
- **Fitting.** The map is aimed at the bounding sphere of the scene, so its
  resolution is spent where there is something to shadow.

Shadows are evaluated in the per-pixel modes only (Phong, PBR, toon); a shadow
looked up once per vertex or face would be too coarse to recognise.

## 15. Environment lighting and skyboxes

`src/scene/environment.hpp` · `environment.cpp`

Lights are points; the environment is everything else. A surface picks it up
in two ways: a matte one averages all the light in the hemisphere it faces
(*irradiance*), a shiny one mirrors a single direction, blurred by roughness.

The default environment is two colours - sky above, ground below - blended by
the normal's height. `--env` replaces it with an HDR panorama
(equirectangular: longitude across, latitude down), or with one of the
procedural skies built in. This is **image-based lighting**.

**Diffuse.** Integrating the whole panorama for every pixel is out of the
question. But irradiance varies so smoothly with the normal that nine
*spherical harmonic* coefficients per channel - the sphere's equivalent of the
first terms of a Fourier series - reproduce it to about 1%. They are computed
once at load time (each texel weighted by the solid angle it covers, which
shrinks towards the poles), and evaluating them is a nine-term polynomial in
the normal. A unit test checks that a uniformly white panorama yields exactly
white.

**Specular.** A rough surface sees a blurred reflection. The panorama's mip
chain already contains progressively blurred copies, so the lookup picks a
level from the roughness (two levels per halving of roughness, since blur
grows with its square). How much of that light is reflected comes from
`environmentBrdf`, a curve fit to the usual precomputed lookup table. This is
an approximation of proper prefiltering, chosen because it costs nothing to
set up.

**Skybox.** Every pixel is a ray from the eye. Where nothing was drawn, the
ray's direction is looked up in the panorama. It is ordinary light - bright
suns bloom, and it is tone mapped with everything else.

## 16. Post-processing

`src/post/`

After rasterization there are two images: HDR colour and depth. Each pass
reads those and rewrites the colour.

### Rebuilding geometry from depth

Several passes need the position and normal of each pixel. Neither is stored:
both are recovered from depth. Position is the projection run backwards.
The normal is the cross product of the position's change across the screen -
taking care at edges, where a neighbour may belong to a different surface, to
use the side on which the surface continues in a straight line. "Straight" is
judged on `1/z`, which (as in perspective-correct interpolation) is what
varies linearly across the screen for a flat surface.

### SSAO

Ambient light arrives from all around, so the bottom of a crease gets less of
it than open ground. Screen-space ambient occlusion estimates that from depth
alone: scatter a few sample points in the hemisphere above the surface,
project each back to the screen, and ask the depth buffer whether something
nearer the camera was drawn there. The fraction of samples buried in geometry
is the occlusion. Eight samples would be noisy, so each pixel rotates the
pattern by an angle from a repeating 4×4 table, and a 4×4 blur afterwards
averages exactly one table's worth.

### Outlines

A line is drawn wherever, between a pixel and its neighbour, coverage changes
(the silhouette), depth jumps (one object in front of another) or the normal
turns sharply (a crease). Depth is tested by the *second* difference of `1/z`,
which is zero across any flat surface however steeply it recedes, so a floor
seen at a glancing angle is not inked all over.

### Bloom

A lens scatters a little light from a very bright source over its
surroundings. Bloom keeps only what exceeds a threshold, blurs it, and adds it
back. The blur is done by halving the image repeatedly - each halving doubles
the reach of a small filter - then adding the levels back together, smallest
first.

### Tone mapping

Four curves: ACES (the default; contrasty), Reinhard (`x / (1 + x)`), Hable's
filmic curve and plain clamping. Exposure, in photographic stops, scales the
light first. A flat background colour is added *after* the curve so that it
comes out exactly as specified.

### FXAA

Supersampling fixes aliasing by brute force. FXAA is the cheap alternative for
the pixel backends, applied to the finished image: find pixels on a
high-contrast edge, work out which way the edge runs from the neighbours'
brightness, and blur *along* it only.

## 17. Dithering

`src/output/dither.hpp`

Most output modes cannot show the colour that was computed: sixel has a
253-colour palette, a 256-colour terminal a fixed cube, ASCII a dozen glyph
densities, Braille two states per dot. Rounding to the nearest available value
turns gradients into bands. Dithering trades the bands for fine noise by
rounding neighbouring pixels in different directions, so their *average* is
right.

| Method          | How                                                              | Good for  |
| --------------- | ---------------------------------------------------------------- | --------- |
| Ordered         | add a position-dependent threshold (a 4×4 Bayer matrix) first    | animation: the pattern holds still |
| Floyd-Steinberg | push each pixel's rounding error onto its unvisited neighbours   | stills: sharper, but the noise crawls when things move |

`--dither auto` picks per backend:

| Backend    | Auto                                                                       |
| ---------- | -------------------------------------------------------------------------- |
| kitty      | none - 24-bit pixels                                                       |
| sixel      | ordered while animating, Floyd-Steinberg for `--print`                     |
| half-block | the same, on 256-colour terminals only                                     |
| ASCII      | none - twelve glyphs are too few for noise to read as shade (opt in)       |
| Braille    | none - dots mark the wireframe. Asking for a dither mode changes the job: shaded surfaces as a one-bit halftone |

Background pixels never take part, so the empty area around a model stays
perfectly flat.

## 18. The BVH: culling and picking

`src/accel/bvh.hpp` · `bvh.cpp` · `model_accel.hpp`

Two questions the rasterizer cannot answer cheaply: *which triangles can the
camera see?* and *which triangle is under the mouse?* Both are searches, and a
**bounding volume hierarchy** turns them from "test every triangle" into "test
a few boxes". Triangles are grouped into a tree of axis-aligned boxes: the
root holds them all, each child holds part, down to leaves of at most four. If
a box is missed, everything in it is dismissed with one test.

### Building it

Start with all triangles in one node and split recursively. Where to split
decides how good the tree is. The **surface area heuristic** scores a split by

```
area(left box) · count(left)  +  area(right box) · count(right)
```

because the chance of a random ray entering a box is proportional to its
surface area, and the work once inside is proportional to its triangle count.
Trying every split would be slow, so each node sorts its triangles into twelve
bins along each axis by centroid and only the eleven boundaries between bins
are scored. Children are stored side by side, so a node needs one index.

### Frustum culling

The six planes of the view volume come straight out of the view-projection
matrix: a point is visible when `-w ≤ x ≤ w`, and "`w + x ≥ 0`" is the dot
product of the point with (row 3 + row 0) of the matrix. Given the
model-view-projection matrix, the planes arrive in model space, where the BVH
lives. A box is tested against each plane using only its corner furthest
along the plane's normal.

- A mesh whose box is **outside** is skipped before any vertex is touched.
- One that is **inside** needs no further tests.
- One that **straddles** the edge walks its BVH, which marks the triangles
  that could be visible in a bitmap *indexed in the mesh's own order*. The
  primitive setup loop then skips unmarked triangles - still visiting the
  rest in their original order, so the picture is bit-for-bit the same with
  culling as without. (A unit test renders both ways and compares.)

The saving is real but modest, because the per-triangle clip test that would
have rejected those triangles anyway is cheap; the big win is the meshes that
never reach the vertex stage. Shadow passes cull against the light's frustum
the same way.

### Picking

A click becomes a ray: the clicked point on the near plane and on the far
plane are run backwards through the projection, and the ray joins them. For
each model the ray is moved into model space (one matrix inverse, instead of
transforming every vertex out), tested against each mesh's box with the
*slab* method, and then walked down that mesh's BVH. Triangles are tested
with Möller-Trumbore - write the hit point as `origin + t·direction` and as
`a + u·(b − a) + v·(c − a)`, and solve the 3×3 system. Once a hit is found
the search distance shrinks to it, so most of the tree is never visited.

For an animated model the boxes are refreshed every frame (cheap) but the
trees only when a click actually needs them.

## 19. Level of detail

`src/accel/simplify.hpp` · `simplify.cpp`

A 100,000-triangle model in a 160×100 terminal has several triangles per
pixel. Level of detail keeps a few progressively coarser versions of each mesh
and draws the coarsest one whose error would be invisible.

### Simplifying a mesh

The coarser versions are made by **edge collapse**: slide one end of an edge
onto the other and the two triangles that shared it vanish. Repeat, always
taking the collapse that changes the shape least.

"Least" is measured with **quadric error metrics**. Every vertex starts out
lying on the planes of the triangles around it; after some collapses it stands
in for several original vertices, and the damage is the sum of its squared
distances to all the planes it has inherited. Squared distance to a plane is
a quadratic in the point, and a sum of quadratics is a quadratic - ten numbers
per vertex, however many planes went in. Merging two vertices adds their
quadrics; evaluating the sum at the surviving position is the cost.

Three safeguards:

- **Borders.** Sliding along the surface off an open edge costs nothing by
  the plane measure, so each border edge adds a plane standing on it at right
  angles to its triangle.
- **Seams.** Where the texture mapping has a seam, a mesh has several
  vertices at one position. They are grouped into a *site* and move together.
  A collapse is refused if it would land two of a site's vertices on the same
  target vertex - that would stitch the two sides of the seam together and
  smear the texture across it.
- **Flips.** A collapse that would turn a neighbouring triangle over is
  refused.

A vertex is always moved onto an *existing* vertex, so a simplified mesh uses
a subset of the original vertices: **a level of detail is just a shorter index
list.** Skinning and morph targets, which work on vertices, apply to every
level for free.

Candidates sit in a priority queue. Rather than remove entries when a collapse
invalidates them, every site carries a version number and stale entries are
recognised and dropped when they reach the top.

### Choosing a level

Each level records its error: the largest root-mean-square distance, in the
mesh's units, between a moved vertex and the planes it stands for. Per frame
that is projected to pixels at the nearest point of the mesh,

```
error in pixels = error · (framebuffer height / 2) · cot(fov / 2) / distance
```

and the coarsest level under the tolerance is drawn. The choice is made once
per frame and shared with the shadow passes, so a shadow is cast by the same
triangles that receive it. Wireframe mode always shows the original mesh.

The viewer uses a tolerance of half an output pixel by default; exports use
the full mesh unless `--lod` is given. For a large static model the levels are
built on a background thread so the first frame does not wait.

## 20. Holding a frame rate

`src/app/quality.hpp` · `Viewer::drawFrame`

While something is moving, a frame is on screen for a few hundredths of a
second and smoothness matters more than detail. The `QualityGovernor` compares
each frame's time with the budget for `--target-fps` and moves along a ladder
of cheaper settings, least noticeable first:

```
full quality → looser level of detail → fewer supersamples → lower resolution, enlarged
```

Six frames in a row over budget drop a rung; getting one back takes forty-five
in a row with half the budget to spare. The asymmetry is deliberate: a rung
restored too eagerly would be dropped again at once and the picture would
flicker between the two. Rungs that would change nothing - a pixel backend has
no supersampling to give up - are skipped.

As soon as the motion stops, one more frame is drawn at full quality, so the
reduced settings are only ever seen in passing.

## 21. Benchmarks and performance regressions

`src/app/benchmark.cpp` · `scripts/perf-check.sh` · `.github/workflows/ci.yml`

`termesh --benchmark` runs a fixed set of scenes built from the demo models,
each exercising one part of the engine along a fixed camera path. Nothing
depends on files, the clock or the terminal, so two runs do exactly the same
work. Each scene reports:

- **counters** - triangles drawn, pixels covered, bytes of terminal output and
  a checksum of everything produced. These are exactly reproducible (and
  independent of the thread count); a change means the engine is doing
  different work.
- **times** - milliseconds per frame. Real, but noisy.

Timing on a shared CI machine cannot detect a 5% slowdown: the same binary
varies by more than that from run to run. So the CI job builds the commit and
its parent, and runs every scene of both under valgrind's *cachegrind*, which
simulates the program and counts the **instructions executed**. That number
depends only on the code and its input, and repeats to within a fraction of a
percent. Comparing against the parent commit, built in the same job with the
same compiler, means there is no stored baseline to go stale. A scene that
executes more than 5% more instructions fails the build; a commit that is
slower on purpose says so with `[perf-ok]` in its message.

## 22. Scenes and scene files

`src/stage/scene.hpp` · `scene_file.cpp` · `src/core/json.cpp` · [docs/scenes.md](docs/scenes.md)

A `Scene` is a list of objects - each a `Model` with a transform, its
acceleration data and (if animated) its `AnimationPlayer` - plus lights, an
environment and a camera. Every frame it hands the renderer a draw list: one
`DrawItem` per visible object. Viewing a single file is just a scene with one
object in it, so there is one code path for everything.

Objects are held by pointer because other things point into them (a player
keeps the address of its model's rig); adding an object must never move the
ones already there.

### The JSON reader

Scene files are JSON, read by a small recursive-descent parser: one function
per kind of value, each looking at the next character to decide what it is
reading and calling the others for whatever is nested inside. The grammar is
small enough that the code *is* the grammar. Errors carry a line and column;
nesting is capped so that a malicious file cannot overflow the stack; and two
liberties are taken for files written by hand - `//` comments and trailing
commas.

A parsed value answers lookups for missing members with a null value instead
of an error, so reading an optional setting is one chained expression:
`json["camera"]["path"]["keys"]`.

### Reading a scene

The loader is a long list of "this field, if present, must look like that".
The first thing that does not is reported with the path to it -
`models[1].position: expected three numbers` - and reading stops. Harmless
oddities (an `animation` on a model that has none, a section whose name is
not recognised) become warnings.

A scene file's `render` block is not applied by the loader at all. The
application slips it in *underneath* the command line: the options are parsed
once to find the scene file, the file's settings become the new defaults, and
the command line is parsed again on top. Whatever the user typed wins, and no
option needs to remember whether it was set explicitly.

### Placing models

A transform in a scene file is position, rotation in degrees about x, y and
z, and scale, combined as `translate · yaw · pitch · roll · scale`. Models
named together on the command line have no such placement, so `arrangeInRow`
scales each to the same bounding radius, stands it on the ground and lines
them up.

## 23. Camera paths

`src/stage/camera_path.hpp` · `camera_path.cpp`

A camera path is keyframe animation applied to the viewer rather than the
viewed: a list of (time, position, target, field of view) and a way to fill
in between. "Smooth" interpolation is a **Catmull-Rom spline**: the curve from
one key to the next is shaped by the keys before and after, so that it passes
exactly through every key and leaves each one heading parallel to the line
between its neighbours. That is what makes a camera glide through its
keyframes instead of stopping at each.

```
          p1 ●━━━━━━━━━● p2            the segment drawn is p1 → p2;
           ╱             ╲             p0 and p3 only bend it
      p0 ●                 ● p3
```

A looping path wraps its neighbours around, so the join is as smooth as
anywhere else; an open path repeats its end keys, so the camera eases in and
out. A turntable is simply a looping path of sixteen keys on a circle.

In the viewer the path plays until the user steers. `leavePath` then builds
an orbit camera that shows *exactly* the same view (`orbitFromPose` recovers
yaw, pitch and distance from the position and target), so control changes
hands without a jump.

## 24. The inspector

`src/app/inspector.hpp` · `inspector.cpp` · `Viewer::drawInspector`

The inspector is split in two so that most of it needs no terminal.
`Inspector` turns a scene into rows - object, group, mesh, material, node,
light - keeps track of which headings are open and which row is selected, and
renders a frame as lines of plain text of an exact width (counting
characters, not bytes: names are UTF-8). The viewer only places those lines,
colours them, and - like the cell backends - sends just the ones that changed.

Rows are identified by a key string (`o1/mesh3`) rather than by index, so the
selection and the set of open headings survive the outline being rebuilt.
Selecting a row tells the renderer which mesh (or whole model) to tint, and a
click in the picture runs the [picking](#18-the-bvh-culling-and-picking) ray
and opens the outline at whatever it hit.

## 25. More formats: STL, PLY, point clouds

`src/scene/stl_loader.cpp` · `ply_loader.cpp` · `loader.cpp` · `assimp_loader.cpp`

### The loader table

`loadModel` looks the file's extension up in a table of loaders. The built-in
formats are in it from the start; `registerModelLoader` adds more, and
`registerFallbackModelLoader` names one to try for anything unclaimed. The
table is built on first use rather than at program start, so code that
registers a loader from a static initialiser finds it ready.

### STL

The format of 3D printing: nothing but triangles. It has two encodings and no
reliable way to tell them apart by looking at the start, because a binary
file's 80-byte header is free text and often begins with `solid` - which is
how every ASCII file begins. The test that works is arithmetic: a binary file
is exactly `84 + 50 × count` bytes long.

STL has no shared vertices, so each triangle becomes three vertices carrying
the triangle's own normal - recomputed from its corners, since the normal
stored in the file is redundant and frequently wrong or zero.

### PLY

The format of 3D scanners. A PLY file describes itself: its header lists
*elements* (vertex, face, ...) with a count and typed *properties* each, and
the data follows in that order as text or as binary of either byte order. So
the reader has no layout to assume. It builds a table from the header and
walks the data with it, collecting the properties it knows by name (`x y z`,
`nx ny nz`, `red green blue`, texture coordinates, `vertex_indices`) and
stepping over the rest - including whole elements it has no use for, and the
dozens of extra per-point properties a Gaussian-splatting file carries (from
which it takes just the colour).

Counts in the header are checked against the size of the file before
anything is allocated, so a damaged or hostile header cannot ask for
gigabytes.

### Vertex colours

Scans carry their colour per vertex, not in a texture. `Mesh::colors` holds
one linear RGB colour per vertex, which multiplies the material's base colour.
Getting it to the fragment shader costs nothing for models that have none:
per pixel it rides in the varyings the tangent would otherwise use (a mesh
needs its tangent only for normal mapping, and where both are wanted the
normal map wins), and vertex-lit frames grow three varyings wider only when
something in them is coloured.

### Point clouds and splats

A mesh with vertices but no triangles is a point cloud, and `--points` draws
any mesh as one. Each point becomes a **splat**: a small square facing the
camera, built directly in clip space,

```
corner = centre + (±radius · Px, ±radius · Py, 0, 0)        Px, Py: the projection's x and y scale
```

Adding the same clip-space offset before the perspective divide makes nearer
points come out larger, as they should. The corners carry (−1, −1) … (1, 1)
where texture coordinates would be and the fragment shader keeps only the
disc inside the unit circle. From there a splat is two ordinary triangles:
clipped, binned, depth-tested and shaded like any others, casting and
receiving shadows.

- **Size.** The radius is the cloud's typical point spacing, estimated when
  it is loaded by timing a few hundred points against a sample of the rest
  (and scaling for the sample being sparser than the whole). A splat is
  never less than three quarters of a pixel in radius - a disc that size
  always contains a pixel centre - so distant points do not flicker.
- **Shading.** With a normal, a point is lit like a surface. Without one it
  is treated as facing the viewer; ambient occlusion (`--ssao`) then does
  most of the work of showing the shape.
- **Thinning.** A cloud so far away that its splats are sub-pixel is drawn
  with every k-th point at k times the area. Clouds are shuffled when loaded
  so that any such subset is an even sample.
- **Textured meshes as points** take each point's colour from the texture at
  its own coordinates.

### The Assimp backend

termesh's own loaders are written out in full because reading a format is
part of what the project is for. `-DTERMESH_ASSIMP=ON` makes the opposite
bargain: one dependency, the Open Asset Import Library, in exchange for FBX,
COLLADA, 3DS, Blender and several dozen more. It registers as the fallback
loader, so it is only asked about extensions nothing built in can read.
Assimp is told to flatten the scene (apply every node's transform, discard
the hierarchy), which is what makes a file in any of those formats look like
a termesh `Model`; the price is that animation does not come through.

## 26. Previews for file managers

`src/app/preview.{hpp,cpp}` · `renderPreview` in `src/app/main.cpp` · [docs/previews.md](docs/previews.md)

`termesh --preview file` is `--print` reshaped for one caller: a file
manager, which runs it every time the cursor lands on a model and shows
whatever comes out in a pane. Three things follow from that.

**It cannot ask the terminal anything.** The normal start-up sends the
terminal a few queries and reads the replies to learn what it supports. A
previewer's standard output is a pipe into the file manager and its standard
input is not the keyboard, so a query would wait for an answer that never
comes. Preview mode never probes: it draws half-blocks (text that survives
being captured and re-laid-out by another program) at a size given by
`--cols` and `--rows`, unless told otherwise.

**It has to be instant the second time.** Moving through a directory asks
for the same preview again and again. The finished text is stored in a cache
directory under a key made of everything that could change it: the file's
absolute path, size and modification time, and the options that affect the
picture. A hit is one file read - no model is loaded at all. Entries are
written to a temporary name and renamed into place, so two previews racing
for the same file cannot leave half of one behind, and the cache is trimmed
to a fixed number of entries, oldest first.

**It must never hang on something enormous.** A file over the size limit
is described in a line of text rather than loaded.

## 27. libtermesh: the C API

`include/termesh.h` · `src/capi/capi.cpp` · `examples/c/render.c` · [docs/library.md](docs/library.md)

Everything above is also available to other programs as a library. Its
interface is C rather than C++ for the usual reasons: C has a stable binary
interface on every platform, and every language can call it.

```
termesh_scene      what to draw    objects, lights, environment, animation state
termesh_renderer   how to draw     a thread pool and the renderer's scratch memory
termesh_camera     from where      position, target, field of view  (a plain struct)
termesh_settings   in what style   shading, shadows, effects        (a plain struct)
```

`termesh_render_pixels` fills a buffer with RGBA; `termesh_render_text`
returns what one of the [output backends](#9-terminal-output) would send to a
terminal. The implementation is a few hundred lines because it does nothing
but translate: each function checks its arguments, calls the C++ class that
does the work, and translates the result back.

What makes the interface *stable* is a short list of rules:

- **Opaque handles.** Clients hold `termesh_scene*` and never see inside, so
  the classes behind it can change freely.
- **No exceptions cross the boundary.** Every function body runs inside
  `guard()`, which catches anything thrown, stores the message per thread
  for `termesh_last_error()`, and returns the function's failure value
  (`NULL` or a negative number).
- **Structs carry their size.** A caller-filled struct begins with
  `struct_size`, set by its `_init` function. The library copies only that
  many bytes over a default-initialised struct of its own, so a program
  compiled against an older, shorter struct keeps working when fields are
  appended - and a struct that was never initialised is refused instead of
  being read as garbage.
- **Memory is freed by whoever allocated it.** Text comes back from the
  library's allocator and goes back through `termesh_free`; a program and
  a library can be using different heaps (on Windows they often are).
- **Only the API is exported.** Everything is compiled with hidden
  visibility; `TERMESH_API` marks the few dozen functions that are the
  library's entire surface.

The build compiles the engine once into object files and links them three
ways - into the `termesh` program, the tests, and `libtermesh` - so the
library is one self-contained file. It installs with a `termesh.pc` for
pkg-config and a CMake package (`find_package(termesh)` gives the target
`termesh::termesh`). The C API has its own test program, which links only
the library and includes only `termesh.h`: it tests what clients get.

## Performance notes

- **Work is proportional to visible pixels, not triangles.** The per-triangle
  cost (transform, outcode test, setup) is a few dozen nanoseconds; shading
  dominates. Run `termesh model.glb --bench 300` to see the split for a model,
  or `termesh --benchmark` for the built-in suite.
- **Threads.** Vertex transform, triangle setup, tile rasterization and the
  resolve all go through one `parallelFor` (`src/core/thread_pool.cpp`), which
  hands out iterations from a shared atomic counter so uneven tiles balance
  themselves.
- **No per-frame allocation.** Clip vertices, triangle lists and tile bins
  are kept between frames.
- **SIMD is used where it pays**: coverage and depth rejection, which run for
  every pixel in every bounding box, and the tone curve, where the three
  colour channels go through identical arithmetic. Shading stays scalar and
  readable - and on GCC the auto-vectoriser is told to leave the fragment
  shader alone (`TERMESH_NO_SLP` in `shading.hpp`): packing pairs of a
  three-component vector into SSE registers costs more in shuffles than it
  saves, about 10% of the frame.
- **The shader is compiled more than once.** It is a template over where its
  output goes (straight to 8 bits, or to the HDR buffer) and over whether the
  frame uses the optional per-pixel features (splats, vertex colours,
  translucency, the selection highlight). A frame that needs none of them
  runs a version in which those tests do not exist.
- **Features cost nothing until used.** A frame without translucency or
  whole-frame effects never touches the HDR buffer; shadow maps are only
  drawn for lights that cast them; the two-colour environment is inlined.
- **The terminal is usually the bottleneck**, which is what the cell diffing
  and the pixel budget on the kitty / sixel backends are for.

## Testing

`tests/` uses doctest. The parts with sharp edges have the most coverage:

- `test_math.cpp` - vectors, matrices, quaternions against known identities.
- `test_clip.cpp` - outcodes, every clipping case, linear interpolation at
  the cut, winding preservation, identical intersections on shared edges.
- `test_raster.cpp` - the fill rule (random quads split along either diagonal
  must cover each pixel exactly once), depth ordering, perspective
  correction, tile independence, and 1-thread vs N-thread equality.
- `test_scene.cpp` - textures, mip-mapping, the camera, and both loaders
  against small files written by the test.
- `test_anim.cpp` - keyframe interpolation (step, linear, cubic spline, the
  short arc for rotations), hierarchy accumulation, pose blending, skinning
  and morph targets against hand-computed positions, the timeline (looping,
  reverse, seeking, cross-fades), and skinned / morphed glTF files built by
  the test.
- `test_render.cpp` - light falloff and cones, the PBR terms (GGX integrates
  to one, Fresnel limits, energy bounds), shadow comparison / bias / soft
  edges, shadows cast in real scenes by directional and spot lights, alpha
  blending against hand-computed colours and in any submission order, the
  direct and HDR routes agreeing, and draw lists with transforms.
- `test_post.cpp` - every tone curve monotonic and bounded, bloom spreading
  only what exceeds the threshold, SSAO darkening a pit but not a flat wall,
  outlines on silhouettes / depth steps but not on steep planes, FXAA
  leaving straight edges alone, and environments: spherical-harmonic
  irradiance, roughness blur, rotation, `.hdr` loading, bad input.
- `test_accel.cpp` - ray / triangle and ray / box tests, frustum
  classification, the BVH's structure, rays agreeing with brute force,
  culling never dropping a visible triangle, identical pictures with and
  without it, and picking through transforms.
- `test_lod.cpp` - every level a valid smaller mesh, borders and texture seams
  preserved, level selection by distance and resolution, levels following an
  animation, and the frame-rate governor's ladder.
- `test_formats.cpp` - STL in both encodings (and the binary file that
  starts with "solid"), PLY as text and in both byte orders, point clouds,
  damaged and hostile files, the loader table, vertex colours in every
  shading mode, and splats: round, sized by distance, never vanishing,
  thinned when far away.
- `test_stage.cpp` - the JSON reader (every type, escapes, errors with
  positions, deep nesting), camera paths (through the keys, smooth across a
  loop's join, turntables), scenes, and scene files: a complete one, each
  kind of mistake, and the examples that ship in `examples/`.
- `test_inspector.cpp` - the outline's rows, folding, selection in both
  directions, details of every kind of row, and frames of exactly the
  requested size.
- `test_output.cpp` - cell diffing, each backend's encoding (the kitty and GIF
  streams are decoded again and compared), palette quantisation, and
  dithering: averages preserved, backgrounds untouched, each backend's
  behaviour in each mode.
- `test_input.cpp` - the input parser, including sequences split across reads.

CI builds and runs all of it on Linux (GCC, Clang, and with SIMD disabled),
macOS on Apple Silicon (the NEON path) and Windows (MSVC), plus an
AddressSanitizer / UBSan job and the [performance
comparison](#21-benchmarks-and-performance-regressions).
