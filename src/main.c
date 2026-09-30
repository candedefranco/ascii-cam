// ascii-cam — your webcam, live, as ASCII art in the terminal.
#include <errno.h>
#include <math.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include "camera.h"

#define TARGET_FPS 30

/* ------------------------------------------------------------------ */
/* Settings                                                            */
/* ------------------------------------------------------------------ */

typedef enum { MODE_ASCII, MODE_COLOR, MODE_BLOCKS, MODE_EDGES, MODE_MATRIX, MODE_COUNT } Mode;
static const char *MODE_NAMES[MODE_COUNT] = {"ascii", "color", "blocks", "edges", "matrix"};

typedef struct {
    Mode mode;
    bool invert;
    bool mirror;
    bool show_bar;
    bool truecolor;
    float contrast;  /* 0.5 .. 3.0 */
    float edge_thr;  /* gradient magnitude needed to draw an edge */
} Settings;

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

/* ------------------------------------------------------------------ */
/* Output buffer: one write() per frame avoids flicker                 */
/* ------------------------------------------------------------------ */

typedef struct {
    char *d;
    size_t n, cap;
} Buf;

static void buf_put(Buf *b, const char *s, size_t n) {
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

static void buf_str(Buf *b, const char *s) { buf_put(b, s, strlen(s)); }
static void buf_ch(Buf *b, char c) { buf_put(b, &c, 1); }

static void buf_int(Buf *b, int v) {
    char tmp[12];
    int i = sizeof tmp;
    if (v == 0) tmp[--i] = '0';
    while (v > 0) { tmp[--i] = (char)('0' + v % 10); v /= 10; }
    buf_put(b, tmp + i, sizeof tmp - i);
}

/* Sets foreground (bg=false) or background color; falls back to the 256-color cube. */
static void buf_color(Buf *b, bool bg, bool truecolor, int r, int g, int bl) {
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

static void write_all(const char *d, size_t n) {
    while (n > 0) {
        ssize_t w = write(STDOUT_FILENO, d, n);
        if (w < 0) {
            if (errno == EINTR) continue;
            return;
        }
        d += w;
        n -= (size_t)w;
    }
}

/* ------------------------------------------------------------------ */
/* Terminal                                                            */
/* ------------------------------------------------------------------ */

static struct termios g_orig_termios;
static volatile sig_atomic_t g_resized = 1, g_quit = 0;

static void term_restore(void) {
    const char *s = "\x1b[0m\x1b[?25h\x1b[?1049l";
    write_all(s, strlen(s));
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &g_orig_termios);
}

static void on_signal(int sig) {
    if (sig == SIGWINCH) g_resized = 1;
    else g_quit = 1;
}

static void term_setup(void) {
    tcgetattr(STDIN_FILENO, &g_orig_termios);
    struct termios raw = g_orig_termios;
    raw.c_lflag &= ~(ECHO | ICANON);
    raw.c_cc[VMIN] = 0; /* non-blocking reads */
    raw.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
    atexit(term_restore);

    signal(SIGWINCH, on_signal);
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    const char *s = "\x1b[?1049h\x1b[?25l\x1b[2J"; /* alt screen, hide cursor, clear */
    write_all(s, strlen(s));
}

static void term_size(int *cols, int *rows) {
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0) {
        *cols = ws.ws_col;
        *rows = ws.ws_row;
    } else {
        *cols = 80;
        *rows = 24;
    }
}

static double now_sec(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

/* ------------------------------------------------------------------ */
/* Image sampling                                                      */
/* ------------------------------------------------------------------ */

typedef struct {
    float r, g, b; /* 0..1 */
    float y;       /* luminance 0..1 */
} Px;

static float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

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

/* ------------------------------------------------------------------ */
/* Demo source (no camera needed)                                      */
/* ------------------------------------------------------------------ */

static void demo_frame(uint8_t *rgb, int w, int h, double t) {
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

/* ------------------------------------------------------------------ */
/* Renderers                                                           */
/* ------------------------------------------------------------------ */

typedef struct {
    Px *grid;
    size_t grid_cap;
    float *tones;
    size_t tones_cap;
    float *rain;   /* matrix mode: head position per column */
    float *speed;
    int rain_cols;
    float lo, hi;
    unsigned long frame;
} State;

static void *grow(void *p, size_t *cap, size_t need, size_t elem) {
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
    sample_grid(rgb, sw, sh, cols, rows, 2.0f, s->mirror, st->grid);
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
                float a = atan2f(gy * 2.0f, gx); /* cells are twice as tall as wide */
                if (a < 0) a += (float)M_PI;
                char c = a < M_PI / 8 || a >= 7 * M_PI / 8 ? '|' : a < 3 * M_PI / 8 ? '/' : a < 5 * M_PI / 8 ? '-' : '\\';
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
    sample_grid(rgb, sw, sh, cols, gh, 1.0f, s->mirror, st->grid);
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

/* ------------------------------------------------------------------ */
/* Status bar, snapshots, input                                        */
/* ------------------------------------------------------------------ */

static void render_bar(Buf *out, const Settings *s, int cols, double fps, const char *msg) {
    char line[512];
    int n;
    if (msg && *msg) {
        n = snprintf(line, sizeof line, " %s", msg);
    } else {
        n = snprintf(line, sizeof line,
                     " ascii-cam │ [1-5] %s │ %s %.1f │ [i]nvert %s │ [m]irror │ [s]napshot │ [h]ide │ [q]uit │ %2.0f fps",
                     MODE_NAMES[s->mode], s->mode == MODE_EDGES ? "[+/-] edges" : "[+/-] contrast",
                     s->mode == MODE_EDGES ? s->edge_thr : s->contrast, s->invert ? "on" : "off", fps);
    }
    if (n < 0) n = 0;
    buf_str(out, "\r\n\x1b[0m\x1b[7m");
    /* Pad to the full width (count display columns, not bytes: "│" is 3 bytes). */
    int visible = 0;
    for (int i = 0; i < n && line[i]; i++)
        if ((line[i] & 0xC0) != 0x80) visible++;
    int keep = n;
    if (visible > cols) { /* truncate on a character boundary */
        int seen = 0;
        for (keep = 0; keep < n; keep++) {
            if ((line[keep] & 0xC0) != 0x80 && ++seen > cols) break;
        }
        visible = cols;
    }
    buf_put(out, line, (size_t)keep);
    for (int i = visible; i < cols; i++) buf_ch(out, ' ');
    buf_str(out, "\x1b[0m");
}

static bool save_snapshot(const char *frame, size_t len, char *name, size_t name_len) {
    time_t t = time(NULL);
    strftime(name, name_len, "ascii-cam-%Y%m%d-%H%M%S.ans", localtime(&t));
    FILE *f = fopen(name, "wb");
    if (!f) return false;
    fwrite(frame, 1, len, f);
    fputs("\x1b[0m\n", f);
    fclose(f);
    return true;
}

static void usage(const char *argv0) {
    printf("usage: %s [--demo] [--mode ascii|color|blocks|edges|matrix]\n\n"
           "  --demo        animated test pattern instead of the camera\n"
           "  --mode NAME   starting mode (default: color)\n\n"
           "keys: 1-5 mode · +/- contrast (edge sensitivity in edges mode) · i invert\n"
           "      m mirror · s snapshot to .ans file · h hide status bar · q quit\n",
           argv0);
}

int main(int argc, char **argv) {
    Settings s = {
        .mode = MODE_COLOR, .mirror = true, .show_bar = true, .contrast = 1.2f, .edge_thr = 0.55f,
    };
    const char *ct = getenv("COLORTERM");
    s.truecolor = ct && (strstr(ct, "truecolor") || strstr(ct, "24bit"));

    bool demo = false;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--demo")) {
            demo = true;
        } else if (!strcmp(argv[i], "--mode") && i + 1 < argc) {
            const char *m = argv[++i];
            bool found = false;
            for (int k = 0; k < MODE_COUNT; k++)
                if (!strcmp(m, MODE_NAMES[k])) { s.mode = (Mode)k; found = true; }
            if (!found) { fprintf(stderr, "unknown mode: %s\n", m); usage(argv[0]); return 2; }
        } else {
            usage(argv[0]);
            return !strcmp(argv[i], "--help") || !strcmp(argv[i], "-h") ? 0 : 2;
        }
    }

    if (!demo) {
        int err = cam_open();
        if (err != CAM_OK) {
            fprintf(stderr,
                    err == CAM_ERR_DENIED   ? "Camera access denied. Allow your terminal app in System Settings → "
                                              "Privacy & Security → Camera, then try again.\n"
                    : err == CAM_ERR_NODEVICE ? "No camera found. Try --demo.\n"
                                              : "Could not open the camera. Try --demo.\n");
            return 1;
        }
    }

    term_setup();

    Buf out = {0};
    State st = {.lo = 0, .hi = 1};
    uint8_t *frame = NULL;
    size_t frame_cap = 0;
    int fw = 0, fh = 0, cols = 80, rows = 24;
    bool have_frame = false;
    double fps = 0, last = now_sec(), msg_until = 0, t0 = last;
    char msg[160] = "";

    while (!g_quit) {
        double start = now_sec();

        if (g_resized) {
            g_resized = 0;
            term_size(&cols, &rows);
            write_all("\x1b[2J", 4);
        }

        char c;
        while (read(STDIN_FILENO, &c, 1) == 1) {
            if (c == 'q' || c == 27) g_quit = 1;
            else if (c >= '1' && c < '1' + MODE_COUNT) s.mode = (Mode)(c - '1');
            else if (c == 'i') s.invert = !s.invert;
            else if (c == 'm') s.mirror = !s.mirror;
            else if (c == 'h') { s.show_bar = !s.show_bar; g_resized = 1; }
            else if (c == '+' || c == '=') {
                if (s.mode == MODE_EDGES) s.edge_thr = clampf(s.edge_thr - 0.05f, 0.1f, 2.0f);
                else s.contrast = clampf(s.contrast + 0.1f, 0.5f, 3.0f);
            } else if (c == '-' || c == '_') {
                if (s.mode == MODE_EDGES) s.edge_thr = clampf(s.edge_thr + 0.05f, 0.1f, 2.0f);
                else s.contrast = clampf(s.contrast - 0.1f, 0.5f, 3.0f);
            } else if (c == 's' && out.n > 3) {
                char name[64];
                size_t frame_len = out.n; /* snapshot the image rows only, not the bar */
                for (size_t k = out.n; k > 3; k--)
                    if (out.d[k - 1] == '\n' && s.show_bar) { frame_len = k - 2; break; }
                if (save_snapshot(out.d + 3, frame_len - 3, name, sizeof name))
                    snprintf(msg, sizeof msg, "saved %s  (view it with: cat %s)", name, name);
                else
                    snprintf(msg, sizeof msg, "could not save snapshot");
                msg_until = start + 2.5;
            }
        }
        if (g_quit) break;

        if (demo) {
            fw = 640; fh = 480;
            frame = grow(frame, &frame_cap, (size_t)fw * fh * 3, 1);
            demo_frame(frame, fw, fh, start - t0);
            have_frame = true;
        } else if (cam_copy(&frame, &frame_cap, &fw, &fh)) {
            have_frame = true;
        }

        int img_rows = rows - (s.show_bar ? 1 : 0);
        if (img_rows < 1) img_rows = 1;

        out.n = 0;
        buf_str(&out, "\x1b[H");
        if (!have_frame) {
            buf_str(&out, "waiting for camera…");
        } else if (s.mode == MODE_BLOCKS) {
            render_blocks(&out, &st, &s, frame, fw, fh, cols, img_rows);
        } else {
            render_chars(&out, &st, &s, frame, fw, fh, cols, img_rows);
        }
        if (s.show_bar) render_bar(&out, &s, cols, fps, start < msg_until ? msg : NULL);
        write_all(out.d, out.n);
        st.frame++;

        double end = now_sec();
        double dt = end - last;
        last = end;
        if (dt > 0) fps = fps * 0.9 + (1.0 / dt) * 0.1;
        double wait = 1.0 / TARGET_FPS - (end - start);
        if (wait > 0) usleep((useconds_t)(wait * 1e6));
    }

    if (!demo) cam_close();
    free(out.d);
    free(frame);
    free(st.grid);
    free(st.tones);
    free(st.rain);
    free(st.speed);
    return 0;
}
