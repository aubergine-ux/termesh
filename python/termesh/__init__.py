"""termesh: 3D rendering without a GPU, to pixels or to terminal text.

    >>> import termesh
    >>> print(termesh.render_text("model.glb", cols=80, rows=24))
    >>> image = termesh.render("model.glb", 640, 480, shading="pbr", shadows=True)
    >>> image.shape
    (480, 640, 4)

For more than a single picture, keep the pieces:

    >>> scene = termesh.Scene.open("model.glb")
    >>> renderer = termesh.Renderer()
    >>> for step in range(36):
    ...     camera = scene.frame(yaw=step * 10)
    ...     frame = renderer.render(scene, camera, 320, 240)
"""

from __future__ import annotations

import os
import sys

from ._termesh import (
    BACKENDS,
    COLOR_DEPTHS,
    DITHER_MODES,
    SHADING_MODES,
    TONE_MAPPERS,
    Camera,
    Error,
    Hit,
    Renderer,
    Scene,
    Settings,
    __version__,
    api_version,
)

__all__ = [
    "BACKENDS",
    "COLOR_DEPTHS",
    "DITHER_MODES",
    "SHADING_MODES",
    "TONE_MAPPERS",
    "Camera",
    "Error",
    "Hit",
    "Renderer",
    "Scene",
    "Settings",
    "__version__",
    "api_version",
    "demo",
    "render",
    "render_text",
    "show",
]

_renderer = None


def _shared_renderer():
    """One renderer for the convenience functions, made on first use."""
    global _renderer
    if _renderer is None:
        _renderer = Renderer()
    return _renderer


def demo(name="knot"):
    """A scene holding one of the built-in models."""
    scene = Scene()
    scene.add_demo(name)
    return scene


def _as_scene(source):
    if isinstance(source, Scene):
        return source
    return Scene.open(os.fspath(source))


def _settings(scene, settings, overrides):
    """The settings to draw with: the scene's own, then `settings`, then keywords."""
    result = settings.copy() if settings is not None else scene.settings()
    for name, value in overrides.items():
        if not hasattr(result, name):
            raise TypeError(f"unknown setting '{name}'")
        setattr(result, name, value)
    return result


def _camera(scene, camera, yaw, pitch, zoom, fov):
    if camera is not None:
        return camera
    if yaw is None and pitch is None and scene.camera is not None:
        return scene.camera
    return scene.frame(
        yaw=30.0 if yaw is None else yaw,
        pitch=15.0 if pitch is None else pitch,
        zoom=zoom,
        fov=fov,
    )


def render(source, width=640, height=480, *, camera=None, yaw=None, pitch=None, zoom=1.0, fov=40.0,
           settings=None, **overrides):
    """Render a model to pixels.

    `source` is a path or a Scene. Returns a (height, width, 4) uint8 NumPy
    array: sRGB red, green, blue, and coverage (0 on the background).

    The view is `camera` if given; otherwise the scene file's camera if it
    has one; otherwise the whole scene framed, turned by `yaw` and raised by
    `pitch` degrees. Any setting can be passed by name, for example
    ``shading="pbr", shadows=True, background=(0, 0, 0)``.
    """
    scene = _as_scene(source)
    return _shared_renderer().render(
        scene, _camera(scene, camera, yaw, pitch, zoom, fov), width, height,
        _settings(scene, settings, overrides),
    )


def render_text(source, cols=80, rows=24, *, backend="halfblock", color="truecolor", camera=None, yaw=None,
                pitch=None, zoom=1.0, fov=40.0, settings=None, **overrides):
    """Render a model to the text a terminal would display.

    Returns a string of `rows` lines. `backend` is 'halfblock', 'braille',
    'ascii', 'sixel' or 'kitty'; `color` is 'truecolor', '256' or 'none'.
    The other arguments are as for render().
    """
    scene = _as_scene(source)
    return _shared_renderer().render_text(
        scene, _camera(scene, camera, yaw, pitch, zoom, fov), cols, rows, backend, color,
        _settings(scene, settings, overrides),
    )


def show(source, cols=None, rows=None, *, file=None, **options):
    """Print a model to the terminal (or to `file`).

    The size defaults to the terminal's width and a matching height.
    """
    file = sys.stdout if file is None else file
    if cols is None or rows is None:
        try:
            size = os.get_terminal_size(file.fileno())
            width, height = size.columns, size.lines
        except (AttributeError, OSError, ValueError):
            width, height = 80, 24
        cols = cols or width
        rows = rows or max(min(cols // 2, height - 2), 2)
    file.write(render_text(source, cols, rows, **options))
    file.flush()
