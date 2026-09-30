# ascii-cam

**▶ Try it in your browser:** https://candedefranco.github.io/ascii-cam/

```text
$ ./ascii-cam
```

Your webcam, live, rendered as ASCII art in the terminal. Written in **C** with no dependencies. A tiny Objective-C shim opens the macOS camera; everything else (sampling, auto-levels, edge detection, rendering) is plain C.

## Modes

| Key | Mode | What it does |
|:--:|:--|:--|
| `1` | `ascii` | Classic 70-character brightness ramp |
| `2` | `color` | Same glyphs, each one painted with the real pixel color |
| `3` | `blocks` | Half-block `▀` pixels: two colored pixels per cell, the sharpest mode |
| `4` | `edges` | Sobel edge detection, drawn with `\| / - \` along each contour |
| `5` | `matrix` | Green katakana rain over your silhouette |

## Controls

| Key | Action |
|:--:|:--|
| `1`–`5` | Switch mode |
| `+` / `-` | Contrast (edge sensitivity in `edges` mode) |
| `i` | Invert |
| `m` | Toggle mirror |
| `s` | Save a snapshot as `.ans` (replay it with `cat file.ans`) |
| `h` | Hide the status bar (clean shots for recording) |
| `q` | Quit |

## Build & run

Requires macOS and the Xcode command line tools (`xcode-select --install`).

```bash
make
./ascii-cam            # camera, color mode
./ascii-cam --mode matrix
./ascii-cam --demo     # animated test pattern, no camera needed
```

The first run asks for camera access for your terminal app. If you denied it, enable it in *System Settings → Privacy & Security → Camera*.

Use a terminal with 24-bit color (iTerm2, Ghostty, WezTerm, Warp, VS Code) for the best result. Other terminals fall back to 256 colors automatically. Smaller fonts mean more "pixels": zoom out with `Cmd -`.

## Browser build (WebAssembly)

The same C engine (`src/render.c`) is compiled to WebAssembly with [Emscripten](https://emscripten.org) and published from `docs/`. In the browser, JavaScript grabs camera frames, hands the pixels to the C engine, and [xterm.js](https://xtermjs.org) draws the exact ANSI output the terminal app prints.

```bash
brew install emscripten
make web                              # builds docs/ascii-cam.js + docs/ascii-cam.wasm
python3 -m http.server -d docs 8000   # open http://localhost:8000
```

## How it works

```text
camera_mac.m          main.c + render.c
┌──────────────┐     ┌──────────────────────────────────────────────────────────┐
│ AVFoundation │ RGB │ sample grid ─▶ auto-levels ─▶ mode renderer ─▶ one write │
│  640×480     ├────▶│ (crop to fit,  (2–98th pct    (ramp / color /   per frame│
│  BGRA frames │     │  3×3 average)   stretch)       ▀ / Sobel / rain)         │
└──────────────┘     └──────────────────────────────────────────────────────────┘
```

- **Sampling:** the frame is center-cropped to the terminal's aspect ratio, since cells are about twice as tall as they are wide. Each cell averages a 3×3 set of samples.
- **Auto-levels:** stretches the 2nd–98th luminance percentile to the full range, smoothed across frames. This way a dim webcam still uses the whole ramp.
- **Rendering:** each frame is built in one buffer and written with a single `write()`. The cursor jumps back to the top instead of clearing the screen, so there's no flicker. Color escape codes are only emitted when the color changes.
- **Edges:** a Sobel operator runs on the luminance grid. The gradient angle picks the glyph, corrected for the 2:1 cell aspect.
