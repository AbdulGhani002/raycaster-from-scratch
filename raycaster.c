/*
 * raycaster.c — Wolfenstein-style software raycaster, from scratch.
 *
 * Pure C. No third-party libraries, no engine, no asset files.
 * The only external calls are to the OS itself (user32/gdi32) to open a
 * window and copy a finished framebuffer to the screen — everything else
 * (ray marching, textures, shading, input, timing, screenshot encoding)
 * is hand-written below.
 *
 * Build (MinGW / w64devkit):
 *   gcc -O2 -Wall -Wextra -o raycaster.exe raycaster.c -luser32 -lgdi32
 * Build (MSVC):
 *   cl /O2 raycaster.c user32.lib gdi32.lib
 *
 * Run:
 *   raycaster.exe                 play
 *   raycaster.exe --screenshot f  render one frame to f (BMP), then exit
 *   raycaster.exe --selftest 5    run 5 seconds, print avg FPS, then exit
 */

#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SCRW 960
#define SCRH 540
#define MAPW 24
#define MAPH 24
#define TEXSZ 64            /* textures are 64x64, power of two */
#define TEXN 7              /* 0 floor, 1..5 walls, 6 ceiling */

#define MOUSE_SENS 0.0018               /* radians per raw mouse count */

static uint32_t fb[SCRW * SCRH];        /* 0x00RRGGBB, top-down rows */
static uint32_t tex[TEXN][TEXSZ * TEXSZ];
static int world[MAPH][MAPW];           /* [y][x], 0 = empty */
static int g_keys[256];
static long g_mouseDX;                  /* accumulated raw deltas per frame */
static int g_cursorHidden;
static int g_captureOn = 1;             /* off in --selftest runs */

/* --- the game: a timed gold hunt ------------------------------------- */

#define NGOLD 10
#define TRANSP 0x00FF00FFu              /* sprite color key = transparent */

static double zbuf[SCRW];               /* wall depth per column, for sprites */
static uint32_t spr_gold[TEXSZ * TEXSZ];
static struct { double x, y; int taken; } gold[NGOLD] = {
    { 5.5, 12.5, 0 },                   /* corridor, between the first pillars */
    { 7.5, 4.5, 0 },                    /* control room */
    { 12.5, 3.5, 0 },                   /* control room, far corner */
    { 5.5, 19.5, 0 },                   /* storeroom */
    { 12.5, 16.5, 0 },                  /* storeroom corner */
    { 17.5, 3.5, 0 },                   /* great hall, north-west */
    { 21.5, 10.5, 0 },                  /* great hall, east wall */
    { 19.5, 19.5, 0 },                  /* great hall, south */
    { 14.5, 13.5, 0 },                  /* corridor alcove behind a pillar */
    { 1.5, 13.5, 0 },                   /* behind the spawn point */
};
static int goldCount, won;
static double gameTime;                 /* freezes when you win */
static double g_time;                   /* always runs; drives animation */
static double g_flash;                  /* pickup screen-flash timer */

static double posX = 2.5, posY = 12.5;  /* player, in map cells */
static double dirX = 1.0, dirY = 0.0;
static double planeX = 0.0, planeY = 0.66;    /* FOV ~66 deg; +0.66 keeps the
                                                 view un-mirrored so left is
                                                 left on screen AND minimap */

/* ---------------------------------------------------------------- utils */

static uint32_t hash2(int x, int y)
{
    uint32_t h = (uint32_t)x * 374761393u ^ (uint32_t)y * 668265263u;
    h ^= h >> 13;
    h *= 1274126177u;
    return h ^ (h >> 16);
}

static float frand2(int x, int y)       /* deterministic noise in [0,1) */
{
    return (float)(hash2(x, y) & 0xffff) / 65536.0f;
}

static uint32_t rgb(int r, int g, int b)
{
    if (r < 0) r = 0;
    if (r > 255) r = 255;
    if (g < 0) g = 0;
    if (g > 255) g = 255;
    if (b < 0) b = 0;
    if (b > 255) b = 255;
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}

static uint32_t shade(uint32_t c, float f)
{
    int r = (int)(((c >> 16) & 255) * f);
    int g = (int)(((c >> 8) & 255) * f);
    int b = (int)((c & 255) * f);
    return rgb(r, g, b);
}

static float fogf(float d)              /* torch-lit falloff with distance */
{
    return 1.0f / (1.0f + d * 0.07f + d * d * 0.005f);
}

/* ------------------------------------------------------- procedural art */

static void gen_textures(void)
{
    int x, y;
    for (y = 0; y < TEXSZ; y++) {
        for (x = 0; x < TEXSZ; x++) {
            int i = y * TEXSZ + x;
            float sp = 0.90f + 0.20f * frand2(x, y);   /* per-pixel speckle */

            /* 0: floor — big grey tiles with grout */
            {
                int tx = x % 32, ty = y % 32;
                if (tx == 0 || ty == 0) {
                    tex[0][i] = shade(rgb(42, 42, 46), sp);
                } else {
                    float j = frand2(x / 32 + 51, y / 32 + 77);
                    int base = 100 + (int)(18.0f * j);
                    tex[0][i] = shade(rgb(base, base, base + 6), sp);
                }
            }

            /* 1: red brick — offset courses with mortar */
            {
                int bh = 16, bw = 32;
                int row = y / bh;
                int xo = (row & 1) ? bw / 2 : 0;
                int mx = (x + xo) % bw, my = y % bh;
                if (my < 2 || mx < 2) {
                    tex[1][i] = shade(rgb(180, 172, 160), sp);
                } else {
                    int bx = (x + xo) / bw;
                    float j = frand2(bx * 7 + 1, row * 13 + 2);
                    tex[1][i] = shade(rgb(150 + (int)(45 * j),
                                          52 + (int)(20 * j),
                                          44 + (int)(16 * j)), sp);
                }
            }

            /* 2: grey stone blocks */
            {
                int bh = 16, bw = 32;
                int row = y / bh;
                int xo = (row & 1) ? bw / 2 : 0;
                int mx = (x + xo) % bw, my = y % bh;
                if (my < 2 || mx < 2) {
                    tex[2][i] = shade(rgb(52, 52, 54), sp);
                } else {
                    int bx = (x + xo) / bw;
                    float j = frand2(bx * 11 + 5, row * 17 + 9);
                    int base = 118 + (int)(42.0f * j);
                    tex[2][i] = shade(rgb(base, base, base + 4), sp);
                }
            }

            /* 3: wood planks — vertical boards with grain */
            {
                int plank = x / 8;
                if (x % 8 == 0) {
                    tex[3][i] = shade(rgb(40, 26, 14), sp);
                } else {
                    float j = frand2(plank * 3 + 7, 3);
                    float grain = 0.82f + 0.18f *
                        (0.5f + 0.5f * sinf((float)y * 0.55f + 6.28f * j));
                    int base = 105 + (int)(45.0f * j);
                    tex[3][i] = shade(rgb((int)(base * grain),
                                          (int)(base * 0.60f * grain),
                                          (int)(base * 0.34f * grain)), sp);
                }
            }

            /* 4: blue-grey metal panels with bevel and rivets */
            {
                int px = x % 32, py = y % 32;
                int base_r = 58, base_g = 84, base_b = 116;
                uint32_t c = rgb(base_r + py / 4, base_g + py / 4, base_b + py / 5);
                if (px < 2 || py < 2)  c = shade(c, 1.35f);
                if (px > 29 || py > 29) c = shade(c, 0.55f);
                {   /* rivets in the panel corners */
                    int rx = (px < 16) ? 4 : 27, ry = (py < 16) ? 4 : 27;
                    int dx = px - rx, dy = py - ry, d2 = dx * dx + dy * dy;
                    if (d2 <= 2)      c = rgb(200, 216, 232);
                    else if (d2 <= 6) c = shade(c, 0.7f);
                }
                tex[4][i] = shade(c, 0.94f + 0.12f * frand2(x + 13, y + 4));
            }

            /* 5: mossy stone — stone base with green growth blotches */
            {
                uint32_t c = tex[2][i];
                float n = 0.6f * frand2(x / 8 + 31, y / 8 + 17)
                        + 0.4f * frand2(x / 4 + 3, y / 4 + 29);
                if (n > 0.52f) {
                    float m = (n - 0.52f) * 2.1f;
                    if (m > 1.0f) m = 1.0f;
                    int r = (int)(((c >> 16) & 255) * (1 - m) + 58 * m);
                    int g = (int)(((c >> 8) & 255) * (1 - m) + 112 * m);
                    int b = (int)((c & 255) * (1 - m) + 48 * m);
                    c = rgb(r, g, b);
                }
                tex[5][i] = c;
            }

            /* 6: ceiling — dark concrete with panel seams */
            {
                uint32_t c = rgb(46, 46, 54);
                if (x % 32 == 0 || y % 32 == 0) c = rgb(34, 34, 40);
                tex[6][i] = shade(c, 0.88f + 0.24f * frand2(x + 91, y + 57));
            }
        }
    }
}

/* three stacked gold mounds with a sparkle, on a color-keyed background */
static void gen_sprites(void)
{
    static const struct { int cx, cy, rx, ry; } m[3] = {
        { 32, 46, 21, 9 }, { 32, 37, 15, 7 }, { 32, 29, 10, 5 }
    };
    int x, y, i;
    for (y = 0; y < TEXSZ; y++) {
        for (x = 0; x < TEXSZ; x++) {
            uint32_t c = TRANSP;
            for (i = 0; i < 3; i++) {
                float dx = (x - m[i].cx) / (float)m[i].rx;
                float dy = (y - m[i].cy) / (float)m[i].ry;
                float d = dx * dx + dy * dy;
                if (d <= 1.0f) {
                    if (d > 0.72f)         c = rgb(148, 96, 18);
                    else if (dy < -0.15f)  c = rgb(248, 214, 88);
                    else                   c = rgb(214, 164, 40);
                    c = shade(c, 0.90f + 0.20f * frand2(x + i * 97, y + i * 31));
                }
            }
            if ((x == 46 && y >= 14 && y <= 20) ||
                (y == 17 && x >= 43 && x <= 49))
                c = rgb(255, 244, 180);
            spr_gold[y * TEXSZ + x] = c;
        }
    }
}

/* ------------------------------------------------- 5x7 bitmap font, HUD */

static const uint8_t font5x7[][7] = {
    {0x0E,0x11,0x11,0x1F,0x11,0x11,0x11}, /* A */
    {0x1E,0x11,0x11,0x1E,0x11,0x11,0x1E}, /* B */
    {0x0E,0x11,0x10,0x10,0x10,0x11,0x0E}, /* C */
    {0x1C,0x12,0x11,0x11,0x11,0x12,0x1C}, /* D */
    {0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F}, /* E */
    {0x1F,0x10,0x10,0x1E,0x10,0x10,0x10}, /* F */
    {0x0E,0x11,0x10,0x17,0x11,0x11,0x0F}, /* G */
    {0x11,0x11,0x11,0x1F,0x11,0x11,0x11}, /* H */
    {0x0E,0x04,0x04,0x04,0x04,0x04,0x0E}, /* I */
    {0x07,0x02,0x02,0x02,0x02,0x12,0x0C}, /* J */
    {0x11,0x12,0x14,0x18,0x14,0x12,0x11}, /* K */
    {0x10,0x10,0x10,0x10,0x10,0x10,0x1F}, /* L */
    {0x11,0x1B,0x15,0x15,0x11,0x11,0x11}, /* M */
    {0x11,0x11,0x19,0x15,0x13,0x11,0x11}, /* N */
    {0x0E,0x11,0x11,0x11,0x11,0x11,0x0E}, /* O */
    {0x1E,0x11,0x11,0x1E,0x10,0x10,0x10}, /* P */
    {0x0E,0x11,0x11,0x11,0x15,0x12,0x0D}, /* Q */
    {0x1E,0x11,0x11,0x1E,0x14,0x12,0x11}, /* R */
    {0x0F,0x10,0x10,0x0E,0x01,0x01,0x1E}, /* S */
    {0x1F,0x04,0x04,0x04,0x04,0x04,0x04}, /* T */
    {0x11,0x11,0x11,0x11,0x11,0x11,0x0E}, /* U */
    {0x11,0x11,0x11,0x11,0x11,0x0A,0x04}, /* V */
    {0x11,0x11,0x11,0x15,0x15,0x15,0x0A}, /* W */
    {0x11,0x11,0x0A,0x04,0x0A,0x11,0x11}, /* X */
    {0x11,0x11,0x0A,0x04,0x04,0x04,0x04}, /* Y */
    {0x1F,0x01,0x02,0x04,0x08,0x10,0x1F}, /* Z */
    {0x0E,0x11,0x13,0x15,0x19,0x11,0x0E}, /* 0 */
    {0x04,0x0C,0x04,0x04,0x04,0x04,0x0E}, /* 1 */
    {0x0E,0x11,0x01,0x02,0x04,0x08,0x1F}, /* 2 */
    {0x0E,0x11,0x01,0x06,0x01,0x11,0x0E}, /* 3 */
    {0x02,0x06,0x0A,0x12,0x1F,0x02,0x02}, /* 4 */
    {0x1F,0x10,0x1E,0x01,0x01,0x11,0x0E}, /* 5 */
    {0x06,0x08,0x10,0x1E,0x11,0x11,0x0E}, /* 6 */
    {0x1F,0x01,0x02,0x04,0x08,0x08,0x08}, /* 7 */
    {0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E}, /* 8 */
    {0x0E,0x11,0x11,0x0F,0x01,0x02,0x0C}, /* 9 */
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00}, /* space */
    {0x00,0x00,0x00,0x00,0x00,0x0C,0x0C}, /* . */
    {0x01,0x01,0x02,0x04,0x08,0x10,0x10}, /* / */
    {0x04,0x04,0x04,0x04,0x04,0x00,0x04}, /* ! */
};

static int font_idx(char c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= '0' && c <= '9') return 26 + (c - '0');
    switch (c) {
    case '.': return 37;
    case '/': return 38;
    case '!': return 39;
    }
    return 36;                          /* space */
}

static void draw_char(int x, int y, int sc, uint32_t col, char ch)
{
    const uint8_t *g = font5x7[font_idx(ch)];
    int r, b, i, j;
    for (r = 0; r < 7; r++)
        for (b = 0; b < 5; b++) {
            if (!(g[r] & (0x10 >> b))) continue;
            for (i = 0; i < sc; i++)
                for (j = 0; j < sc; j++) {
                    int px = x + b * sc + j, py = y + r * sc + i;
                    if (px >= 0 && px < SCRW && py >= 0 && py < SCRH)
                        fb[(size_t)py * SCRW + px] = col;
                }
        }
}

static void draw_text(int x, int y, int sc, uint32_t col, const char *s)
{
    const char *p;
    int cx;
    for (p = s, cx = x; *p; p++, cx += 6 * sc)
        draw_char(cx + 2, y + 2, sc, rgb(12, 12, 12), *p);    /* shadow */
    for (p = s, cx = x; *p; p++, cx += 6 * sc)
        draw_char(cx, y, sc, col, *p);
}

static int text_w(int sc, const char *s)
{
    return (int)strlen(s) * 6 * sc - sc;
}

/* -------------------------------------------------------------- the map */

static void hwall(int y, int x0, int x1, int t)
{
    int x;
    for (x = x0; x <= x1; x++) world[y][x] = t;
}

static void vwall(int x, int y0, int y1, int t)
{
    int y;
    for (y = y0; y <= y1; y++) world[y][x] = t;
}

static void fillrect(int x0, int y0, int x1, int y1, int t)
{
    int x, y;
    for (y = y0; y <= y1; y++)
        for (x = x0; x <= x1; x++) world[y][x] = t;
}

static void box(int x0, int y0, int x1, int y1, int t)
{
    hwall(y0, x0, x1, t);
    hwall(y1, x0, x1, t);
    vwall(x0, y0, y1, t);
    vwall(x1, y0, y1, t);
}

static void gen_map(void)
{
    memset(world, 0, sizeof world);
    box(0, 0, MAPW - 1, MAPH - 1, 2);          /* stone outer border */

    fillrect(1, 1, 15, 9, 2);                  /* solid rock, rooms carved out */
    fillrect(1, 15, 15, 22, 2);
    fillrect(16, 1, 22, 1, 2);
    fillrect(16, 22, 22, 22, 2);

    /* main corridor, west to east, with brick pillar rhythm */
    hwall(10, 1, 16, 1);
    hwall(14, 1, 16, 1);
    world[11][5] = 1;  world[13][5] = 1;
    world[11][9] = 1;  world[13][9] = 1;
    world[11][13] = 1; world[13][13] = 1;

    /* north wing: metal control room */
    box(4, 2, 13, 9, 4);
    fillrect(5, 3, 12, 8, 0);
    world[5][6] = 4;  world[5][11] = 4;

    /* south wing: wooden storeroom */
    box(4, 15, 13, 21, 3);
    fillrect(5, 16, 12, 20, 0);
    world[18][7] = 3; world[18][10] = 3;

    /* east: great mossy hall */
    box(16, 2, 22, 21, 5);
    world[6][18] = 5;  world[6][20] = 5;
    world[17][18] = 5; world[17][20] = 5;

    /* doorways (carved last so nothing overwrites them) */
    world[9][8] = 0;  world[9][9] = 0;         /* corridor -> control room */
    world[10][8] = 0; world[10][9] = 0;
    world[14][8] = 0; world[14][9] = 0;        /* corridor -> storeroom */
    world[15][8] = 0; world[15][9] = 0;
    world[11][16] = 0; world[12][16] = 0;      /* corridor -> great hall */
    world[13][16] = 0;
}

static int cell(int x, int y)
{
    if (x < 0 || x >= MAPW || y < 0 || y >= MAPH) return 2;
    return world[y][x];
}

/* ------------------------------------------------------------ rendering */

static void render_frame(void)
{
    int x, y;
    memset(fb, 0, sizeof fb);

    /* floor and ceiling: one perspective-projected texture row per scanline */
    {
        double rdx0 = dirX - planeX, rdy0 = dirY - planeY;
        double rdx1 = dirX + planeX, rdy1 = dirY + planeY;
        for (y = SCRH / 2 + 1; y < SCRH; y++) {
            int p = y - SCRH / 2;
            float rowDist = (0.5f * SCRH) / (float)p;
            float fsx = rowDist * (float)(rdx1 - rdx0) / SCRW;
            float fsy = rowDist * (float)(rdy1 - rdy0) / SCRW;
            float fx = (float)posX + rowDist * (float)rdx0;
            float fy = (float)posY + rowDist * (float)rdy0;
            float f = fogf(rowDist);
            uint32_t *rowF = fb + (size_t)y * SCRW;
            uint32_t *rowC = fb + (size_t)(SCRH - 1 - y) * SCRW;
            for (x = 0; x < SCRW; x++) {
                int tx = (int)(fx * TEXSZ) & (TEXSZ - 1);
                int ty = (int)(fy * TEXSZ) & (TEXSZ - 1);
                rowF[x] = shade(tex[0][ty * TEXSZ + tx], f);
                rowC[x] = shade(tex[6][ty * TEXSZ + tx], f * 0.9f);
                fx += fsx;
                fy += fsy;
            }
        }
    }

    /* walls: one DDA ray per screen column */
    for (x = 0; x < SCRW; x++) {
        double cameraX = 2.0 * x / SCRW - 1.0;
        double rdx = dirX + planeX * cameraX;
        double rdy = dirY + planeY * cameraX;
        int mapX = (int)posX, mapY = (int)posY;
        double ddx = (rdx == 0.0) ? 1e30 : fabs(1.0 / rdx);
        double ddy = (rdy == 0.0) ? 1e30 : fabs(1.0 / rdy);
        double sdx, sdy;
        int stepX, stepY, side = 0, type = 2, iter;

        if (rdx < 0) { stepX = -1; sdx = (posX - mapX) * ddx; }
        else         { stepX = 1;  sdx = (mapX + 1.0 - posX) * ddx; }
        if (rdy < 0) { stepY = -1; sdy = (posY - mapY) * ddy; }
        else         { stepY = 1;  sdy = (mapY + 1.0 - posY) * ddy; }

        for (iter = 0; iter < 128; iter++) {
            if (sdx < sdy) { sdx += ddx; mapX += stepX; side = 0; }
            else           { sdy += ddy; mapY += stepY; side = 1; }
            type = cell(mapX, mapY);
            if (type > 0) break;
        }

        {
            double pwd = (side == 0) ? sdx - ddx : sdy - ddy;
            int lh, ds, de, texX;
            double wallX, step, texPos;
            const uint32_t *T = tex[(type >= 1 && type <= 5) ? type : 2];
            float lit;

            if (pwd < 1e-6) pwd = 1e-6;
            lh = (int)(SCRH / pwd);
            ds = SCRH / 2 - lh / 2;
            de = SCRH / 2 + lh / 2;
            if (ds < 0) ds = 0;
            if (de > SCRH) de = SCRH;

            wallX = (side == 0) ? posY + pwd * rdy : posX + pwd * rdx;
            wallX -= floor(wallX);
            texX = (int)(wallX * TEXSZ);
            if ((side == 0 && rdx > 0) || (side == 1 && rdy < 0))
                texX = TEXSZ - 1 - texX;
            if (texX < 0) texX = 0;
            if (texX >= TEXSZ) texX = TEXSZ - 1;

            step = (double)TEXSZ / lh;
            texPos = (ds - SCRH / 2.0 + lh / 2.0) * step;
            lit = fogf((float)pwd) * (side == 1 ? 0.72f : 1.0f);
            zbuf[x] = pwd;              /* sprites test against this */

            for (y = ds; y < de; y++) {
                int ty = (int)texPos & (TEXSZ - 1);
                texPos += step;
                fb[(size_t)y * SCRW + x] = shade(T[ty * TEXSZ + texX], lit);
            }
        }
    }

    /* gold sprites: billboards, far to near, clipped by the wall z-buffer */
    {
        int order[NGOLD];
        double dist[NGOLD];
        int n = 0, i, j;
        for (i = 0; i < NGOLD; i++) {
            if (gold[i].taken) continue;
            order[n] = i;
            dist[n] = (posX - gold[i].x) * (posX - gold[i].x)
                    + (posY - gold[i].y) * (posY - gold[i].y);
            n++;
        }
        for (i = 1; i < n; i++) {       /* insertion sort, farthest first */
            int oi = order[i];
            double di = dist[i];
            j = i;
            while (j > 0 && dist[j - 1] < di) {
                dist[j] = dist[j - 1];
                order[j] = order[j - 1];
                j--;
            }
            dist[j] = di;
            order[j] = oi;
        }
        for (i = 0; i < n; i++) {
            int s = order[i];
            double sx = gold[s].x - posX, sy = gold[s].y - posY;
            double invDet = 1.0 / (planeX * dirY - dirX * planeY);
            double tX = invDet * (dirY * sx - dirX * sy);
            double tY = invDet * (-planeY * sx + planeX * sy);
            double bob = 10.0 * (0.5 + 0.5 * sin(g_time * 2.5 + s * 1.3));
            int scrX, vMove, sh, sw, dsx, dsy, x0, x1, y0, y1, px, py;
            float lit;
            if (tY <= 0.1) continue;
            scrX = (int)((SCRW / 2.0) * (1.0 + tX / tY));
            vMove = (int)((SCRH * 0.22 - bob) / tY);    /* rest near floor */
            sh = (int)(SCRH / tY / 1.9);
            if (sh <= 0) continue;
            sw = sh;
            dsy = SCRH / 2 - sh / 2 + vMove;
            dsx = scrX - sw / 2;
            x0 = dsx < 0 ? 0 : dsx;
            x1 = dsx + sw > SCRW ? SCRW : dsx + sw;
            y0 = dsy < 0 ? 0 : dsy;
            y1 = dsy + sh > SCRH ? SCRH : dsy + sh;
            lit = fogf((float)tY);
            for (px = x0; px < x1; px++) {
                int texX = (px - dsx) * TEXSZ / sw;
                if (tY >= zbuf[px]) continue;
                for (py = y0; py < y1; py++) {
                    int texY = (py - dsy) * TEXSZ / sh;
                    uint32_t c = spr_gold[texY * TEXSZ + texX];
                    if (c != TRANSP)
                        fb[(size_t)py * SCRW + px] = shade(c, lit);
                }
            }
        }
    }

    /* pickup flash: brief golden wash over the frame */
    if (g_flash > 0) {
        float a = (float)(g_flash / 0.18) * 0.30f;
        size_t k;
        for (k = 0; k < (size_t)SCRW * SCRH; k++) {
            uint32_t c = fb[k];
            int r = (c >> 16) & 255, g = (c >> 8) & 255, b = c & 255;
            fb[k] = rgb(r + (int)((255 - r) * a),
                        g + (int)((208 - g) * a),
                        b + (int)((80 - b) * a));
        }
    }

    /* minimap overlay, top-left */
    {
        const int S = 5, OX = 10, OY = 10;
        int mx, my, i;
        for (my = 0; my < MAPH; my++) {
            for (mx = 0; mx < MAPW; mx++) {
                uint32_t c;
                switch (world[my][mx]) {
                case 1:  c = rgb(168, 62, 46);   break;
                case 2:  c = rgb(150, 150, 152); break;
                case 3:  c = rgb(138, 90, 46);   break;
                case 4:  c = rgb(74, 122, 176);  break;
                case 5:  c = rgb(92, 138, 74);   break;
                default: c = rgb(16, 16, 22);    break;
                }
                for (y = 0; y < S; y++) {
                    for (x = 0; x < S; x++) {
                        size_t o = (size_t)(OY + my * S + y) * SCRW + OX + mx * S + x;
                        fb[o] = ((fb[o] >> 1) & 0x7f7f7f) + ((c >> 1) & 0x7f7f7f);
                    }
                }
            }
        }
        for (i = 0; i < 10; i++) {              /* facing line, then dot */
            int px = OX + (int)((posX + dirX * i * 0.14) * S);
            int py = OY + (int)((posY + dirY * i * 0.14) * S);
            fb[(size_t)py * SCRW + px] = rgb(255, 220, 90);
        }
        for (y = -1; y <= 1; y++)
            for (x = -1; x <= 1; x++)
                fb[(size_t)(OY + (int)(posY * S) + y) * SCRW
                   + OX + (int)(posX * S) + x] = rgb(255, 255, 255);
    }

    /* HUD */
    {
        char buf[64];
        snprintf(buf, sizeof buf, "GOLD %d/%d", goldCount, NGOLD);
        draw_text(SCRW - text_w(3, buf) - 14, 12, 3, rgb(244, 204, 72), buf);
        snprintf(buf, sizeof buf, "TIME %.1f", gameTime);
        draw_text(SCRW - text_w(3, buf) - 14, 44, 3, rgb(228, 228, 228), buf);
        if (won) {
            const char *s1 = "ALL GOLD FOUND!";
            const char *s2 = "PRESS R TO PLAY AGAIN";
            snprintf(buf, sizeof buf, "TIME %.1f SECONDS", gameTime);
            draw_text((SCRW - text_w(5, s1)) / 2, SCRH / 2 - 96, 5,
                      rgb(250, 210, 80), s1);
            draw_text((SCRW - text_w(4, buf)) / 2, SCRH / 2 - 34, 4,
                      rgb(235, 235, 235), buf);
            draw_text((SCRW - text_w(3, s2)) / 2, SCRH / 2 + 22, 3,
                      rgb(185, 185, 185), s2);
        } else if (g_time < 6.0) {
            const char *s0 = "FIND ALL THE GOLD!";
            draw_text((SCRW - text_w(4, s0)) / 2, 60, 4, rgb(250, 210, 80), s0);
        }
    }
}

/* ------------------------------------------------------ player movement */

static int blocked(double x, double y)
{
    const double R = 0.18;                      /* player radius */
    return cell((int)(x - R), (int)(y - R)) || cell((int)(x + R), (int)(y - R))
        || cell((int)(x - R), (int)(y + R)) || cell((int)(x + R), (int)(y + R));
}

static void rotate(double a)
{
    double c = cos(a), s = sin(a);
    double odx = dirX, opx = planeX;
    dirX = dirX * c - dirY * s;
    dirY = odx * s + dirY * c;
    planeX = planeX * c - planeY * s;
    planeY = opx * s + planeY * c;
}

static void restart_game(void)
{
    int i;
    for (i = 0; i < NGOLD; i++) gold[i].taken = 0;
    goldCount = 0;
    won = 0;
    gameTime = 0;
    g_flash = 0;
    posX = 2.5; posY = 12.5;
    dirX = 1.0; dirY = 0.0;
    planeX = 0.0; planeY = 0.66;
}

static void update(double dt)
{
    double ms = 3.8 * dt, rs = 2.6 * dt;
    double mvx = 0, mvy = 0;
    int i;

    g_time += dt;
    if (!won) gameTime += dt;
    if (g_flash > 0) g_flash -= dt;
    if (g_keys['R']) restart_game();
    if (g_keys['W']) { mvx += dirX * ms;  mvy += dirY * ms; }
    if (g_keys['S']) { mvx -= dirX * ms;  mvy -= dirY * ms; }
    if (g_keys['A']) { mvx += dirY * ms;  mvy -= dirX * ms; }   /* strafe left */
    if (g_keys['D']) { mvx -= dirY * ms;  mvy += dirX * ms; }   /* strafe right */
    if (g_keys[VK_LEFT])  rotate(-rs);
    if (g_keys[VK_RIGHT]) rotate(rs);
    if (g_mouseDX) {                    /* raw counts, already frame-rate free */
        rotate(g_mouseDX * MOUSE_SENS);
        g_mouseDX = 0;
    }

    if (!blocked(posX + mvx, posY)) posX += mvx;   /* per-axis: wall sliding */
    if (!blocked(posX, posY + mvy)) posY += mvy;

    for (i = 0; i < NGOLD; i++) {       /* walk over gold to collect it */
        double dx, dy;
        if (gold[i].taken) continue;
        dx = posX - gold[i].x;
        dy = posY - gold[i].y;
        if (dx * dx + dy * dy < 0.45 * 0.45) {
            gold[i].taken = 1;
            goldCount++;
            g_flash = 0.18;
            if (goldCount == NGOLD) won = 1;
        }
    }
}

/* ------------------------------------------------- BMP screenshot writer */

static int save_bmp(const char *path, const uint32_t *px, int w, int h)
{
    uint8_t hdr[54] = { 0 };
    uint32_t img = (uint32_t)(w * h * 4);
    uint32_t fsize = 54 + img, off = 54, bisz = 40;
    int32_t iw = w, ih = h;                     /* positive height: bottom-up */
    uint16_t planes = 1, bpp = 32;
    FILE *f = fopen(path, "wb");
    int y;
    if (!f) return 0;
    hdr[0] = 'B'; hdr[1] = 'M';
    memcpy(hdr + 2, &fsize, 4);
    memcpy(hdr + 10, &off, 4);
    memcpy(hdr + 14, &bisz, 4);
    memcpy(hdr + 18, &iw, 4);
    memcpy(hdr + 22, &ih, 4);
    memcpy(hdr + 26, &planes, 2);
    memcpy(hdr + 28, &bpp, 2);
    memcpy(hdr + 34, &img, 4);
    fwrite(hdr, 1, 54, f);
    for (y = h - 1; y >= 0; y--)
        fwrite(px + (size_t)y * w, 4, (size_t)w, f);
    fclose(f);
    return 1;
}

/* -------------------------------------------------------------- windowing */

static void capture_mouse(HWND hw)      /* FPS-style: hide + lock to window */
{
    RECT rc;
    POINT p0 = { 0, 0 }, p1;
    if (!g_captureOn) return;
    GetClientRect(hw, &rc);
    p1.x = rc.right;
    p1.y = rc.bottom;
    ClientToScreen(hw, &p0);
    ClientToScreen(hw, &p1);
    rc.left = p0.x; rc.top = p0.y;
    rc.right = p1.x; rc.bottom = p1.y;
    ClipCursor(&rc);
    if (!g_cursorHidden) { ShowCursor(FALSE); g_cursorHidden = 1; }
}

static void release_mouse(void)
{
    ClipCursor(NULL);
    if (g_cursorHidden) { ShowCursor(TRUE); g_cursorHidden = 0; }
}

static LRESULT CALLBACK wndproc(HWND hw, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE) { DestroyWindow(hw); return 0; }
        g_keys[wp & 0xff] = 1;
        return 0;
    case WM_KEYUP:
        g_keys[wp & 0xff] = 0;
        return 0;
    case WM_INPUT: {
        RAWINPUT ri;
        UINT sz = sizeof ri;
        if (GetRawInputData((HRAWINPUT)lp, RID_INPUT, &ri, &sz,
                            sizeof(RAWINPUTHEADER)) != (UINT)-1
            && ri.header.dwType == RIM_TYPEMOUSE
            && !(ri.data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE))
            g_mouseDX += ri.data.mouse.lLastX;
        break;                          /* DefWindowProc does the cleanup */
    }
    case WM_SETFOCUS:
        capture_mouse(hw);
        return 0;
    case WM_KILLFOCUS:
        release_mouse();
        memset(g_keys, 0, sizeof g_keys);
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        BeginPaint(hw, &ps);
        EndPaint(hw, &ps);
        return 0;
    }
    case WM_DESTROY:
        release_mouse();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcA(hw, msg, wp, lp);
}

static double now_seconds(void)
{
    static LARGE_INTEGER fq;
    static int init = 0;
    LARGE_INTEGER t;
    if (!init) { QueryPerformanceFrequency(&fq); init = 1; }
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart / (double)fq.QuadPart;
}

int main(int argc, char **argv)
{
    int screenshot = 0, selftest = 0, i;
    double selfsecs = 5.0;
    const char *shotpath = "screenshot.bmp";

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--screenshot")) {
            screenshot = 1;
            if (i + 1 < argc && argv[i + 1][0] != '-') shotpath = argv[++i];
        } else if (!strcmp(argv[i], "--selftest")) {
            selftest = 1;
            if (i + 1 < argc && argv[i + 1][0] != '-') selfsecs = atof(argv[++i]);
        }
    }

    gen_textures();
    gen_sprites();
    gen_map();
    g_captureOn = !selftest;            /* don't grab the mouse in test runs */

    if (screenshot) {
        render_frame();
        if (!save_bmp(shotpath, fb, SCRW, SCRH)) {
            fprintf(stderr, "failed to write %s\n", shotpath);
            return 1;
        }
        printf("wrote %s (%dx%d)\n", shotpath, SCRW, SCRH);
        return 0;
    }

    {
        WNDCLASSA wc = { 0 };
        HWND hw;
        RECT r = { 0, 0, SCRW, SCRH };
        DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
        BITMAPINFO bmi = { 0 };
        double tprev, t0;
        long frames = 0, fpsFrames = 0;
        double fpsT = 0;

        wc.lpfnWndProc = wndproc;
        wc.hInstance = GetModuleHandleA(NULL);
        wc.hCursor = LoadCursorA(NULL, MAKEINTRESOURCEA(32512));  /* IDC_ARROW */
        wc.lpszClassName = "raycasterwnd";
        RegisterClassA(&wc);

        AdjustWindowRect(&r, style, FALSE);
        hw = CreateWindowExA(0, "raycasterwnd", "Gold Hunter (pure C)",
                             style | WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT,
                             r.right - r.left, r.bottom - r.top,
                             NULL, NULL, wc.hInstance, NULL);
        if (!hw) {
            fprintf(stderr, "CreateWindow failed\n");
            return 1;
        }

        {                               /* raw mouse input for smooth turning */
            RAWINPUTDEVICE rid = { 1, 2, 0, hw };   /* generic desktop, mouse */
            RegisterRawInputDevices(&rid, 1, sizeof rid);
        }

        bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth = SCRW;
        bmi.bmiHeader.biHeight = -SCRH;         /* negative: top-down rows */
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;

        printf("Gold Hunter (pure C raycaster, no libraries)\n");
        printf("  Find all %d gold stashes as fast as you can!\n", NGOLD);
        printf("  W/S move   A/D strafe   Mouse or Left/Right arrows turn\n");
        printf("  R restart   Esc quit   Alt+Tab releases the mouse\n");

        t0 = tprev = now_seconds();
        for (;;) {
            MSG m;
            double t, dt;
            while (PeekMessageA(&m, NULL, 0, 0, PM_REMOVE)) {
                if (m.message == WM_QUIT) goto done;
                TranslateMessage(&m);
                DispatchMessageA(&m);
            }
            t = now_seconds();
            dt = t - tprev;
            tprev = t;
            if (dt > 0.05) dt = 0.05;

            update(dt);
            render_frame();
            {
                HDC dc = GetDC(hw);
                StretchDIBits(dc, 0, 0, SCRW, SCRH, 0, 0, SCRW, SCRH,
                              fb, &bmi, DIB_RGB_COLORS, SRCCOPY);
                ReleaseDC(hw, dc);
            }

            frames++;
            fpsFrames++;
            if (t - fpsT >= 0.5) {
                if (fpsT > 0) {
                    char title[64];
                    snprintf(title, sizeof title, "Gold Hunter (pure C) — %d fps",
                             (int)(fpsFrames / (t - fpsT)));
                    SetWindowTextA(hw, title);
                    if (selftest)
                        printf("[fps] %d\n", (int)(fpsFrames / (t - fpsT)));
                }
                fpsT = t;
                fpsFrames = 0;
            }
            if (selftest && t - t0 >= selfsecs) {
                printf("[selftest] %ld frames in %.1fs, avg %.0f fps\n",
                       frames, t - t0, frames / (t - t0));
                DestroyWindow(hw);
            }
        }
    done:;
    }
    return 0;
}
