# termesh for Python

```sh
pip install termesh
```

The package is the termesh renderer with a Python face: load or build a
scene, render it to a NumPy array or to terminal text. It has no dependencies
beyond NumPy, needs no GPU or display, and also installs the `termesh`
command.

Until a release is on PyPI - or to build the current source - install from a
checkout (a C++20 compiler and CMake are needed):

```sh
pip install .
```

## One-liners

```python
import termesh

termesh.show("model.glb")                                  # print it in the terminal
image = termesh.render("model.glb", 640, 480)              # (480, 640, 4) uint8 array
text = termesh.render_text("model.glb", cols=80, rows=24)  # a str of terminal text
```

Each takes a path or a `Scene`, and the same keyword arguments:

| Argument                      | Meaning                                                                    |
| ----------------------------- | -------------------------------------------------------------------------- |
| `yaw`, `pitch`, `zoom`, `fov` | where to look from: the scene framed, turned and raised by these (degrees) |
| `camera`                      | an explicit `Camera`, instead                                              |
| `settings`                    | a `Settings` object to start from                                          |
| any setting by name           | `shading="pbr"`, `shadows=True`, `background=(0, 0, 0)`, ...               |
| `backend`, `color`            | (text only) `"halfblock"`, `"braille"`, `"ascii"`, `"sixel"`, `"kitty"`; `"truecolor"`, `"256"`, `"none"` |

The image's fourth channel is coverage - 0 where nothing was drawn - so
`Image.fromarray(image)` gives a picture with a transparent background, and
`image[..., :3]` one with the background colour.

## Scenes

```python
scene = termesh.Scene.open("model.glb")     # a model, or a JSON scene file
scene = termesh.Scene()                     # or start empty
scene.add_file("robot.glb")                 # each returns the new object's index
scene.add_demo("plane")                     # knot, cube, sphere, tentacle, plane
```

Geometry can come straight from arrays:

```python
import numpy as np

positions = np.array([[-1, -1, 0], [1, -1, 0], [0, 1, 0]], dtype=np.float32)
scene.add_mesh(positions, [[0, 1, 2]], color=(1.0, 0.3, 0.2, 1.0), double_sided=True)
scene.add_mesh(points, colors=point_colors)          # no indices: a point cloud
```

`add_mesh` also takes `normals` (n, 3), `uvs` (n, 2), `metallic`, `roughness`
and `name`. Objects are placed with a 4 x 4 matrix, written the NumPy way
(translation in the last column):

```python
matrix = np.eye(4)
matrix[:3, 3] = (2.0, 0.0, 0.0)
scene.set_transform(1, matrix)
scene.set_visible(0, False)
```

`len(scene)`, `scene.objects` (their names), `scene.bounds`,
`scene.triangle_count` and `scene.vertex_count` describe what is there.

### Lights and environment

```python
scene.add_light("directional", direction=(-1, -2, -1), intensity=3, shadows=True)
scene.add_light("spot", position=(0, 4, 3), direction=(0, -1, -0.6), intensity=40, outer_cone=30)
scene.add_light("point", position=(2, 1, 0), color=(1.0, 0.6, 0.3), intensity=10, range=8)
scene.set_environment("sky")                # studio, sky, sunset, or an .hdr file
scene.clear_lights()
```

A scene with no lights of its own is lit by a default pair attached to the
camera.

### Animation

```python
for name, seconds in scene.animations():    # of object 0; pass object=n for others
    print(name, seconds)

scene.play("Run", fade=0.3)                 # by name or number, with a cross-fade
scene.seek(0.4)
scene.set_speed(0.5)
scene.advance(1 / 30)                       # move every animation on
```

## Rendering

The one-liners share a renderer. For a loop, or to choose the thread count,
keep your own:

```python
renderer = termesh.Renderer(threads=0)      # 0: every core
settings = scene.settings()                 # defaults + the scene file's own
settings.shading = "pbr"
settings.shadows = True
settings.ssao = True

frames = []
for step in range(72):
    camera = scene.frame(yaw=step * 5, pitch=20)
    scene.advance(1 / 24)
    frames.append(renderer.render(scene, camera, 640, 480, settings))
```

Rendering releases the GIL, so several threads can render at once, each with
its own `Scene` and `Renderer`.

### Settings

| Attribute                                       | Type            | Default   |
| ----------------------------------------------- | --------------- | --------- |
| `shading`                                       | `"flat"`, `"gouraud"`, `"phong"`, `"pbr"`, `"toon"` | `"phong"` |
| `wireframe`, `points`                           | bool            | `False`   |
| `textures`, `normal_maps`, `cull_backfaces`     | bool            | `True`    |
| `point_size`                                    | float           | `1.0`     |
| `background`                                    | (r, g, b) 0-255 | dark grey |
| `shadows`                                       | bool            | `False`   |
| `shadow_map_size`, `shadow_softness`            | int             | 1024, 3   |
| `skybox`                                        | bool or `None` (the scene decides) | `None` |
| `sky_blur`                                      | float 0-1 or `None` | `None` |
| `ssao`, `bloom`, `fxaa`                         | bool            | `False`   |
| `outline`                                       | bool or `None` (with toon shading only) | `None` |
| `tone_mapper`                                   | `"aces"`, `"reinhard"`, `"filmic"`, `"linear"` | `"aces"` |
| `exposure`                                      | float, in stops | `0.0`     |
| `supersampling`                                 | 1-4, or 0 for the default | `0` |
| `lod_tolerance`                                 | float, pixels; 0 is off | `0.0` |
| `dither`                                        | `"auto"`, `"none"`, `"ordered"`, `"floyd-steinberg"` | `"auto"` |

### Cameras and picking

```python
camera = scene.frame(yaw=30, pitch=15, zoom=1.0, fov=40)     # fit everything in view
camera = termesh.Camera(position=(0, 1, 5), target=(0, 0, 0), fov=40)
camera = scene.camera                                        # from a scene file, or None

hit = scene.pick(camera, x=0.0, y=0.0, aspect=640 / 480)     # x, y in -1..1, +y up
if hit:
    print(scene.objects[hit.object], hit.mesh, hit.triangle, hit.distance, hit.position)
```

## Errors

Anything the renderer reports - a file that cannot be read, a bad index
buffer - raises `termesh.Error` (a `RuntimeError`). Arguments of the wrong
shape or an unknown option name raise `ValueError`, `IndexError` or
`TypeError` as usual.

## Examples

Save a picture with Pillow:

```python
from PIL import Image
Image.fromarray(termesh.render("model.glb", 1280, 960, shading="pbr", fxaa=True)).save("model.png")
```

Show a NumPy mesh in a notebook or a log, as plain text:

```python
print(termesh.render_text(scene, cols=60, rows=20, backend="ascii", color="none"))
```

## Publishing (for maintainers)

`.github/workflows/wheels.yml` builds wheels for Linux, macOS and Windows
with cibuildwheel and, when a `v*` tag is pushed, uploads them to PyPI by
trusted publishing. That needs a one-time setup on pypi.org: under *Publishing*
add a (pending) trusted publisher for project `termesh`, owner
`aubergine-ux`, repository `termesh`, workflow `wheels.yml`, environment
`pypi`.
