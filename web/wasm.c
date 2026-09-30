// Browser entry points for the WebAssembly build. JavaScript hands us camera
// pixels; we return the same ANSI text the terminal app prints, and xterm.js
// draws it.
#include <emscripten/emscripten.h>
#include <stdlib.h>

#include "../src/render.h"

static uint8_t *g_rgba;
static size_t g_rgba_cap;
static uint8_t *g_rgb;
static size_t g_rgb_cap;
static Buf g_out;
static State g_state = {.lo = 0, .hi = 1};
static Settings g_settings = {
    .mode = MODE_COLOR, .mirror = true, .truecolor = true,
    .contrast = 1.2f, .edge_thr = 0.55f, .cell_aspect = 2.0f,
};

/* Returns a buffer where JS writes one RGBA frame (canvas getImageData layout). */
EMSCRIPTEN_KEEPALIVE uint8_t *ac_rgba_buffer(int w, int h) {
    g_rgba = grow(g_rgba, &g_rgba_cap, (size_t)w * h * 4, 1);
    return g_rgba;
}

EMSCRIPTEN_KEEPALIVE void ac_set(int mode, int invert, int mirror, float contrast, float edge_thr, float cell_aspect) {
    g_settings.mode = (Mode)(mode >= 0 && mode < MODE_COUNT ? mode : MODE_COLOR);
    g_settings.invert = invert;
    g_settings.mirror = mirror;
    g_settings.contrast = contrast;
    g_settings.edge_thr = edge_thr;
    g_settings.cell_aspect = cell_aspect;
}

static const char *finish(int w, int h, int cols, int rows) {
    g_out.n = 0;
    buf_str(&g_out, "\x1b[H");
    render_frame(&g_out, &g_state, &g_settings, g_rgb, w, h, cols, rows);
    return g_out.d;
}

/* Renders the RGBA frame previously written to ac_rgba_buffer(). */
EMSCRIPTEN_KEEPALIVE const char *ac_render(int w, int h, int cols, int rows) {
    size_t n = (size_t)w * h;
    g_rgb = grow(g_rgb, &g_rgb_cap, n * 3, 1);
    for (size_t i = 0; i < n; i++) {
        g_rgb[i * 3 + 0] = g_rgba[i * 4 + 0];
        g_rgb[i * 3 + 1] = g_rgba[i * 4 + 1];
        g_rgb[i * 3 + 2] = g_rgba[i * 4 + 2];
    }
    return finish(w, h, cols, rows);
}

/* Renders the animated test pattern (no camera). */
EMSCRIPTEN_KEEPALIVE const char *ac_render_demo(int w, int h, int cols, int rows, double t) {
    g_rgb = grow(g_rgb, &g_rgb_cap, (size_t)w * h * 3, 1);
    demo_frame(g_rgb, w, h, t);
    return finish(w, h, cols, rows);
}

/* Length in bytes of the last rendered frame. */
EMSCRIPTEN_KEEPALIVE int ac_len(void) { return (int)g_out.n; }
