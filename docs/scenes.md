# Scene files

A scene file describes several models, where each one stands, the lights, the
environment, the camera and how to render it all, in JSON:

```sh
termesh examples/showroom.json
termesh examples/flythrough.json --gif fly.gif
```

Everything is optional except `models`. Paths are relative to the scene file.
Two liberties are taken with JSON because these files are written by hand:
`//` comments, and a comma after the last element of a list.

```json
{
  "name": "showroom",
  "models": [
    { "file": "helmet.glb", "position": [0, 0.8, 0], "rotation": [0, 30, 0] },
    { "demo": "plane", "scale": 4, "material": { "color": "#808890", "roughness": 0.9 } }
  ],
  "lights": [
    { "type": "directional", "direction": [-0.6, -1, -0.5], "intensity": 2, "shadows": true }
  ],
  "environment": { "name": "sky", "skybox": false },
  "camera": { "turntable": { "seconds": 12, "elevation": 20 } },
  "render": { "shading": "pbr", "shadows": true }
}
```

## `models`

A list. Each entry needs a `file` (OBJ or glTF) or a `demo` (one of the
built-in models: `knot`, `cube`, `sphere`, `tentacle`, `plane`).

| Field | Meaning | Default |
| ----- | ------- | ------- |
| `file` / `demo` | what to load | required |
| `name` | shown in the inspector | the file or demo name |
| `position` | `[x, y, z]` | `[0, 0, 0]` |
| `rotation` | degrees about x, y and z; applied as yaw (y), then pitch (x), then roll (z) | `[0, 0, 0]` |
| `scale` | a number, or `[x, y, z]` | `1` |
| `visible` | `false` hides it | `true` |
| `material` | overrides applied to every material of the model (below) | none |
| `animation` | clip to play: a name, or a number from 1 | the first |
| `animationSpeed` | playback rate; negative runs backwards | `1` |
| `animationTime` | where to start, in seconds | `0` |
| `paused` | start with the animation stopped | `false` |

`material` may set `color` (which replaces the colour texture), `metallic`,
`roughness`, `emissive`, `opacity` (below 1 makes the model translucent) and
`doubleSided`.

Colours are `"#rrggbb"` (sRGB, as on the web) or `[r, g, b]` in linear light,
where values above 1 are allowed - useful for `emissive`.

## `lights`

A list. With no lights at all, the default rig is used: a key and a fill light
attached to the camera.

| Field | Meaning | Default |
| ----- | ------- | ------- |
| `type` | `directional`, `point` or `spot` | `directional` |
| `direction` | the way the light travels (directional and spot) | `[0, -1, 0]` |
| `position` | where it is (point and spot) | `[0, 0, 0]` |
| `target` | a point to aim at, instead of `direction` | - |
| `color` | | white |
| `intensity` | | `1` |
| `range` | point and spot: the light fades to nothing at this distance | unlimited |
| `angle` | spot: half-width of the cone in degrees | `30` |
| `blend` | spot: fraction of the cone over which the edge fades | `0.2` |
| `specular` | scale of the highlights; `0` for a fill light | `1` |
| `shadows` | cast shadows (directional and spot) | `false` |
| `attachedToCamera` | `direction` and `position` are relative to the camera | `false` |

Point and spot lights fall off with the square of the distance, so their
`intensity` depends on the scale of the scene: a spot five units away needs
about 25 to be as bright as a directional light of 1.

Shadows also need `"shadows": true` under `render`, or `--shadows`.

## `environment`

Ambient light and reflections, and optionally the background.

| Field | Meaning | Default |
| ----- | ------- | ------- |
| `name` | a built-in environment: `studio`, `sky`, `sunset` | `studio` |
| `file` | an HDR panorama (Radiance `.hdr`, equirectangular) | - |
| `sky`, `ground` | two colours: the simplest environment | - |
| `intensity` | brightness multiplier | `1` |
| `rotation` | turn about the vertical axis, in degrees | `0` |
| `skybox` | draw it as the background | on for panoramas |
| `blur` | 0-1: soften the background | `0` |

## `camera`

Without one, the camera frames the whole scene and can be orbited as usual.

| Field | Meaning |
| ----- | ------- |
| `position`, `target`, `fov` | a fixed starting view (`fov` is vertical, in degrees) |
| `turntable` | circle the scene: `seconds` per turn (12), `elevation` in degrees (15), `distance` (1 = framed exactly) |
| `path` | follow keyframes (below) |

A path:

```json
"path": {
  "loop": true,
  "duration": 20,
  "interpolation": "smooth",
  "keys": [
    { "time": 0, "position": [-7, 2, 5], "target": [-3, 1, 0], "fov": 50 },
    { "time": 4, "position": [-3, 1.4, 3.5], "target": [-1, 1, -0.5] },
    { "time": 8, "position": [0, 3.5, 4.5] }
  ]
}
```

Each key gives a `time` in seconds and any of `position`, `target` and `fov`;
whatever it leaves out is inherited from the key before, and a key with no
`time` comes one second after the last. `"interpolation": "smooth"` (the
default) passes a curve through the keys; `"linear"` moves in straight lines.
With `loop`, the camera returns from the last key to the first, and `duration`
says how long the whole circuit takes.

In the viewer the path plays by itself. Steering the camera - dragging, the
arrow keys, the wheel - takes over from wherever the path had reached;
<kbd>v</kbd> hands control back. `--no-path` ignores the path altogether, and
`--camera-time <s>` picks the moment for a still.

## `render`

Defaults for the render settings. Anything given on the command line wins.

| Field | Values |
| ----- | ------ |
| `shading` | `flat`, `gouraud`, `phong`, `pbr`, `toon` |
| `wireframe`, `textures`, `normalMaps`, `cull` | `true` / `false` |
| `shadows`, `shadowSize` (64-2048), `shadowSoftness` (1-7) | |
| `ssao`, `bloom`, `fxaa`, `outline` | `true` / `false` |
| `tonemap` | `aces`, `reinhard`, `filmic`, `linear` |
| `exposure` | stops |
| `background` | `"#rrggbb"` |
| `ssaa` | supersampling factor, 1-4 |
| `lod` | level-of-detail tolerance in pixels; 0 is off |
| `fov` | vertical field of view for the orbit camera, in degrees |
| `lightAngle` | turn of the default key light |
| `dither` | `auto`, `none`, `ordered`, `fs` |

## Several models without a scene file

```sh
termesh helmet.glb fox.glb boombox.glb
```

Models named together on the command line were not made to share a space, so
each is scaled to the same size and they are stood in a row.

## The inspector

<kbd>Tab</kbd> opens a sidebar listing the scene: every model with its meshes,
materials and (for animated models) skeleton nodes, then the lights, the
environment and the camera. The arrow keys move through it, <kbd>←</kbd> and
<kbd>→</kbd> fold and unfold, and the details of the selected row are shown
underneath, with totals for the scene and the last frame at the bottom.
Selecting a model or mesh highlights it in the picture; clicking something in
the picture selects it in the outline.
