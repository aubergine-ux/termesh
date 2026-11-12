// Inspector: the scene as an outline you can walk through.
//
// A sidebar listing everything in the scene - models, their meshes,
// materials and skeleton nodes, the lights, the environment, the camera -
// with the details of whichever row is selected underneath, and totals for
// the scene and the last frame at the bottom.
//
// This class knows nothing about terminals. It turns a Scene into rows of
// plain text of an exact width; the viewer positions and colours them.
// That split is what lets the whole thing be unit-tested.
#pragma once

#include <set>
#include <string>
#include <vector>

#include "pipeline/renderer.hpp"
#include "stage/scene.hpp"

namespace termesh {

struct InspectorRow {
    enum class Kind {
        Object,  // a model in the scene
        Group,   // "meshes", "materials", "nodes", "lights": a heading with children
        Mesh,
        Material,
        Node,  // a node of an animated model's skeleton
        Light,
        Environment,
        Camera,
    };
    Kind kind = Kind::Group;
    int depth = 0;      // indentation level
    std::string label;  // left-aligned
    std::string value;  // right-aligned: a count, a state
    int object = -1;    // which object it belongs to, if any
    int index = -1;     // which mesh / material / node / light
    bool expandable = false, expanded = false;
    std::string key;  // identifies the row across rebuilds
};

class Inspector {
public:
    // Regenerate the rows from the scene. Call once at the start and again
    // whenever something is expanded or collapsed (the class does so
    // itself for its own operations).
    void rebuild(const Scene& scene);

    const std::vector<InspectorRow>& rows() const { return rows_; }
    int selected() const { return selected_; }
    const InspectorRow* selectedRow() const {
        return selected_ >= 0 && selected_ < static_cast<int>(rows_.size())
                   ? &rows_[static_cast<std::size_t>(selected_)]
                   : nullptr;
    }

    // Move the selection up or down, stopping at the ends.
    void move(int delta);
    void select(int row);
    // Open or close the selected row. `open` < 0 toggles.
    void setExpanded(const Scene& scene, int open = -1);

    // Select the row for a mesh (as after a click in the 3D view), opening
    // whatever has to be open for it to be visible.
    void selectMesh(const Scene& scene, int object, int mesh);

    // The object and mesh the selection refers to, for highlighting:
    // (-1, -1) for none, (object, -1) for a whole object.
    void selection(int& object, int& mesh) const;

    // What a frame of the panel looks like: exactly `height` lines, each
    // exactly `width` columns wide.
    struct Frame {
        std::vector<std::string> lines;
        int selectedLine = -1;      // the line showing the selected row, or -1 if scrolled off
        int treeLines = 0;          // how many lines, from line 1, belong to the outline
        std::vector<int> headings;  // lines to draw as headings
    };
    Frame render(const Scene& scene, const RenderStats& stats, int width, int height, bool unicode);

    // Which row a line of the last rendered frame shows; -1 for none.
    int rowAtLine(int line) const;

    // The details of a row, as "name: value" lines. Public for testing.
    static std::vector<std::string> describe(const Scene& scene, const InspectorRow& row);

private:
    std::vector<InspectorRow> rows_;
    std::set<std::string> expanded_;
    bool initialised_ = false;
    int selected_ = 0;
    int scroll_ = 0;       // first row shown
    int visibleRows_ = 0;  // from the last render
};

}  // namespace termesh
