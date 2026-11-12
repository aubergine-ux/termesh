# Recording

termesh can record what it shows, in three formats. The file's extension
chooses.

| Extension | What is recorded                               | Needs    | Good for                                         |
| --------- | ---------------------------------------------- | -------- | ------------------------------------------------ |
| `.cast`   | the text sent to the terminal, with its timing | nothing  | exact replay in a terminal or on a web page; tiny |
| `.gif`    | the pictures, 20 frames a second               | nothing  | READMEs, chat, anywhere images go                |
| `.mp4`    | the pictures, as H.264 video                   | `ffmpeg` | long or large recordings                         |

## A session

```sh
termesh model.glb --record session.cast     # record from the start; quit to finish
```

or press <kbd>R</kbd> in the viewer to start, and again to stop. Recordings
started that way are named `termesh-<date>-<time>` and are of the same kind
as the `--record` file, or `.cast` if there was none. The status bar shows
`REC` while one is running.

Play a `.cast` back with [asciinema](https://asciinema.org):

```sh
asciinema play session.cast
```

or embed it in a page with asciinema's player. It replays exactly what your
terminal showed, so record with the backend you want viewers to see -
half-blocks (`--backend halfblock`) replay everywhere.

GIF and MP4 recordings hold the rendered picture rather than the terminal's
text, scaled up so that each pixel is the shape it was on screen. The status
bar and the inspector are not part of them.

## A turntable, without a terminal

```sh
termesh model.glb --gif turn.gif --size 480x360           # a looping turntable
termesh model.glb --mp4 turn.mp4 --size 1280x720 --gif-fps 30
termesh --demo physics --mp4 crash.mp4 --frames 240       # eight seconds of the physics demo
```

`--gif` and `--mp4` render off-screen at any size, so they work in scripts
and over SSH. For an animated model they record one loop of the animation;
for a scene with a camera path, one trip along it.
