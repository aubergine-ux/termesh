# termesh

3D rendering without a GPU. termesh is a software rasterizer written from
scratch - shading, shadows, skeletal animation and all - that draws to
pixels or to the text a terminal displays.

```sh
pip install termesh
```

```python
import termesh

# Straight to the terminal
termesh.show("model.glb")

# To a NumPy array: (height, width, 4) uint8
image = termesh.render("model.glb", 640, 480, shading="pbr", shadows=True)

# To a string of terminal text
text = termesh.render_text(termesh.demo("knot"), cols=80, rows=24, backend="braille")
```

Scenes can be built in code, from files or from arrays:

```python
import numpy as np

scene = termesh.Scene()
scene.add_file("robot.glb")
scene.add_mesh(
    positions=np.array([[-2, 0, -2], [2, 0, -2], [2, 0, 2], [-2, 0, 2]], dtype=np.float32),
    indices=[[0, 2, 1], [0, 3, 2]],
    color=(0.5, 0.5, 0.5, 1.0),
)
scene.add_light("directional", direction=(-1, -2, -1), intensity=3, shadows=True)
scene.set_environment("sky")

renderer = termesh.Renderer()
settings = termesh.Settings()
settings.shading = "pbr"
settings.shadows = True

for step in range(36):                      # a turntable
    camera = scene.frame(yaw=step * 10, pitch=20)
    scene.advance(1 / 24)                   # play the model's animation
    frame = renderer.render(scene, camera, 640, 480, settings)
```

It reads OBJ, glTF 2.0 (`.gltf`, `.glb`), STL and PLY, plus termesh's own
JSON scene files. The package also installs the `termesh` command, an
interactive model viewer for the terminal.

Documentation: <https://github.com/aubergine-ux/termesh/blob/main/docs/python.md>
