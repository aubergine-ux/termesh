// Drives the WebAssembly viewer the way the page does, but from Node and
// with a fake terminal, and checks that it draws and responds.
//
//     node web/test/smoke.cjs build-wasm/termesh-web.js
"use strict";

const assert = require("node:assert");
const path = require("node:path");

const createTermesh = require(path.resolve(process.argv[2]));
const sleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms));

(async () => {
    let written = [];
    const errors = [];
    const io = {
        input: [],
        cols: 80,
        rows: 30,
        pixelWidth: 800,
        pixelHeight: 600,
        quit: false,
        write: (bytes) => written.push(Buffer.from(bytes)),
    };
    const termesh = await createTermesh({ termeshIO: io, print: () => {}, printErr: (line) => errors.push(line) });
    const start = (args) => termesh.ccall("termesh_web_start", "number", ["string"], [args.join("\n")]);
    const output = () => Buffer.concat(written).toString("utf8");
    const frames = async (count) => {
        for (let i = 0; i < count; ++i) {
            assert.strictEqual(termesh._termesh_web_frame(), 1, "the viewer stopped unexpectedly");
            await sleep(20);
        }
    };
    const type = (text) => io.input.push(Buffer.from(text));

    // Nothing started yet: nothing to step.
    assert.strictEqual(termesh._termesh_web_frame(), 0);

    // A demo model, drawn with half blocks in 24-bit colour.
    assert.strictEqual(start(["--demo", "knot", "--fps", "120"]), 0, errors.join("\n"));
    await frames(5);
    let text = output();
    assert(text.includes("\x1b[?1049h"), "did not switch to the alternate screen");
    assert(text.includes("▀"), "no half blocks were drawn");
    assert(text.includes("\x1b[38;2;"), "no 24-bit colour");
    assert(text.includes("knot"), "no status bar");

    // Keys reach it: 's' changes the shading mode named in the status bar.
    written = [];
    type("s");
    await frames(4);
    assert(/gouraud|pbr|toon|flat|phong/.test(output()), "the status bar did not redraw after a key");

    // A resize is noticed.
    written = [];
    io.cols = 60;
    io.rows = 20;
    io.pixelWidth = 600;
    io.pixelHeight = 400;
    await frames(4);
    assert(output().length > 0, "nothing was redrawn after a resize");

    // Mouse drag (SGR encoding): press, move, release.
    written = [];
    type("\x1b[<0;30;10M\x1b[<32;34;12M\x1b[<0;34;12m");
    await frames(3);
    assert(output().length > 0, "nothing was redrawn after a drag");

    // 'q' quits, and the terminal is given back.
    written = [];
    type("q");
    assert.strictEqual(termesh._termesh_web_frame(), 0);
    assert(output().includes("\x1b[?1049l"), "did not leave the alternate screen");
    assert.strictEqual(termesh._termesh_web_frame(), 0);

    // A scene file packed into the module, with an animated model in it.
    written = [];
    assert.strictEqual(start(["/examples/showroom.json", "--backend", "braille"]), 0, errors.join("\n"));
    await frames(4);
    assert(/[⠀-⣿]/.test(output()), "no Braille was drawn");

    // A file "dropped" on the page: written to the in-memory file system.
    termesh.FS.mkdir("/drop");
    termesh.FS.writeFile("/drop/tri.obj", "v -1 -1 0\nv 1 -1 0\nv 0 1 0\nf 1 2 3\n");
    written = [];
    assert.strictEqual(start(["/drop/tri.obj", "--backend", "ascii"]), 0, errors.join("\n"));
    await frames(3);
    assert(output().includes("tri"), "the dropped model's name is not in the status bar");

    // The physics demo runs, and takes its keys.
    written = [];
    assert.strictEqual(start(["--demo", "physics", "--bodies", "8"]), 0, errors.join("\n"));
    await frames(4);
    type("\r");
    await frames(4);
    assert(output().includes("physics"), "the physics demo did not draw");

    // Failures are reported, not fatal.
    errors.length = 0;
    assert.notStrictEqual(start(["/drop/missing.glb"]), 0);
    assert(errors.join("\n").includes("missing.glb"), "no error message for a missing file");
    assert.strictEqual(termesh._termesh_web_frame(), 0);
    termesh.FS.writeFile("/drop/broken.glb", "this is not a glb file");
    assert.notStrictEqual(start(["/drop/broken.glb"]), 0);
    assert.notStrictEqual(start(["--no-such-option"]), 0);

    // ...and it still works afterwards.
    assert.strictEqual(start(["--demo", "tentacle"]), 0, errors.join("\n"));
    await frames(3);
    termesh._termesh_web_stop();
    assert.strictEqual(termesh._termesh_web_frame(), 0);

    console.log("web smoke test passed");
})().catch((error) => {
    console.error(error);
    process.exit(1);
});
