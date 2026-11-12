// The page around the WebAssembly build of termesh.
//
// The program believes it is talking to a terminal. This script is that
// terminal's other end: it gives the program an xterm.js widget to write
// to, queues up what is typed for it to read, calls it once per animation
// frame, and puts dropped files where it can open them.
"use strict";

(async function () {
    const status = document.getElementById("status");
    const overlay = document.getElementById("overlay");
    const overlayText = document.getElementById("overlay-text");
    const dropHint = document.getElementById("drop");
    const backendSelect = document.getElementById("backend");
    const modelButtons = document.querySelectorAll("#models button[data-args]");

    function setStatus(text, isError) {
        status.textContent = text;
        status.classList.toggle("error", Boolean(isError));
    }

    // ------------------------------------------------------------ terminal
    const term = new Terminal({
        fontFamily: 'ui-monospace, "SF Mono", Menlo, Consolas, "DejaVu Sans Mono", monospace',
        fontSize: 13,
        lineHeight: 1,
        cursorBlink: false,
        scrollback: 0,
        allowProposedApi: true,
        theme: { background: "#101116" },
    });
    const fit = new FitAddon.FitAddon();
    term.loadAddon(fit);
    term.open(document.getElementById("terminal"));
    try {
        // Draws block characters itself, edge to edge, so half-block
        // "pixels" meet without seams. Falls back to the DOM if WebGL is
        // unavailable.
        const webgl = new WebglAddon.WebglAddon();
        webgl.onContextLoss(() => webgl.dispose());
        term.loadAddon(webgl);
    } catch (error) {
        console.warn("termesh: WebGL renderer unavailable, using the DOM renderer", error);
    }

    // What the program sees of the terminal (see src/input/terminal_wasm.cpp).
    const encoder = new TextEncoder();
    const io = {
        input: [],
        cols: 80,
        rows: 24,
        pixelWidth: 0,
        pixelHeight: 0,
        quit: false,
        write: (bytes) => term.write(bytes),
    };
    term.onData((text) => io.input.push(encoder.encode(text)));
    term.onBinary((text) => io.input.push(Uint8Array.from(text, (ch) => ch.charCodeAt(0) & 0xff)));

    function measure() {
        fit.fit();
        io.cols = term.cols;
        io.rows = term.rows;
        const screen = document.querySelector("#terminal .xterm-screen");
        io.pixelWidth = screen ? Math.round(screen.clientWidth) : 0;
        io.pixelHeight = screen ? Math.round(screen.clientHeight) : 0;
    }
    measure();
    new ResizeObserver(measure).observe(document.getElementById("stage"));

    // -------------------------------------------------------------- module
    const errors = [];
    let termesh;
    try {
        termesh = await createTermesh({
            termeshIO: io,
            print: (line) => console.log(line),
            printErr: (line) => {
                console.warn(line);
                errors.push(line);
            },
        });
    } catch (error) {
        setStatus("Could not start the WebAssembly module: " + error, true);
        return;
    }

    // ------------------------------------------------------------- running
    let running = false;
    let current = ["--demo", "knot"];
    let currentLabel = "knot";

    function frame() {
        if (!running) return;
        if (termesh._termesh_web_frame()) {
            requestAnimationFrame(frame);
            return;
        }
        running = false;
        overlayText.textContent = "The viewer has quit.";
        overlay.hidden = false;
    }

    // Start the viewer on a command line (an array of arguments).
    function start(args, label) {
        errors.length = 0;
        io.input.length = 0;
        overlay.hidden = true;
        measure();
        term.reset();
        const all = args.concat(["--backend", backendSelect.value, "--fps", "120"]);
        const failed = termesh.ccall("termesh_web_start", "number", ["string"], [all.join("\n")]);
        if (failed) {
            running = false;
            const reason = errors.join("\n").replace(/^termesh: /gm, "") || "could not be loaded";
            overlayText.textContent = label + ": " + reason;
            overlay.hidden = false;
            setStatus("");
            return false;
        }
        current = args;
        currentLabel = label;
        setStatus(errors.length ? errors.join("\n") : "Showing " + label + ".");
        term.focus();
        if (!running) {
            running = true;
            requestAnimationFrame(frame);
        }
        return true;
    }

    function markActive(button) {
        modelButtons.forEach((b) => b.classList.toggle("active", b === button));
    }

    modelButtons.forEach((button) => {
        button.addEventListener("click", () => {
            markActive(button);
            start(button.dataset.args.split(" "), button.textContent.toLowerCase());
        });
    });
    backendSelect.addEventListener("change", () => start(current, currentLabel));
    document.getElementById("restart").addEventListener("click", () => start(current, currentLabel));

    // --------------------------------------------------------------- files
    const MODEL_TYPES = ["glb", "gltf", "obj", "stl", "ply", "json"];
    const extension = (name) => name.slice(name.lastIndexOf(".") + 1).toLowerCase();

    // Put the files in the module's in-memory file system and view the one
    // that is a model. The rest (a .bin, textures, an .mtl) sit beside it,
    // where the loader will look for them.
    async function view(files) {
        files = Array.from(files);
        if (files.length === 0) return;
        const model =
            files.find((f) => MODEL_TYPES.includes(extension(f.name)) && extension(f.name) !== "json") ||
            files.find((f) => extension(f.name) === "json");
        if (!model) {
            setStatus("None of those is a model termesh can read (" + MODEL_TYPES.slice(0, 5).join(", ") + ").", true);
            return;
        }
        setStatus("Reading " + model.name + "…");
        const FS = termesh.FS;
        try {
            FS.mkdir("/drop");
        } catch (error) {
            for (const name of FS.readdir("/drop")) {
                if (name !== "." && name !== "..") FS.unlink("/drop/" + name);
            }
        }
        for (const file of files) {
            // Names only: no directories to escape from.
            const name = file.name.replace(/[\\/]/g, "_");
            FS.writeFile("/drop/" + name, new Uint8Array(await file.arrayBuffer()));
        }
        markActive(null);
        // Give the page a moment to show the message before a big model
        // keeps the main thread busy loading.
        await new Promise((resolve) => setTimeout(resolve, 30));
        start(["/drop/" + model.name.replace(/[\\/]/g, "_")], model.name);
    }

    let dragDepth = 0;
    window.addEventListener("dragenter", (event) => {
        event.preventDefault();
        dragDepth += 1;
        dropHint.hidden = false;
    });
    window.addEventListener("dragleave", () => {
        dragDepth = Math.max(dragDepth - 1, 0);
        if (dragDepth === 0) dropHint.hidden = true;
    });
    window.addEventListener("dragover", (event) => event.preventDefault());
    window.addEventListener("drop", (event) => {
        event.preventDefault();
        dragDepth = 0;
        dropHint.hidden = true;
        view(event.dataTransfer.files);
    });

    const fileInput = document.getElementById("file");
    document.getElementById("open").addEventListener("click", () => fileInput.click());
    fileInput.addEventListener("change", () => {
        view(fileInput.files);
        fileInput.value = "";
    });

    start(current, currentLabel);
})();
