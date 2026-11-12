// The scene inspector's outline, details and layout.
#include <doctest/doctest.h>

#include <string>

#include "app/inspector.hpp"
#include "scene/primitives.hpp"

using namespace termesh;

namespace {

using Kind = InspectorRow::Kind;

// Two models, two lights and a camera path: one of everything.
Scene makeScene() {
    Scene scene;
    scene.name = "test scene";
    scene.add(makeDemoModel("cube"), "box", translate({3, 0, 0})).source = "demo:cube";
    scene.add(makeDemoModel("tentacle"), "worm");
    Light sun;
    sun.castsShadow = true;
    Light spot;
    spot.type = LightType::Spot;
    spot.position = {0, 5, 0};
    scene.lights = {sun, spot};
    scene.cameraPath = makeTurntable(scene.bounds(), 8.0f, 0.3f, 1.0f, radians(40.0f));
    return scene;
}

int find(const Inspector& inspector, Kind kind, const std::string& label) {
    for (std::size_t i = 0; i < inspector.rows().size(); ++i)
        if (inspector.rows()[i].kind == kind && inspector.rows()[i].label == label) return static_cast<int>(i);
    return -1;
}

int columns(const std::string& text) {
    int n = 0;
    for (const char c : text) n += (static_cast<unsigned char>(c) & 0xC0) != 0x80;
    return n;
}

bool contains(const std::vector<std::string>& lines, const std::string& part) {
    for (const std::string& line : lines)
        if (line.find(part) != std::string::npos) return true;
    return false;
}

}  // namespace

TEST_CASE("inspector: the outline lists everything in the scene") {
    const Scene scene = makeScene();
    Inspector inspector;
    inspector.rebuild(scene);

    // A small scene starts with its objects open.
    const int box = find(inspector, Kind::Object, "box"), worm = find(inspector, Kind::Object, "worm");
    REQUIRE(box == 0);
    REQUIRE(worm > box);
    CHECK(inspector.rows()[0].expanded);
    CHECK(inspector.rows()[0].depth == 0);
    CHECK(inspector.rows()[0].value == "12 tris");
    CHECK(inspector.rows()[1].kind == Kind::Group);
    CHECK(inspector.rows()[1].label == "meshes");
    CHECK(inspector.rows()[1].depth == 1);
    CHECK(inspector.rows()[1].object == 0);

    // Only the animated model has a skeleton to show.
    int nodeGroups = 0;
    for (const InspectorRow& row : inspector.rows()) nodeGroups += row.kind == Kind::Group && row.label == "nodes";
    CHECK(nodeGroups == 1);

    // Lights are listed (and open); the environment and camera close the list.
    const int lights = find(inspector, Kind::Group, "lights");
    REQUIRE(lights > worm);
    CHECK(inspector.rows()[static_cast<std::size_t>(lights)].value == "2");
    CHECK(inspector.rows()[static_cast<std::size_t>(lights) + 1].kind == Kind::Light);
    CHECK(inspector.rows()[static_cast<std::size_t>(lights) + 1].label == "directional");
    CHECK(inspector.rows()[static_cast<std::size_t>(lights) + 1].value == "shadows");
    CHECK(inspector.rows()[static_cast<std::size_t>(lights) + 2].label == "spot");
    CHECK(find(inspector, Kind::Environment, "environment") > lights);
    CHECK(inspector.rows().back().kind == Kind::Camera);
    CHECK(inspector.rows().back().value == "path 8s");

    // Every row can be told apart.
    std::set<std::string> keys;
    for (const InspectorRow& row : inspector.rows()) keys.insert(row.key);
    CHECK(keys.size() == inspector.rows().size());
}

TEST_CASE("inspector: opening, closing and moving") {
    const Scene scene = makeScene();
    Inspector inspector;
    inspector.rebuild(scene);
    const std::size_t initial = inspector.rows().size();

    // Movement stops at the ends.
    inspector.move(-5);
    CHECK(inspector.selected() == 0);
    inspector.move(1000);
    CHECK(inspector.selected() == static_cast<int>(initial) - 1);
    inspector.select(1);  // the box's "meshes" heading

    // Opening a heading shows its children; the selection stays put.
    inspector.setExpanded(scene, 1);
    REQUIRE(inspector.rows().size() == initial + 1);
    CHECK(inspector.selected() == 1);
    CHECK(inspector.rows()[2].kind == Kind::Mesh);
    CHECK(inspector.rows()[2].depth == 2);
    inspector.setExpanded(scene, 1);  // already open: nothing happens
    CHECK(inspector.rows().size() == initial + 1);

    // "Left" on a leaf goes to its heading; "left" again closes it.
    inspector.move(1);
    inspector.setExpanded(scene, 0);
    CHECK(inspector.selected() == 1);
    inspector.setExpanded(scene, 0);
    CHECK(inspector.rows().size() == initial);

    // Toggling flips the state each time.
    inspector.setExpanded(scene);
    CHECK(inspector.rows().size() == initial + 1);
    inspector.setExpanded(scene);
    CHECK(inspector.rows().size() == initial);

    // Closing an object hides everything under it, and the selection
    // follows the row it was on.
    inspector.select(0);
    inspector.setExpanded(scene, 0);
    CHECK(inspector.rows()[1].kind == Kind::Object);
    CHECK(inspector.selectedRow()->label == "box");

    // The skeleton is indented by depth in the hierarchy.
    const int worm = find(inspector, Kind::Object, "worm");
    inspector.select(find(inspector, Kind::Group, "nodes"));
    REQUIRE(inspector.selectedRow()->object == scene.objects.size() - 1);
    inspector.setExpanded(scene, 1);
    const int bone0 = find(inspector, Kind::Node, "bone 0"), bone1 = find(inspector, Kind::Node, "bone 1");
    REQUIRE(bone0 > worm);
    CHECK(bone1 == bone0 + 1);
    CHECK(inspector.rows()[static_cast<std::size_t>(bone1)].depth ==
          inspector.rows()[static_cast<std::size_t>(bone0)].depth + 1);
}

TEST_CASE("inspector: selection, to and from the 3D view") {
    const Scene scene = makeScene();
    Inspector inspector;
    inspector.rebuild(scene);
    int object = 0, mesh = 0;

    // An object row selects the whole object; a mesh row one mesh; rows
    // that are not part of a model select nothing.
    inspector.select(0);
    inspector.selection(object, mesh);
    CHECK(object == 0);
    CHECK(mesh == -1);
    inspector.select(find(inspector, Kind::Group, "lights"));
    inspector.selection(object, mesh);
    CHECK(object == -1);
    inspector.select(static_cast<int>(inspector.rows().size()) - 1);
    inspector.selection(object, mesh);
    CHECK(object == -1);

    // A click on a mesh in the view: the outline opens to show it.
    inspector.selectMesh(scene, 1, 0);
    REQUIRE(inspector.selectedRow());
    CHECK(inspector.selectedRow()->kind == Kind::Mesh);
    CHECK(inspector.selectedRow()->object == 1);
    inspector.selection(object, mesh);
    CHECK(object == 1);
    CHECK(mesh == 0);

    // Selecting "no object" changes nothing.
    inspector.selectMesh(scene, -1, -1);
    inspector.selection(object, mesh);
    CHECK(object == 1);
}

TEST_CASE("inspector: a large scene starts folded") {
    Scene scene;
    for (int i = 0; i < 9; ++i) scene.add(makeDemoModel("cube"), "cube " + std::to_string(i));
    Inspector inspector;
    inspector.rebuild(scene);
    int objects = 0, others = 0;
    for (const InspectorRow& row : inspector.rows()) (row.kind == Kind::Object ? objects : others)++;
    CHECK(objects == 9);
    CHECK(others == 3);  // lights, environment, camera
    CHECK(inspector.rows()[static_cast<std::size_t>(find(inspector, Kind::Group, "lights"))].value == "default rig");
    CHECK(inspector.rows().back().value == "orbit");
}

TEST_CASE("inspector: details of each kind of row") {
    Scene scene = makeScene();
    scene.objects[1]->accel.buildLods(scene.objects[1]->model);
    Inspector inspector;
    inspector.rebuild(scene);

    const std::vector<std::string> box = Inspector::describe(scene, inspector.rows()[0]);
    CHECK(contains(box, "source: demo:cube"));
    CHECK(contains(box, "triangles: 12"));
    CHECK(contains(box, "position: 3, 0, 0"));
    CHECK_FALSE(contains(box, "animation"));

    const std::vector<std::string> worm =
        Inspector::describe(scene, inspector.rows()[static_cast<std::size_t>(find(inspector, Kind::Object, "worm"))]);
    CHECK(contains(worm, "animation: wave (1 of 2)"));
    CHECK(contains(worm, "time: 0 / 2.4 s"));

    inspector.selectMesh(scene, 1, 0);
    const std::vector<std::string> mesh = Inspector::describe(scene, *inspector.selectedRow());
    CHECK(contains(mesh, "triangles: 3072"));
    CHECK(contains(mesh, "skinned: yes"));
    CHECK(contains(mesh, "levels of detail: "));
    CHECK(contains(mesh, "material: tentacle"));

    inspector.select(find(inspector, Kind::Group, "materials"));
    inspector.setExpanded(scene, 1);
    inspector.move(1);
    REQUIRE(inspector.selectedRow()->kind == Kind::Material);
    const std::vector<std::string> material = Inspector::describe(scene, *inspector.selectedRow());
    CHECK(contains(material, "metallic: 0"));
    CHECK(contains(material, "alpha: opaque"));
    CHECK(contains(material, "colour map: 256x256"));
    CHECK(contains(material, "emissive map: none"));

    const std::vector<std::string> spot =
        Inspector::describe(scene, inspector.rows()[static_cast<std::size_t>(find(inspector, Kind::Light, "spot"))]);
    CHECK(contains(spot, "type: spot"));
    CHECK(contains(spot, "position: 0, 5, 0"));
    CHECK(contains(spot, "cone: "));
    CHECK(contains(spot, "shadows: no"));
    const std::vector<std::string> sun = Inspector::describe(
        scene, inspector.rows()[static_cast<std::size_t>(find(inspector, Kind::Light, "directional"))]);
    CHECK(contains(sun, "shadows: yes"));
    CHECK_FALSE(contains(sun, "position"));

    const std::vector<std::string> environment = Inspector::describe(
        scene, inspector.rows()[static_cast<std::size_t>(find(inspector, Kind::Environment, "environment"))]);
    CHECK(contains(environment, "kind: two colours"));
    const std::vector<std::string> camera = Inspector::describe(scene, inspector.rows().back());
    CHECK(contains(camera, "path: 16 keys, 8 s"));
    CHECK(contains(camera, "loops: yes"));

    // Rows that point at things which are not there describe nothing,
    // rather than reading out of bounds.
    InspectorRow stale;
    stale.kind = Kind::Mesh;
    stale.object = 0;
    stale.index = 99;
    CHECK(Inspector::describe(scene, stale).empty());
    stale.object = 42;
    CHECK(Inspector::describe(scene, stale).empty());
    stale.kind = Kind::Light;
    stale.index = 7;
    CHECK(Inspector::describe(scene, stale).empty());
}

TEST_CASE("inspector: the panel is exactly the size it is given") {
    const Scene scene = makeScene();
    Inspector inspector;
    inspector.rebuild(scene);
    RenderStats stats;
    stats.trianglesDrawn = 1234;
    stats.trianglesCulled = 56;

    for (const bool unicode : {true, false})
        for (const int width : {12, 30, 45})
            for (const int height : {4, 9, 20, 60}) {
                const Inspector::Frame frame = inspector.render(scene, stats, width, height, unicode);
                CAPTURE(width);
                CAPTURE(height);
                REQUIRE(frame.lines.size() == static_cast<std::size_t>(height));
                for (const std::string& line : frame.lines) {
                    REQUIRE(columns(line) == width);
                    if (!unicode)
                        for (const char c : line) REQUIRE(static_cast<unsigned char>(c) < 0x80);
                }
                // The title is a heading, and the selected row is on screen.
                REQUIRE_FALSE(frame.headings.empty());
                CHECK(frame.headings[0] == 0);
                CHECK(frame.selectedLine >= 1);
                CHECK(frame.selectedLine <= frame.treeLines);
            }

    const Inspector::Frame frame = inspector.render(scene, stats, 40, 40, true);
    CHECK(frame.lines[0].find("test scene") != std::string::npos);
    CHECK(contains(frame.lines, "1234 drawn, 56 culled"));
    CHECK(contains(frame.lines, "2 models"));
    CHECK(frame.lines[static_cast<std::size_t>(frame.selectedLine)].find("box") != std::string::npos);

    // Nothing to draw into: nothing drawn.
    CHECK(inspector.render(scene, stats, 0, 10, true).lines.empty());
    CHECK(inspector.render(scene, stats, 10, 0, true).lines.empty());
}

TEST_CASE("inspector: scrolling keeps the selection in view") {
    Scene scene;
    for (int i = 0; i < 40; ++i) scene.add(makeDemoModel("cube"), "cube " + std::to_string(i));
    Inspector inspector;
    inspector.rebuild(scene);
    const RenderStats stats;

    // Walk down past the bottom of a short panel...
    Inspector::Frame frame = inspector.render(scene, stats, 30, 16, true);
    const int visible = frame.treeLines;
    REQUIRE(visible > 2);
    REQUIRE(visible < 40);
    for (int i = 0; i < 25; ++i) inspector.move(1);
    frame = inspector.render(scene, stats, 30, 16, true);
    REQUIRE(frame.selectedLine >= 1);
    CHECK(frame.lines[static_cast<std::size_t>(frame.selectedLine)].find("cube 25") != std::string::npos);
    // ...and a line of the panel maps back to the row it shows.
    CHECK(inspector.rowAtLine(frame.selectedLine) == 25);
    CHECK(inspector.rowAtLine(0) == -1);            // the title
    CHECK(inspector.rowAtLine(visible + 1) == -1);  // below the outline
    CHECK(inspector.rowAtLine(-3) == -1);

    // ...then back up to the top.
    inspector.move(-100);
    frame = inspector.render(scene, stats, 30, 16, true);
    CHECK(frame.selectedLine == 1);
    CHECK(inspector.rowAtLine(1) == 0);
}

TEST_CASE("inspector: long names are cut to fit, counts are kept") {
    Scene scene;
    scene.add(makeDemoModel("cube"), "a model with a remarkably long name that goes on and on");
    scene.add(makeDemoModel("cube"),
              "caf\xc3\xa9 \xe2\x98\x95 na\xc3\xafve");  // non-ASCII, several bytes per character
    Inspector inspector;
    inspector.rebuild(scene);
    const Inspector::Frame frame = inspector.render(scene, RenderStats{}, 28, 20, true);
    const std::string& longLine = frame.lines[1];
    CHECK(columns(longLine) == 28);
    CHECK(longLine.find("\xe2\x80\xa6") != std::string::npos);  // an ellipsis
    CHECK(longLine.find("12 tris") != std::string::npos);
    for (const std::string& line : frame.lines) CHECK(columns(line) == 28);
}
