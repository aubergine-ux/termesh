"""Tests for the Python package. Run with: pytest python/tests"""

import io

import numpy as np
import pytest

import termesh


@pytest.fixture(scope="module")
def renderer():
    return termesh.Renderer(threads=2)


def covered(image):
    return int((image[..., 3] > 0).sum())


def test_version():
    assert termesh.__version__.count(".") == 2
    assert termesh.api_version >= 1


def test_render_returns_an_image(renderer):
    scene = termesh.demo("knot")
    image = renderer.render(scene, scene.frame(), 64, 48)
    assert isinstance(image, np.ndarray)
    assert image.shape == (48, 64, 4)
    assert image.dtype == np.uint8
    assert 64 * 48 // 10 < covered(image) < 64 * 48
    assert tuple(image[0, 0]) == (16, 17, 22, 0)  # the default background, uncovered
    assert np.array_equal(image, renderer.render(scene, scene.frame(), 64, 48))


def test_convenience_functions():
    image = termesh.render(termesh.demo("cube"), 40, 30, shading="toon", background=(200, 0, 0))
    assert image.shape == (30, 40, 4)
    assert tuple(image[0, 0, :3]) == (200, 0, 0)

    text = termesh.render_text(termesh.demo("sphere"), cols=30, rows=10, backend="ascii", color="none")
    assert text.count("\n") == 10

    out = io.StringIO()
    termesh.show(termesh.demo("knot"), cols=20, rows=6, file=out)
    assert out.getvalue().count("\n") == 6

    with pytest.raises(TypeError, match="unknown setting"):
        termesh.render(termesh.demo("cube"), 8, 8, shadding="pbr")


def test_settings():
    settings = termesh.Settings()
    assert settings.shading == "phong"
    assert settings.textures is True
    assert settings.skybox is None and settings.outline is None
    settings.shading = "pbr"
    settings.shadows = True
    settings.skybox = False
    settings.exposure = 0.5
    settings.background = (1, 2, 3)
    assert (settings.shading, settings.shadows, settings.skybox) == ("pbr", True, False)
    assert settings.exposure == pytest.approx(0.5)
    assert settings.background == [1, 2, 3]
    copy = settings.copy()
    copy.shading = "flat"
    assert settings.shading == "pbr"
    with pytest.raises(ValueError, match="shading mode"):
        settings.shading = "cel"
    with pytest.raises(ValueError):
        settings.background = (0, 0, 300)
    assert "pbr" in termesh.SHADING_MODES and "braille" in termesh.BACKENDS


def test_settings_change_the_picture(renderer):
    scene = termesh.demo("sphere")
    camera = scene.frame(yaw=0, pitch=0)
    plain = renderer.render(scene, camera, 48, 48)
    settings = termesh.Settings()
    settings.shading = "toon"
    assert not np.array_equal(plain, renderer.render(scene, camera, 48, 48, settings))


def test_mesh_from_arrays(renderer):
    scene = termesh.Scene()
    positions = np.array([[-1, -1, 0], [1, -1, 0], [0, 1, 0]], dtype=np.float64)  # converted for us
    index = scene.add_mesh(positions, [[0, 1, 2]], color=(1, 0, 0, 1), double_sided=True, name="tri")
    assert index == 0
    assert len(scene) == 1 and scene.objects == ["tri"]
    assert scene.triangle_count == 1 and scene.vertex_count == 3
    lo, hi = scene.bounds
    assert lo == [-1, -1, 0] and hi == [1, 1, 0]

    camera = termesh.Camera(position=(0, 0, 4), target=(0, 0, 0), fov=40)
    image = renderer.render(scene, camera, 40, 40)
    r, g, b, a = (int(v) for v in image[20, 20])
    assert a == 255 and r > g + 40 and r > b + 40

    # Translation goes in the last column, as NumPy writes matrices.
    matrix = np.eye(4, dtype=np.float32)
    matrix[0, 3] = 100
    scene.set_transform(0, matrix)
    assert scene.bounds[0][0] == pytest.approx(99)
    scene.set_transform(0, np.eye(4))

    scene.set_visible(0, False)
    assert covered(renderer.render(scene, camera, 40, 40)) == 0

    # Points: no indices.
    cloud = scene.add_mesh(np.random.default_rng(1).random((50, 3)), colors=np.ones((50, 3)))
    assert cloud == 1 and scene.vertex_count == 53

    with pytest.raises(ValueError, match="positions"):
        scene.add_mesh(np.zeros((3, 2)))
    with pytest.raises(ValueError, match="normals"):
        scene.add_mesh(positions, [[0, 1, 2]], normals=np.zeros((2, 3)))
    with pytest.raises(termesh.Error, match="does not exist"):
        scene.add_mesh(positions, [[0, 1, 7]])
    with pytest.raises(IndexError):
        scene.set_visible(9, True)
    with pytest.raises(ValueError):
        scene.set_transform(0, np.eye(3))


def test_errors_are_exceptions(renderer, tmp_path):
    with pytest.raises(termesh.Error):
        termesh.Scene.open(str(tmp_path / "missing.obj"))
    assert issubclass(termesh.Error, RuntimeError)
    scene = termesh.Scene()
    with pytest.raises(termesh.Error, match="teapot"):
        scene.add_demo("teapot")
    with pytest.raises(termesh.Error):
        scene.set_environment(str(tmp_path / "missing.hdr"))
    scene.add_demo("cube")
    with pytest.raises(ValueError):
        renderer.render(scene, scene.frame(), 0, 10)
    with pytest.raises(ValueError):
        renderer.render(scene, scene.frame(), 100000, 100000)
    with pytest.raises(ValueError, match="backend"):
        renderer.render_text(scene, scene.frame(), backend="teletype")
    with pytest.raises(termesh.Error):
        renderer.render(scene, termesh.Camera(position=(1, 1, 1), target=(1, 1, 1)), 8, 8)


def test_files(renderer, tmp_path):
    obj = tmp_path / "tri.obj"
    obj.write_text("v -1 -1 0\nv 1 -1 0\nv 0 1 0\nf 1 2 3\n")
    scene = termesh.Scene.open(str(obj))
    assert scene.triangle_count == 1
    assert scene.camera is None
    assert termesh.render(obj, 16, 16).shape == (16, 16, 4)  # a path-like works too

    (tmp_path / "scene.json").write_text(
        """{
          "models": [ { "file": "tri.obj" }, { "demo": "cube", "position": [3, 0, 0] } ],
          "camera": { "position": [0, 1, 9], "target": [1, 0, 0], "fov": 50 },
          "render": { "shading": "pbr", "ssao": true, "background": "#102030" }
        }"""
    )
    scene = termesh.Scene.open(str(tmp_path / "scene.json"))
    assert len(scene) == 2
    assert scene.camera.position == pytest.approx([0, 1, 9])
    assert scene.camera.fov == pytest.approx(50)
    settings = scene.settings()
    assert settings.shading == "pbr" and settings.ssao and settings.background == [0x10, 0x20, 0x30]
    # The convenience functions pick up the scene's camera and settings.
    image = termesh.render(scene, 48, 32)
    assert tuple(image[0, 0, :3]) == (0x10, 0x20, 0x30)
    assert covered(image) > 20


def test_text_backends(renderer):
    scene = termesh.demo("knot")
    camera = scene.frame()
    blocks = renderer.render_text(scene, camera, 40, 12)
    assert blocks.count("\n") == 12
    assert "▀" in blocks and "\x1b[38;2;" in blocks
    braille = renderer.render_text(scene, camera, 40, 12, backend="braille", color="none")
    assert any("⠀" <= ch <= "⣿" for ch in braille)
    for backend in ("sixel", "kitty"):
        assert renderer.render_text(scene, camera, 20, 6, backend=backend).startswith("\x1b")


def test_lights_and_environment(renderer):
    scene = termesh.demo("sphere")
    camera = scene.frame(yaw=0, pitch=0)
    before = renderer.render(scene, camera, 32, 32)
    assert scene.add_light("directional", direction=(0, 0, -1), color=(1, 0, 0), intensity=4) == 0
    lit = renderer.render(scene, camera, 32, 32)
    assert int(lit[16, 16, 0]) > int(lit[16, 16, 2]) + 30
    with pytest.raises(ValueError, match="light type"):
        scene.add_light("laser")
    scene.clear_lights()
    assert np.array_equal(before, renderer.render(scene, camera, 32, 32))
    scene.set_environment("sunset")
    assert not np.array_equal(before, renderer.render(scene, camera, 32, 32))


def test_animation(renderer):
    scene = termesh.Scene()
    scene.add_demo("tentacle")
    clips = scene.animations()
    assert clips and all(isinstance(name, str) and seconds > 0 for name, seconds in clips)
    camera = scene.frame()
    scene.seek(0)
    start = renderer.render(scene, camera, 48, 48)
    scene.advance(clips[0][1] * 0.3)
    assert not np.array_equal(start, renderer.render(scene, camera, 48, 48))
    scene.seek(0)
    assert np.array_equal(start, renderer.render(scene, camera, 48, 48))
    scene.play(clips[-1][0], fade=0.1)  # by name
    scene.play(0)                       # by number
    scene.set_speed(2.0)
    with pytest.raises(KeyError):
        scene.play("moonwalk")
    still = termesh.demo("cube")
    assert still.animations() == []
    with pytest.raises(termesh.Error):
        still.play(0)


def test_picking():
    scene = termesh.Scene()
    scene.add_mesh(np.array([[-1, -1, 0], [1, -1, 0], [0, 1, 0]], dtype=np.float32), [[0, 1, 2]])
    camera = termesh.Camera(position=(0, 0, 4))
    hit = scene.pick(camera, 0, 0)
    assert hit is not None
    assert (hit.object, hit.mesh, hit.triangle) == (0, 0, 0)
    assert hit.distance == pytest.approx(4, rel=0.01)
    assert hit.position[2] == pytest.approx(0, abs=0.01)
    assert scene.pick(camera, 0.95, 0.95) is None
    assert "Hit(" in repr(hit) and "Camera(" in repr(camera) and "Scene" in repr(scene)


def test_rendering_from_several_threads():
    from concurrent.futures import ThreadPoolExecutor

    def work(name):
        scene = termesh.demo(name)
        return covered(termesh.Renderer(threads=1).render(scene, scene.frame(), 48, 48))

    with ThreadPoolExecutor(4) as pool:
        results = list(pool.map(work, ["knot", "cube", "sphere", "knot"] * 3))
    assert all(count > 0 for count in results)
    assert results[0] == results[3]
