// Entry points of the WebAssembly build: the viewer, driven by a web page.
//
// A native program owns its main loop. A page cannot: the browser calls
// *it*. So instead of main() there are three functions for the page's
// script (web/playground.js) to call:
//
//   termesh_web_start(args)   load what the arguments name and take over the terminal
//   termesh_web_frame()       one step of the viewer; called on every animation frame
//   termesh_web_stop()        put the terminal back
//
// The arguments are the command line's, separated by newlines, so anything
// the program can be started with works here too. Files are whatever the
// page has put in the in-memory file system (a model dropped on the page
// is written there first).
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <memory>
#include <string>
#include <vector>

#include <emscripten.h>

#include "app/options.hpp"
#include "app/setup.hpp"
#include "app/viewer.hpp"

using namespace termesh;

namespace {

// Everything one run of the viewer needs, in the order it must be destroyed
// in reverse: the viewer refers to all the rest.
struct Session {
    Options options;
    ThreadPool pool{1};
    Scene scene;
    std::unique_ptr<Viewer> viewer;
};

std::unique_ptr<Session> session;

bool start(const std::string& commandLine) {
    std::vector<std::string> arguments{"termesh"};
    for (std::size_t begin = 0; begin <= commandLine.size();) {
        std::size_t end = commandLine.find('\n', begin);
        if (end == std::string::npos) end = commandLine.size();
        if (end > begin) arguments.push_back(commandLine.substr(begin, end - begin));
        begin = end + 1;
    }
    std::vector<const char*> argv;
    for (const std::string& argument : arguments) argv.push_back(argument.c_str());
    const int argc = static_cast<int>(argv.size());

    ParseResult parsed = parseOptions(argc, argv.data());
    if (!parsed.ok) {
        std::fprintf(stderr, "termesh: %s\n", parsed.error.c_str());
        return false;
    }
    auto next = std::make_unique<Session>();
    next->options = std::move(parsed.options);
    if (next->options.demo.empty() && next->options.inputs.empty()) next->options.demo = "knot";
    if (!buildScene(argc, argv.data(), next->options, next->pool, next->scene)) return false;

    if (!next->options.environment.empty() || !next->scene.hasEnvironment) {
        if (std::string error; !loadEnvironmentOption(next->options, next->scene.environment, error)) {
            std::fprintf(stderr, "termesh: %s\n", error.c_str());
            return false;
        }
    }
    if (!setUpAnimations(next->options, next->scene, next->pool)) return false;

    next->viewer = std::make_unique<Viewer>(next->options, next->scene, next->pool);
    if (!next->viewer->begin()) return false;
    session = std::move(next);
    return true;
}

void stop() {
    if (session && session->viewer) session->viewer->end();
    session.reset();
}

}  // namespace

extern "C" {

// Returns 0 on success. On failure the reason has been written to stderr
// (which the page shows) and whatever was running before is left stopped.
EMSCRIPTEN_KEEPALIVE int termesh_web_start(const char* commandLine) {
    // What a page's terminal can do, where a native build would ask.
    setenv("TERM", "xterm-256color", 1);
    setenv("COLORTERM", "truecolor", 1);
    setenv("LANG", "en_US.UTF-8", 1);
    try {
        stop();
        return start(commandLine ? commandLine : "") ? 0 : 1;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "termesh: %s\n", e.what());
    } catch (...) {
        std::fprintf(stderr, "termesh: unexpected error\n");
    }
    session.reset();
    return 1;
}

// Returns 1 while the viewer is running, 0 once it has stopped (the user
// pressed q, or nothing was started).
EMSCRIPTEN_KEEPALIVE int termesh_web_frame(void) {
    if (!session) return 0;
    try {
        if (session->viewer->step()) return 1;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "termesh: %s\n", e.what());
    } catch (...) {
        std::fprintf(stderr, "termesh: unexpected error\n");
    }
    stop();
    return 0;
}

EMSCRIPTEN_KEEPALIVE void termesh_web_stop(void) { stop(); }

}  // extern "C"
