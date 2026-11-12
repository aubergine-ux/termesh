// The built-in benchmark suite: `termesh --benchmark`.
//
// `--bench` times whatever model it is given. This is different: a fixed set
// of scenes made from the built-in models, each exercising one part of the
// engine, rendered along a fixed camera path. Nothing depends on files, the
// clock or the terminal, so two runs of the same build do exactly the same
// work - which is what makes the numbers comparable between commits.
//
// Each scene reports two kinds of number:
//
//   counters   triangles drawn, pixels covered, bytes of terminal output
//              and a checksum of everything produced. These are exactly
//              reproducible, and a change in any of them means the engine
//              is now doing different work: more overdraw, culling that
//              stopped culling, an encoder that got chattier.
//   times      milliseconds per frame. Real, but noisy; on a shared CI
//              machine they are only good for spotting large changes.
//
// For a precise time-like measure, CI runs each scene under a simulator
// that counts instructions executed (see scripts/perf-check.sh): a number
// that is reproducible to a fraction of a percent on any machine.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "core/thread_pool.hpp"

namespace termesh {

struct BenchmarkResult {
    std::string name;
    std::string description;
    int frames = 0;
    std::uint64_t triangles = 0;         // drawn, summed over the frames
    std::uint64_t pixels = 0;            // covered by geometry, summed over the frames
    std::uint64_t bytes = 0;             // of terminal output, for the scenes that produce any
    std::uint64_t checksum = 0;          // of every image (or byte) produced
    double medianMs = 0.0, minMs = 0.0;  // per frame
};

// The scenes, in the order they run.
std::vector<std::string> benchmarkSceneNames();

// Run one scene for `frames` frames. Returns false for an unknown name.
bool runBenchmarkScene(const std::string& name, int frames, ThreadPool& pool, BenchmarkResult& result);

// The results as a table for people, and as JSON for scripts.
std::string benchmarkTable(const std::vector<BenchmarkResult>& results);
std::string benchmarkJson(const std::vector<BenchmarkResult>& results, unsigned threads);

}  // namespace termesh
