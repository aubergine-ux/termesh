# 3D previews in terminal file managers

`termesh --preview <file>` prints one still of a model and exits, sized for a
file manager's preview pane:

```sh
termesh --preview model.glb --cols 60 --rows 20
```

It is built for that job:

- **It never waits for the terminal.** A preview pane's output is a pipe, so
  termesh does not ask the terminal what it supports (the question would
  never be answered). It prints half-block text, which every file manager
  below can show; pass `--backend` and `--color` to choose something else.
- **It is remembered.** The finished output is cached under
  `$XDG_CACHE_HOME/termesh/previews` (or `~/.cache/termesh/previews`), keyed
  by the file's path, size and modification time and by the size requested.
  Loading a model is what takes the time - mostly decoding its textures - so
  the first look at a large one can take a second; every look after that is
  a single file read. `--no-cache` turns this off.
- **It gives up on enormous files.** Anything over 256 MB gets a line of text
  instead of a long wait; change the limit with `--preview-max-mb`.

All the usual options apply: `--shading pbr`, `--yaw 120`, `--bg 000000`,
`--animation Run --anim-time 0.4`, and so on. Scene files work too.

The snippets below show termesh for the formats it reads natively. If your
build has the Assimp backend, add `fbx`, `dae` and friends to the lists.

## yazi

Yazi's [piper](https://github.com/yazi-rs/plugins/tree/main/piper.yazi)
plugin turns any command into a previewer. Install it once:

```sh
ya pkg add yazi-rs/plugins:piper
```

Then, in `~/.config/yazi/yazi.toml`:

```toml
[plugin]
prepend_previewers = [
  { name = "*.glb",  run = 'piper -- termesh --preview "$1" --cols $w --rows $h' },
  { name = "*.gltf", run = 'piper -- termesh --preview "$1" --cols $w --rows $h' },
  { name = "*.obj",  run = 'piper -- termesh --preview "$1" --cols $w --rows $h' },
  { name = "*.stl",  run = 'piper -- termesh --preview "$1" --cols $w --rows $h' },
  { name = "*.ply",  run = 'piper -- termesh --preview "$1" --cols $w --rows $h' },
]
```

`$1` is the file, `$w` and `$h` the size of the preview pane in cells.

## lf

lf runs a previewer script with the file and the pane's size. In
`~/.config/lf/lfrc`:

```sh
set previewer ~/.config/lf/preview.sh
```

and `~/.config/lf/preview.sh` (make it executable with `chmod +x`):

```sh
#!/bin/sh
# $1 file   $2 width   $3 height   $4 x   $5 y
case "$(printf '%s' "$1" | tr '[:upper:]' '[:lower:]')" in
    *.glb | *.gltf | *.obj | *.stl | *.ply)
        termesh --preview "$1" --cols "$2" --rows "$3"
        ;;
    *)
        # ...whatever you use for other files; at its simplest:
        cat "$1"
        ;;
esac
```

## ranger

Ranger previews through `scope.sh`. If you do not have one yet:

```sh
ranger --copy-config=scope
```

Make sure `~/.config/ranger/rc.conf` has

```
set use_preview_script true
set preview_script ~/.config/ranger/scope.sh
```

and add a case to the `handle_extension()` function in
`~/.config/ranger/scope.sh`:

```sh
        ## 3D models
        glb|gltf|obj|stl|ply)
            termesh --preview "${FILE_PATH}" --cols "${PV_WIDTH}" --rows "${PV_HEIGHT}" --color 256 && exit 5
            exit 1;;
```

`--color 256` is there because ranger's preview pane understands the
256-colour escape sequences but not 24-bit ones. `exit 5` tells ranger the
output was made for exactly this pane size.

## Other programs

Anything that can run a command and show its output can use `--preview`:
`fzf --preview 'termesh --preview {} --cols $FZF_PREVIEW_COLUMNS --rows $FZF_PREVIEW_LINES'`
gives a fuzzy finder with 3D previews.

For a terminal that supports real images, `--backend kitty` or `--backend
sixel` produces a pixel image instead of text - but only where the file
manager passes such output through untouched, which the text-based preview
panes above do not.
