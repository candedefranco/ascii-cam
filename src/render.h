#ifndef ASCII_CAM_RENDER_H
#define ASCII_CAM_RENDER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>


typedef enum { MODE_ASCII, MODE_COLOR, MODE_BLOCKS, MODE_EDGES, MODE_MATRIX, MODE_COUNT } Mode;
extern const char *MODE_NAMES[MODE_COUNT];

typedef struct {
    Mode mode;
    bool invert;
    bool mirror;
    bool show_bar;
    bool truecolor;
    float contrast;  /* 0.5 .. 3.0 */
    float edge_thr;  /* gradient magnitude needed to draw an edge */
    float cell_aspect; /* height/width of one character cell (~2 in most terminals) */
} Settings;

typedef struct {
    char *d;
    size_t n, cap;
} Buf;

void buf_put(Buf *b, const char *s, size_t n);
void buf_str(Buf *b, const char *s);
void buf_ch(Buf *b, char c);
void buf_int(Buf *b, int v);
void buf_color(Buf *b, bool bg, bool truecolor, int r, int g, int bl);

typedef struct {
    float r, g, b; /* 0..1 */
    float y;       /* luminance 0..1 */
} Px;

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

/* Renders one frame (packed RGB, w x h) into `out` as a cols x rows grid of ANSI text. */
void render_frame(Buf *out, State *st, const Settings *s, const uint8_t *rgb, int w, int h, int cols, int rows);

/* Animated test pattern, for running without a camera. */
void demo_frame(uint8_t *rgb, int w, int h, double t);

float clampf(float v, float lo, float hi);
void *grow(void *p, size_t *cap, size_t need, size_t elem);
void state_free(State *st);

#endif
