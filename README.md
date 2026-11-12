<div align="center">

# termesh

**A 3D model viewer that lives in your terminal.**

Textured, lit, depth-buffered 3D - drawn by a software rasterizer written from scratch in C++20.<br>
No OpenGL. No GPU. Just triangles, maths and escape codes.

[![CI](https://github.com/aubergine-ux/termesh/actions/workflows/ci.yml/badge.svg)](https://github.com/aubergine-ux/termesh/actions/workflows/ci.yml)
[![License: MIT](https://img.shields.io/badge/license-MIT-3b82f6.svg)](LICENSE)
![C++20](https://img.shields.io/badge/C%2B%2B-20-00599c.svg?logo=cplusplus&logoColor=white)
![Platforms](https://img.shields.io/badge/platforms-Linux%20%7C%20macOS%20%7C%20Windows-4b5563.svg)

<br>

<img src="docs/media/hero.gif" alt="A textured flight helmet model spinning in a terminal, drawn with half-block characters" width="748">

<sub>
<code>termesh FlightHelmet.gltf</code> - 94,722 triangles, drawn with half-block characters.<br>
This is the program's real terminal output, captured and replayed frame by frame.
</sub>

</div>

<br>

```sh
termesh model.glb
```

That is the whole interface. termesh loads the model, works out what your terminal can display, and gives you
a smooth, shaded, spinning view you can orbit with the mouse.

## Highlights

|     |     |
| --- | --- |
| **A complete pipeline, from scratch** | Vertex transform, homogeneous clipping, perspective divide, back-face culling, tile-based rasterization with a depth buffer, perspective-correct interpolation. |
| **Real shading** | Flat, Gouraud and Phong lighting; diffuse, normal, metallic-roughness and emissive textures; mip-mapping with trilinear filtering; hidden-line wireframe. |
| **Five ways to draw** | Kitty graphics and Sixel for real pixels, half-blocks in 24-bit colour, Braille dots for wireframe, plain ASCII for everything else. The best one is detected automatically. |
| **Fast** | Multithreaded tiles, SIMD coverage and depth tests (SSE2 / NEON), and only the terminal cells that changed are redrawn. A 95k-triangle model renders in under 3 ms. |
| **OBJ and glTF 2.0** | `.obj` + `.mtl`, `.gltf` and `.glb`, with embedded or external textures. |
| **Exports** | Save the current view as a PNG, record a turntable GIF, or print a single frame into your scrollback. |
| **Small and portable** | Linux, macOS and Windows Terminal. Three vendored single-header dependencies. One static binary. |
| **Meant to be read** | The renderer is a few thousand lines of commented C++, with a guided tour in [ARCHITECTURE.md](ARCHITECTURE.md). |

## Install

**Pre-built binaries** for Linux, macOS (universal) and Windows are on the
[releases page](https://github.com/aubergine-ux/termesh/releases/latest). Unpack and run - there is nothing to install.

**From source** - you need CMake 3.20+ and a C++20 compiler (recent GCC, Clang, Apple Clang or MSVC 2022):

```sh
git clone https://github.com/aubergine-ux/termesh.git
cd termesh
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
./build/termesh --demo knot
```

No model to hand? `--demo knot`, `--demo cube` and `--demo sphere` are built in, and
[`scripts/fetch-samples.sh`](scripts/fetch-samples.sh) downloads a few public-domain glTF models to play with.

## Usage

```sh
termesh model.glb                       # view a model
termesh model.obj --shading flat        # pick a shading mode
termesh model.gltf --backend braille    # force an output backend
termesh model.glb --png view.png        # render a still image
termesh model.glb --spin --gif out.gif  # record a 360° turntable
termesh model.glb --print               # print one frame and exit
```

### Controls

| Action                 | Mouse                     | Keys                          |
| ---------------------- | ------------------------- | ----------------------------- |
| Orbit                  | drag                      | arrows, <kbd>h</kbd> <kbd>j</kbd> <kbd>k</kbd> <kbd>l</kbd> |
| Pan                    | right-drag, or shift-drag | shift + arrows, <kbd>H</kbd> <kbd>J</kbd> <kbd>K</kbd> <kbd>L</kbd> |
| Zoom                   | wheel                     | <kbd>+</kbd> <kbd>-</kbd>     |
| Auto-rotate on / off   |                           | <kbd>space</kbd>              |
| Reset the view         |                           | <kbd>r</kbd>                  |
| Flat / Gouraud / Phong |                           | <kbd>1</kbd> <kbd>2</kbd> <kbd>3</kbd>, or <kbd>s</kbd> to cycle |
| Wireframe              |                           | <kbd>w</kbd>                  |
| Textures, normal maps  |                           | <kbd>t</kbd>, <kbd>n</kbd>    |
| Back-face culling      |                           | <kbd>c</kbd>                  |
| Turn the light         |                           | <kbd>[</kbd> <kbd>]</kbd>     |
| Next output backend    |                           | <kbd>b</kbd>                  |
| Status bar             |                           | <kbd>o</kbd>                  |
| Save a PNG             |                           | <kbd>p</kbd>                  |
| Help, quit             |                           | <kbd>?</kbd>, <kbd>q</kbd>    |

<details>
<summary><b>All command-line options</b></summary>

<br>

| Option | Meaning |
| ------ | ------- |
| `-b`, `--backend <name>` | `kitty`, `sixel`, `halfblock`, `braille` or `ascii` (default: best available) |
| `--color <depth>` | `truecolor`, `256` or `none` (default: detect) |
| `-s`, `--shading <mode>` | `flat`, `gouraud` or `phong` (default) |
| `-w`, `--wireframe` | start in wireframe mode |
| `--no-textures`, `--no-normal-maps`, `--no-cull` | switch parts of the renderer off |
| `--bg <rrggbb>` | background colour |
| `--ssaa <n>` | supersampling factor, 1-4 |
| `--threads <n>` | worker threads (default: all cores) |
| `--fps <n>` | frame rate limit (default: 60) |
| `--yaw`, `--pitch`, `--zoom`, `--fov` | initial camera |
| `--spin`, `--no-spin`, `--spin-speed <deg/s>` | auto-rotation |
| `--no-stats` | hide the status bar |
| `--print`, `--cols <n>`, `--rows <n>` | print one frame into the scrollback |
| `--png <file>`, `--size <WxH>` | render a still image |
| `--gif <file>`, `--frames <n>`, `--gif-fps <n>` | record a turntable |
| `--blocks <n>` | draw exported images with chunky n×n pixels, like terminal cells |
| `--bench <n>` | render n frames off-screen and print timings |
| `--demo <name>` | built-in model: `knot`, `cube`, `sphere` |

</details>

## Output backends

termesh asks the terminal what it supports at start-up and picks the best option. Press <kbd>b</kbd> to step
through the others.

<div align="center">
<img src="docs/media/backends.png" alt="The same model drawn with the pixel, half-block, Braille and ASCII backends" width="940">
</div>

| Backend      | Resolution per cell | Used when                                                            |
| ------------ | ------------------- | -------------------------------------------------------------------- |
| `kitty`      | real pixels         | the terminal answers a kitty graphics query (kitty, Ghostty, WezTerm, Konsole…) |
| `sixel`      | real pixels         | the terminal reports sixel support (foot, xterm, mlterm, Windows Terminal 1.22+…) |
| `halfblock`  | 1 × 2 pixels        | Unicode and colour are available - nearly every modern terminal      |
| `braille`    | 2 × 4 dots          | wireframe mode on a half-block terminal switches to this automatically |
| `ascii`      | 1 character         | nothing else works; also handy over a slow link                      |

Cell-based backends keep a copy of what is on screen and send only the cells that changed, wrapped in a
synchronized update so frames never tear.

## Shading

<div align="center">
<img src="docs/media/shading.png" alt="The same model with flat, Gouraud and Phong shading, and as a wireframe" width="876">
</div>

All three modes use the same Blinn-Phong lighting; what changes is how often it is evaluated. Flat shading
lights each triangle once, Gouraud lights each vertex and interpolates the colour, and Phong interpolates the
normal and lights every pixel - which is also what makes normal maps possible.

## Exporting

```sh
# A 1920×1080 still of the default view
termesh model.glb --png shot.png --size 1920x1080

# A looping 360° turntable, 72 frames at 24 fps
termesh model.glb --spin --gif spin.gif --size 480x360

# The same, with big square pixels like the half-block backend draws
termesh model.glb --spin --gif spin.gif --size 480x480 --blocks 6

# One frame, straight into the terminal
termesh model.glb --print --cols 60
```

In the viewer, <kbd>p</kbd> saves the current view as `termesh-<model>-NN.png`. PNG encoding uses
stb_image_write; the GIF encoder (median-cut palette, LZW, inter-frame deltas) is part of termesh.

## How it works

```
 model file ─► loader ─► vertex stage ─► clipping ─► projection + culling
                                                             │
   terminal ◄─ backend ◄─ resolve ◄─ shading ◄─ rasterizer ◄─ tile binning
```

1. **Vertex stage** - every vertex is transformed to clip space once, in parallel.
2. **Clipping** - triangles crossing the view frustum are cut in homogeneous coordinates (Sutherland-Hodgman),
   before the perspective divide can misbehave.
3. **Setup** - divide by *w*, map to the viewport, cull back faces, snap to a 1/16-pixel grid.
4. **Binning** - triangles are sorted into 32×32-pixel tiles, and tiles are handed to worker threads.
5. **Rasterization** - integer edge functions with a top-left fill rule decide coverage, four pixels at a time
   with SIMD; survivors are depth-tested.
6. **Shading** - attributes are interpolated with perspective correction and fed to the fragment shader.
7. **Resolve and present** - supersamples are averaged in linear light and the image is handed to a backend.

**[ARCHITECTURE.md](ARCHITECTURE.md)** walks through each stage in detail, with the maths behind clipping and
perspective-correct interpolation and pointers into the code.

## Performance

The [Flight Helmet](https://github.com/KhronosGroup/glTF-Sample-Assets/tree/main/Models/FlightHelmet) model -
94,722 triangles, fifteen textures - on a Ryzen 7 5800X (8 cores / 16 threads), measured with
`termesh FlightHelmet.gltf --bench 300`:

| Render target                                         | Flat    | Gouraud | Phong   |
| ----------------------------------------------------- | ------- | ------- | ------- |
| 600 × 354 (a 200×60 terminal, half-blocks, 3× SSAA)   | 2.6 ms  | 2.6 ms  | 2.7 ms  |
| 900 × 580 (kitty / sixel at the default pixel budget) | 4.1 ms  | 3.8 ms  | 4.9 ms  |
| 1920 × 1080                                           | 9.4 ms  | 9.5 ms  | 13.4 ms |

Those are renderer times, with no terminal attached. On one thread the first row takes 18 ms, so even a single
core holds 30 fps. In practice the terminal emulator is the limit, which is why the cell backends work hard to
send as little as possible: for a spinning model a half-block frame is a few tens of kilobytes, against a
couple of hundred for a full redraw.

## Building and testing

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

| CMake option             | Default | Effect                                              |
| ------------------------ | ------- | --------------------------------------------------- |
| `TERMESH_BUILD_TESTS`    | `ON`    | build the doctest unit tests                        |
| `TERMESH_NATIVE`         | `OFF`   | compile with `-march=native`                        |
| `TERMESH_STATIC`         | `OFF`   | link the C++ runtime statically (release binaries)  |

The tests cover the maths library, clipping, the rasterizer's fill rule and perspective correction, texture
sampling, both loaders, every output backend, the GIF encoder and the input parser. CI runs them on Linux
(GCC, Clang, and with SIMD disabled), macOS on Apple Silicon and Windows with MSVC, plus an
AddressSanitizer / UBSan build.

```
src/
├── math/       vectors, matrices, quaternions
├── core/       framebuffer, textures, colour, thread pool
├── scene/      model, camera, OBJ and glTF loaders
├── pipeline/   vertex stage, clipping, lighting, renderer
├── raster/     triangle and line rasterizer, SIMD wrappers
├── output/     kitty, sixel, half-block, Braille, ASCII
├── input/      raw terminal I/O, key and mouse parsing
├── export/     PNG, GIF, off-screen rendering
└── app/        command line, capability detection, viewer
```

## Limitations

termesh is a viewer for static models. It does not play animations or apply skinning and morph targets, it
ignores vertex colours and secondary UV sets, and it cannot read Draco- or meshopt-compressed geometry or
KTX2 / WebP textures. Translucent materials are drawn as cut-outs rather than blended. Lighting is
Blinn-Phong with a fixed light rig, not physically based rendering - glTF metallic-roughness materials are
approximated.

## Credits

- [cgltf](https://github.com/jkuhlmann/cgltf) by Johannes Kuhlmann - glTF parsing (MIT)
- [stb_image and stb_image_write](https://github.com/nothings/stb) by Sean Barrett - image decoding and PNG writing (public domain / MIT)
- [doctest](https://github.com/doctest/doctest) by Viktor Kirilov - unit testing (MIT)
- The models in the images above are *Flight Helmet*, *Boom Box* and *Avocado* from the
  [Khronos glTF Sample Assets](https://github.com/KhronosGroup/glTF-Sample-Assets), all released under CC0.

## License

[MIT](LICENSE)
