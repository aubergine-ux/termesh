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
| `src/core/`      | Framebuffer, images, textures (mipmaps, filtering), colour space, thread pool.   |
| `src/scene/`     | `Model` / `Mesh` / `Material`, rig data, the orbit camera, OBJ and glTF loaders, demos. |
| `src/anim/`      | Keyframe sampling, pose blending, skinning and morph targets, the timeline.      |
| `src/pipeline/`  | The renderer: vertex stage, clipping, triangle setup, shading models, shadows, resolve. |
| `src/post/`      | Post-processing: sky, SSAO, outlines, bloom, tone mapping, FXAA.                 |
| `src/raster/`    | The triangle and line rasterizer and the SIMD wrappers it uses.                  |
| `src/output/`    | Terminal backends and the cell-diffing writer.                                   |
| `src/input/`     | Raw terminal access (POSIX / Windows) and the input parser.                      |
| `src/export/`    | Off-screen rendering, PNG and GIF writing.                                       |
| `src/app/`       | Command line, capability detection, the interactive viewer, `main`.              |
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

## Performance notes

- **Work is proportional to visible pixels, not triangles.** The per-triangle
  cost (transform, outcode test, setup) is a few dozen nanoseconds; shading
  dominates. Run `termesh model.glb --bench 300` to see the split.
- **Threads.** Vertex transform, triangle setup, tile rasterization and the
  resolve all go through one `parallelFor` (`src/core/thread_pool.cpp`), which
  hands out iterations from a shared atomic counter so uneven tiles balance
  themselves.
- **No per-frame allocation.** Clip vertices, triangle lists and tile bins
  are kept between frames.
- **SIMD is used where it pays**: coverage and depth rejection, which run for
  every pixel in every bounding box. Shading stays scalar and readable - and
  on GCC the auto-vectoriser is told to leave the fragment shader alone
  (`-fno-tree-slp-vectorize` for `renderer.cpp`): packing pairs of a
  three-component vector into SSE registers costs more in shuffles than it
  saves, about 10% of the frame.
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
- `test_output.cpp` - cell diffing, each backend's encoding (the kitty and GIF
  streams are decoded again and compared), palette quantisation, and
  dithering: averages preserved, backgrounds untouched, each backend's
  behaviour in each mode.
- `test_input.cpp` - the input parser, including sequences split across reads.

CI builds and runs all of it on Linux (GCC, Clang, and with SIMD disabled),
macOS on Apple Silicon (the NEON path) and Windows (MSVC), plus an
AddressSanitizer / UBSan job.
