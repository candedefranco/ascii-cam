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
#include "render.h"

#define TARGET_FPS 30

/* ------------------------------------------------------------------ */
/* Terminal                                                          */
/* ------------------------------------------------------------------ */
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
/* Status bar, snapshots, input                                     */
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
        .mode = MODE_COLOR, .mirror = true, .show_bar = true, .contrast = 1.2f, .edge_thr = 0.55f, .cell_aspect = 2.0f,
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
        } else {
            render_frame(&out, &st, &s, frame, fw, fh, cols, img_rows);
        }
        if (s.show_bar) render_bar(&out, &s, cols, fps, start < msg_until ? msg : NULL);
        write_all(out.d, out.n);

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
    state_free(&st);
    return 0;
}
