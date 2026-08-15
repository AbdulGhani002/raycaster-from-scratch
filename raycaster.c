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

static uint32_t fb[SCRW * SCRH];        /* 0x00RRGGBB, top-down rows */
static uint32_t tex[TEXN][TEXSZ * TEXSZ];
static int world[MAPH][MAPW];           /* [y][x], 0 = empty */
static int g_keys[256];

static double posX = 2.5, posY = 12.5;  /* player, in map cells */
static double dirX = 1.0, dirY = 0.0;
static double planeX = 0.0, planeY = -0.66;   /* FOV ~66 deg */

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

            for (y = ds; y < de; y++) {
                int ty = (int)texPos & (TEXSZ - 1);
                texPos += step;
                fb[(size_t)y * SCRW + x] = shade(T[ty * TEXSZ + texX], lit);
            }
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

static void update(double dt)
{
    double ms = 3.8 * dt, rs = 2.6 * dt;
    double mvx = 0, mvy = 0;
    if (g_keys['W']) { mvx += dirX * ms;  mvy += dirY * ms; }
    if (g_keys['S']) { mvx -= dirX * ms;  mvy -= dirY * ms; }
    if (g_keys['A']) { mvx += dirY * ms;  mvy -= dirX * ms; }   /* strafe left */
    if (g_keys['D']) { mvx -= dirY * ms;  mvy += dirX * ms; }   /* strafe right */
    if (g_keys[VK_LEFT])  rotate(-rs);
    if (g_keys[VK_RIGHT]) rotate(rs);

    if (!blocked(posX + mvx, posY)) posX += mvx;   /* per-axis: wall sliding */
    if (!blocked(posX, posY + mvy)) posY += mvy;
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
    case WM_KILLFOCUS:
        memset(g_keys, 0, sizeof g_keys);
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        BeginPaint(hw, &ps);
        EndPaint(hw, &ps);
        return 0;
    }
    case WM_DESTROY:
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
    gen_map();

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
        hw = CreateWindowExA(0, "raycasterwnd", "Raycaster (pure C)",
                             style | WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT,
                             r.right - r.left, r.bottom - r.top,
                             NULL, NULL, wc.hInstance, NULL);
        if (!hw) {
            fprintf(stderr, "CreateWindow failed\n");
            return 1;
        }

        bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth = SCRW;
        bmi.bmiHeader.biHeight = -SCRH;         /* negative: top-down rows */
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;

        printf("Raycaster (pure C, no libraries)\n");
        printf("  W/S move   A/D strafe   Left/Right arrows turn   Esc quit\n");

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
                    snprintf(title, sizeof title, "Raycaster (pure C) — %d fps",
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
