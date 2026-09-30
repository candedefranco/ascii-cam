// ascii-cam engine: turns an RGB frame into ANSI-colored text. Shared by the
// terminal app (main.c) and the browser build (web/wasm.c).
#include "render.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* PI isn't part of standard C, so define our own. */
#define PI 3.14159265358979323846f

const char *MODE_NAMES[MODE_COUNT] = {"ascii", "color", "blocks", "edges", "matrix"};

/* Dark → bright. The long ramp gives smoother gradients on faces. */
static const char RAMP[] =
    " .'`^\",:;Il!i><~+_-?][}{1)(|\\/tfjrxnuvczXYUJCLQ0OZmwqpdbkhao*#MW&8%B@$";
#define RAMP_LEN ((int)sizeof(RAMP) - 1)

/* Half-width katakana + digits: single terminal cell each. */
static const char *MATRIX_GLYPHS[] = {
    "ｱ", "ｲ", "ｳ", "ｴ", "ｵ", "ｶ", "ｷ", "ｸ", "ｹ", "ｺ", "ｻ", "ｼ", "ｽ", "ｾ", "ｿ", "ﾀ", "ﾁ", "ﾂ", "ﾃ",
    "ﾄ", "ﾅ", "ﾆ", "ﾇ", "ﾈ", "ﾉ", "ﾊ", "ﾋ", "ﾌ", "ﾍ", "ﾎ", "ﾏ", "ﾐ", "ﾑ", "ﾒ", "ﾓ", "ﾔ", "ﾕ", "ﾖ",
    "ﾗ", "ﾘ", "ﾙ", "ﾚ", "ﾛ", "ﾜ", "ﾝ", "0", "1", "2", "3", "4", "5", "6", "7", "8", "9"};
#define MATRIX_GLYPH_COUNT ((int)(sizeof(MATRIX_GLYPHS) / sizeof(MATRIX_GLYPHS[0])))

void buf_put(Buf *b, const char *s, size_t n) {
    if (b->n + n > b->cap) {
        size_t cap = b->cap ? b->cap * 2 : 1 << 16;
        while (cap < b->n + n) cap *= 2;
        char *d = realloc(b->d, cap);
        if (!d) { perror("realloc"); exit(1); }
        b->d = d;
        b->cap = cap;
    }
    memcpy(b->d + b->n, s, n);
    b->n += n;
}

void buf_str(Buf *b, const char *s) { buf_put(b, s, strlen(s)); }
void buf_ch(Buf *b, char c) { buf_put(b, &c, 1); }

void buf_int(Buf *b, int v) {
    char tmp[12];
    int i = sizeof tmp;
    if (v == 0) tmp[--i] = '0';
    while (v > 0) { tmp[--i] = (char)('0' + v % 10); v /= 10; }
    buf_put(b, tmp + i, sizeof tmp - i);
}

/* Sets foreground (bg=false) or background color; falls back to the 256-color cube. */
void buf_color(Buf *b, bool bg, bool truecolor, int r, int g, int bl) {
    buf_str(b, bg ? "\x1b[48;" : "\x1b[38;");
    if (truecolor) {
        buf_str(b, "2;");
        buf_int(b, r); buf_ch(b, ';');
        buf_int(b, g); buf_ch(b, ';');
        buf_int(b, bl);
    } else {
        int idx = 16 + 36 * ((r * 5 + 127) / 255) + 6 * ((g * 5 + 127) / 255) + (bl * 5 + 127) / 255;
        buf_str(b, "5;");
        buf_int(b, idx);
    }
    buf_ch(b, 'm');
}

float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

/*
 * Samples the source image into a gw x gh grid. `aspect` is the height/width
 * ratio of one grid cell on screen (terminal cells are ~2:1, half blocks ~1:1).
 * The source is center-cropped so the picture fills the terminal undistorted.
 */
static void sample_grid(const uint8_t *rgb, int sw, int sh, int gw, int gh, float aspect, bool mirror, Px *out) {
    float target = (float)gw / (gh * aspect);
    float cw = (float)sw, ch = (float)sh, ox = 0, oy = 0;
    if ((float)sw / sh > target) {
        cw = sh * target;
        ox = (sw - cw) / 2;
    } else {
        ch = sw / target;
        oy = (sh - ch) / 2;
    }
    float cellw = cw / gw, cellh = ch / gh;

    for (int gy = 0; gy < gh; gy++) {
        for (int gx = 0; gx < gw; gx++) {
            float x0 = ox + gx * cellw, y0 = oy + gy * cellh;
            float r = 0, g = 0, b = 0;
            for (int sy = 0; sy < 3; sy++) {
                for (int sx = 0; sx < 3; sx++) {
                    int px = (int)(x0 + (sx + 0.5f) * cellw / 3);
                    int py = (int)(y0 + (sy + 0.5f) * cellh / 3);
                    px = px < 0 ? 0 : px >= sw ? sw - 1 : px;
                    py = py < 0 ? 0 : py >= sh ? sh - 1 : py;
                    if (mirror) px = sw - 1 - px;
                    const uint8_t *p = rgb + ((size_t)py * sw + px) * 3;
                    r += p[0];
                    g += p[1];
                    b += p[2];
                }
            }
            Px *o = &out[gy * gw + gx];
            o->r = r / (9 * 255.f);
            o->g = g / (9 * 255.f);
            o->b = b / (9 * 255.f);
            o->y = 0.2126f * o->r + 0.7152f * o->g + 0.0722f * o->b;
        }
    }
}

/*
 * Auto-levels: stretches the 2nd..98th luminance percentile to 0..1 so a dim
 * webcam still uses the whole character ramp. Smoothed across frames.
 */
static void auto_levels(const Px *grid, int n, float *lo, float *hi) {
    int hist[64] = {0};
    for (int i = 0; i < n; i++) hist[(int)(clampf(grid[i].y, 0, 1) * 63)]++;
    int acc = 0, lo_i = 0, hi_i = 63;
    for (int i = 0; i < 64; i++) { acc += hist[i]; if (acc >= n * 0.02f) { lo_i = i; break; } }
    acc = 0;
    for (int i = 63; i >= 0; i--) { acc += hist[i]; if (acc >= n * 0.02f) { hi_i = i; break; } }
    float nlo = lo_i / 63.f, nhi = hi_i / 63.f;
    if (nhi - nlo < 0.15f) nhi = nlo + 0.15f;
    *lo = *lo * 0.85f + nlo * 0.15f;
    *hi = *hi * 0.85f + nhi * 0.15f;
}

/* Leveled + contrast-adjusted luminance for a cell. */
static float tone(const Px *p, float lo, float hi, float contrast) {
    float y = (p->y - lo) / (hi - lo);
    return clampf((y - 0.5f) * contrast + 0.5f, 0, 1);
}

/* Scales a cell's color so its brightness matches the leveled luminance, keeping the hue. */
static void tone_rgb(const Px *p, float y, int *r, int *g, int *b) {
    float k = y / (p->y > 0.02f ? p->y : 0.02f);
    *r = (int)(clampf(p->r * k, 0, 1) * 255);
    *g = (int)(clampf(p->g * k, 0, 1) * 255);
    *b = (int)(clampf(p->b * k, 0, 1) * 255);
}

void demo_frame(uint8_t *rgb, int w, int h, double t) {
    float cx = w / 2 + cosf((float)t * 0.9f) * w * 0.22f;
    float cy = h / 2 + sinf((float)t * 1.3f) * h * 0.18f;
    float rad = h * 0.26f;
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            float v = sinf(x * 0.021f + (float)t) + sinf(y * 0.027f - (float)t * 1.3f) +
                      sinf((x + y) * 0.015f + (float)t * 0.7f);
            float r = 0.35f + 0.25f * sinf(v * 1.7f);
            float g = 0.30f + 0.25f * sinf(v * 1.7f + 2.1f);
            float b = 0.45f + 0.30f * sinf(v * 1.7f + 4.2f);
            float dx = x - cx, dy = y - cy, d = sqrtf(dx * dx + dy * dy) / rad;
            if (d < 1) { /* a shaded sphere drifting around */
                float light = clampf(1.1f - sqrtf((dx + rad * 0.35f) * (dx + rad * 0.35f) +
                                                  (dy + rad * 0.35f) * (dy + rad * 0.35f)) / (rad * 1.4f), 0.05f, 1);
                r = 0.95f * light; g = 0.72f * light; b = 0.55f * light;
            }
            uint8_t *p = rgb + ((size_t)y * w + x) * 3;
            p[0] = (uint8_t)(clampf(r, 0, 1) * 255);
            p[1] = (uint8_t)(clampf(g, 0, 1) * 255);
            p[2] = (uint8_t)(clampf(b, 0, 1) * 255);
        }
    }
}

void *grow(void *p, size_t *cap, size_t need, size_t elem) {
    if (*cap >= need) return p;
    void *q = realloc(p, need * elem);
    if (!q) { perror("realloc"); exit(1); }
    *cap = need;
    return q;
}

static uint32_t hash3(uint32_t x, uint32_t y, uint32_t z) {
    uint32_t h = x * 374761393u + y * 668265263u + z * 2147483647u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return h ^ (h >> 16);
}

static void render_chars(Buf *out, State *st, const Settings *s, const uint8_t *rgb, int sw, int sh, int cols, int rows) {
    size_t n = (size_t)cols * rows;
    st->grid = grow(st->grid, &st->grid_cap, n, sizeof(Px));
    st->tones = grow(st->tones, &st->tones_cap, n, sizeof(float));
    sample_grid(rgb, sw, sh, cols, rows, s->cell_aspect, s->mirror, st->grid);
    auto_levels(st->grid, (int)n, &st->lo, &st->hi);
    for (size_t i = 0; i < n; i++) {
        float y = tone(&st->grid[i], st->lo, st->hi, s->contrast);
        st->tones[i] = s->invert ? 1 - y : y;
    }

    if (s->mode == MODE_MATRIX && st->rain_cols != cols) {
        st->rain = realloc(st->rain, cols * sizeof(float));
        st->speed = realloc(st->speed, cols * sizeof(float));
        for (int x = 0; x < cols; x++) {
            st->rain[x] = -(float)(rand() % (rows * 2));
            st->speed[x] = 0.3f + (rand() % 100) / 140.f;
        }
        st->rain_cols = cols;
    }

    int last = -1;
    for (int y = 0; y < rows; y++) {
        for (int x = 0; x < cols; x++) {
            size_t i = (size_t)y * cols + x;
            float t = st->tones[i];

            if (s->mode == MODE_ASCII) {
                buf_ch(out, RAMP[(int)(t * (RAMP_LEN - 1) + 0.5f)]);
            } else if (s->mode == MODE_COLOR) {
                int r, g, b;
                tone_rgb(&st->grid[i], s->invert ? 1 - t : t, &r, &g, &b);
                int key = ((r >> 3) << 10) | ((g >> 3) << 5) | (b >> 3);
                if (key != last) { buf_color(out, false, s->truecolor, r, g, b); last = key; }
                /* Color carries the image, so bias toward denser glyphs. */
                buf_ch(out, RAMP[(int)(sqrtf(t) * (RAMP_LEN - 1) + 0.5f)]);
            } else if (s->mode == MODE_EDGES) {
                /* Sobel on the toned grid, then pick a glyph along the edge. */
                if (x == 0 || y == 0 || x == cols - 1 || y == rows - 1) { buf_ch(out, ' '); continue; }
                const float *T = st->tones;
#define TT(dx, dy) T[(size_t)(y + (dy)) * cols + (x + (dx))]
                float gx = -TT(-1, -1) - 2 * TT(-1, 0) - TT(-1, 1) + TT(1, -1) + 2 * TT(1, 0) + TT(1, 1);
                float gy = -TT(-1, -1) - 2 * TT(0, -1) - TT(1, -1) + TT(-1, 1) + 2 * TT(0, 1) + TT(1, 1);
#undef TT
                float mag = sqrtf(gx * gx + gy * gy);
                if (mag < s->edge_thr) { buf_ch(out, ' '); continue; }
                float a = atan2f(gy * s->cell_aspect, gx); /* cells are taller than they are wide */
                if (a < 0) a += PI;
                char c = a < PI / 8 || a >= 7 * PI / 8 ? '|' : a < 3 * PI / 8 ? '/' : a < 5 * PI / 8 ? '-' : '\\';
                float k = clampf(0.45f + 0.55f * (mag - s->edge_thr) / (s->edge_thr * 2), 0.45f, 1);
                int key = (int)(k * 16);
                if (key != last) { buf_color(out, false, s->truecolor, (int)(121 * k), (int)(192 * k), (int)(255 * k)); last = key; }
                buf_ch(out, c);
            } else { /* MODE_MATRIX */
                float head = st->rain[x];
                float dist = head - y;
                float boost = dist >= 0 && dist < 14 ? 1 - dist / 14 : 0;
                float v = clampf(t * 0.85f + boost * 0.45f, 0, 1);
                if (v < 0.12f) { buf_ch(out, ' '); continue; }
                int r = 0, g = (int)(40 + v * 215), b = (int)(v * 70);
                if (dist >= 0 && dist < 1) { r = 200; g = 255; b = 200; } /* bright raindrop head */
                int key = (r << 16) | (g << 8) | b;
                if (key != last) { buf_color(out, false, s->truecolor, r, g, b); last = key; }
                uint32_t h = hash3((uint32_t)x, (uint32_t)y, (uint32_t)(st->frame / 5 + x * 7));
                buf_str(out, MATRIX_GLYPHS[h % MATRIX_GLYPH_COUNT]);
            }
        }
        buf_str(out, "\x1b[0m");
        last = -1;
        if (y < rows - 1) buf_str(out, "\r\n");
    }

    if (s->mode == MODE_MATRIX) {
        for (int x = 0; x < cols; x++) {
            st->rain[x] += st->speed[x];
            if (st->rain[x] > rows + 14) {
                st->rain[x] = -(float)(rand() % rows);
                st->speed[x] = 0.3f + (rand() % 100) / 140.f;
            }
        }
    }
}

/* Half blocks: top pixel = foreground of '▀', bottom pixel = background. */
static void render_blocks(Buf *out, State *st, const Settings *s, const uint8_t *rgb, int sw, int sh, int cols, int rows) {
    int gh = rows * 2;
    size_t n = (size_t)cols * gh;
    st->grid = grow(st->grid, &st->grid_cap, n, sizeof(Px));
    sample_grid(rgb, sw, sh, cols, gh, s->cell_aspect / 2, s->mirror, st->grid);
    auto_levels(st->grid, (int)n, &st->lo, &st->hi);

    for (int y = 0; y < rows; y++) {
        int last_fg = -1, last_bg = -1;
        for (int x = 0; x < cols; x++) {
            const Px *top = &st->grid[(size_t)(2 * y) * cols + x];
            const Px *bot = &st->grid[(size_t)(2 * y + 1) * cols + x];
            int r1, g1, b1, r2, g2, b2;
            tone_rgb(top, tone(top, st->lo, st->hi, s->contrast), &r1, &g1, &b1);
            tone_rgb(bot, tone(bot, st->lo, st->hi, s->contrast), &r2, &g2, &b2);
            if (s->invert) {
                r1 = 255 - r1; g1 = 255 - g1; b1 = 255 - b1;
                r2 = 255 - r2; g2 = 255 - g2; b2 = 255 - b2;
            }
            int kf = ((r1 >> 2) << 12) | ((g1 >> 2) << 6) | (b1 >> 2);
            int kb = ((r2 >> 2) << 12) | ((g2 >> 2) << 6) | (b2 >> 2);
            if (kf != last_fg) { buf_color(out, false, s->truecolor, r1, g1, b1); last_fg = kf; }
            if (kb != last_bg) { buf_color(out, true, s->truecolor, r2, g2, b2); last_bg = kb; }
            buf_str(out, "\xe2\x96\x80"); /* ▀ */
        }
        buf_str(out, "\x1b[0m");
        if (y < rows - 1) buf_str(out, "\r\n");
    }
}

void render_frame(Buf *out, State *st, const Settings *s, const uint8_t *rgb, int w, int h, int cols, int rows) {
    if (s->mode == MODE_BLOCKS) render_blocks(out, st, s, rgb, w, h, cols, rows);
    else render_chars(out, st, s, rgb, w, h, cols, rows);
    st->frame++;
}

void state_free(State *st) {
    free(st->grid);
    free(st->tones);
    free(st->rain);
    free(st->speed);
    memset(st, 0, sizeof *st);
}
