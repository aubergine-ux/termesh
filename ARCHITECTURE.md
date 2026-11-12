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
            └────────────────────────────────────────────────────────────────┘
                          │
                       resolve (average supersamples)
                          │
              output backend (kitty · sixel · half-block · Braille · ASCII)
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
| `src/pipeline/`  | The renderer: vertex stage, clipping, triangle setup, lighting, resolve.         |
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
2. in Phong mode, renormalises the interpolated normal, optionally perturbs
   it with the normal map, and evaluates lighting; in Flat / Gouraud mode it
   just reads the already-lit varyings;
3. combines: `albedo·(1 − metallic)·diffuse + specularColour·specular + emissive`;
4. tone-maps and encodes to sRGB.

**Lighting** is Blinn-Phong with a key light, a dim fill light and a
hemisphere ambient term (sky colour from above, ground colour from below).
glTF roughness is mapped to a specular exponent, and the highlight is
energy-normalised so glossy surfaces get small bright highlights and rough
ones wide dim ones. The lights are attached to the camera, which is why
orbiting feels like turning the model in your hands.

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
| sixel      | real pixels         | 6-pixel bands, one pass per colour, fixed palette + dithering    |
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
  every pixel in every bounding box. Shading stays scalar and readable.
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
- `test_output.cpp` - cell diffing, each backend's encoding (the kitty and GIF
  streams are decoded again and compared), palette quantisation.
- `test_input.cpp` - the input parser, including sequences split across reads.

CI builds and runs all of it on Linux (GCC, Clang, and with SIMD disabled),
macOS on Apple Silicon (the NEON path) and Windows (MSVC), plus an
AddressSanitizer / UBSan job.
