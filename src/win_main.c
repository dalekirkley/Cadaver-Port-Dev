/* Windows front end for the native Cadaver: GDI window (2x scale), keyboard input, fixed-rate loop.
 * Game logic lives in game.c and never touches the OS. */
#define WIN32_LEAN_AND_MEAN
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
#include <windows.h>
#include <mmsystem.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "game.h"
#include "crt.h"
#include "d3d_present.h"

#define SCALE 2
static BITMAPINFO bmi;
static uint32_t fb[SCREEN_W * SCREEN_H];
static uint32_t bgra[SCREEN_W * SCREEN_H];
static int running = 1;
static Game *g_game;                                                         /* for debug keys handled in the window procedure */
static int use_d3d = 0;                                                      /* Direct3D 11 presenter; GDI is the fallback */
static int crt_on = 0; static uint32_t *crt_buf; static int crt_w, crt_h;   /* F2 toggles the CRT filter */
static int pending_level = 0;   /* F6..F10 = load data\\levelN.bin */
#define AUDIO_RATE 22050
#define ABUF 8
#define ASAMPLES 441
static HWAVEOUT wo; static WAVEHDR whdr[ABUF]; static short abuf[ABUF][ASAMPLES]; static int have_audio;
static void audio_init(void) {
    WAVEFORMATEX fmt = {0}; fmt.wFormatTag = WAVE_FORMAT_PCM; fmt.nChannels = 1; fmt.nSamplesPerSec = AUDIO_RATE;
    fmt.wBitsPerSample = 16; fmt.nBlockAlign = 2; fmt.nAvgBytesPerSec = AUDIO_RATE * 2;
    if (waveOutOpen(&wo, WAVE_MAPPER, &fmt, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR) return;
    have_audio = 1;
    for (int i = 0; i < ABUF; i++) { whdr[i].lpData = (LPSTR)abuf[i]; whdr[i].dwBufferLength = sizeof abuf[i]; waveOutPrepareHeader(wo, &whdr[i], sizeof whdr[i]); whdr[i].dwFlags |= WHDR_DONE; }
}
static void audio_pump(Game *g) {
    if (!have_audio) return;
    for (int i = 0; i < ABUF; i++) if (whdr[i].dwFlags & WHDR_DONE) {
        game_audio_mix(g, abuf[i], ASAMPLES, AUDIO_RATE);
        whdr[i].dwFlags &= ~WHDR_DONE; waveOutWrite(wo, &whdr[i], sizeof whdr[i]);
    }
}

static int read_joy(void) {
    int j = 0;
#define K(vk) (GetAsyncKeyState(vk) & 0x8000)
    if (K(VK_UP) || K('W')) j |= JOY_UP;
    if (K(VK_DOWN) || K('S')) j |= JOY_DOWN;
    if (K(VK_LEFT) || K('A')) j |= JOY_LEFT;
    if (K(VK_RIGHT) || K('D')) j |= JOY_RIGHT;
    if (K(VK_CONTROL) || K('X') || K('Z')) j |= JOY_FIRE;
    if (K(VK_SPACE)) j |= JOY_INV;
    if (K(VK_RETURN)) j |= JOY_LIST;
    if (K('H')) j |= JOY_HOLD;
    if (K(VK_F1) || K('M')) j |= JOY_MAP;
    if (K(VK_F12)) j |= JOY_DEBUG;
    return j;
}

static WINDOWPLACEMENT fs_prev = { sizeof(WINDOWPLACEMENT) }; static int fullscreen;
static void toggle_fullscreen(HWND h) {                                  /* borderless fullscreen on the monitor the window is on (F11 / Alt+Enter) */
    DWORD st = (DWORD)GetWindowLongA(h, GWL_STYLE);
    if (!fullscreen) {
        MONITORINFO mi = { sizeof mi };
        if (GetWindowPlacement(h, &fs_prev) && GetMonitorInfoA(MonitorFromWindow(h, MONITOR_DEFAULTTOPRIMARY), &mi)) {
            SetWindowLongA(h, GWL_STYLE, (LONG)(st & ~WS_OVERLAPPEDWINDOW));
            SetWindowPos(h, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left, mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
            fullscreen = 1; ShowCursor(FALSE); }
    } else {
        SetWindowLongA(h, GWL_STYLE, (LONG)(st | WS_OVERLAPPEDWINDOW)); SetWindowPlacement(h, &fs_prev);
        SetWindowPos(h, NULL, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
        fullscreen = 0; ShowCursor(TRUE); }
}
static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_DESTROY: running = 0; PostQuitMessage(0); return 0;
    case WM_KEYDOWN:
        if (w == VK_ESCAPE) { running = 0; PostQuitMessage(0); }
        if (w == VK_F2 && !(l & (1 << 30))) crt_on ^= 1;
        if (w == VK_F11 && !(l & (1 << 30))) toggle_fullscreen(h);
        if ((w == VK_PRIOR || w == VK_NEXT) && g_game) game_cycle_room(g_game, w == VK_NEXT ? 1 : -1);   /* debug: walk through the rooms */                    /* F2: CRT filter on/off */
        if (w >= VK_F6 && w <= VK_F10) pending_level = (int)(w - VK_F6) + 1;   /* debug level switch; F1 stays free for the map */
        return 0;
    case WM_SYSKEYDOWN:
        if (w == VK_RETURN && !(l & (1 << 30))) { toggle_fullscreen(h); return 0; }
        break;
    case WM_SIZING: {                                           /* snap the window to the 320x200 shape while it is dragged */
        RECT *rc = (RECT *)l, nc = {0, 0, 0, 0}; AdjustWindowRect(&nc, WS_OVERLAPPEDWINDOW, FALSE);
        int bw = (nc.right - nc.left), bh = (nc.bottom - nc.top);               /* frame size around the client area */
        int cw = (rc->right - rc->left) - bw, ch = (rc->bottom - rc->top) - bh;
        if (cw < SCREEN_W) cw = SCREEN_W; if (ch < SCREEN_H) ch = SCREEN_H;
        if (w == WMSZ_LEFT || w == WMSZ_RIGHT) ch = cw * SCREEN_H / SCREEN_W;      /* width drives height */
        else if (w == WMSZ_TOP || w == WMSZ_BOTTOM) cw = ch * SCREEN_W / SCREEN_H; /* height drives width */
        else { int hw2 = ch * SCREEN_W / SCREEN_H; if (hw2 > cw) cw = hw2; ch = cw * SCREEN_H / SCREEN_W; }   /* corner: take the larger */
        if (w == WMSZ_LEFT || w == WMSZ_TOPLEFT || w == WMSZ_BOTTOMLEFT) rc->left = rc->right - (cw + bw); else rc->right = rc->left + cw + bw;
        if (w == WMSZ_TOP || w == WMSZ_TOPLEFT || w == WMSZ_TOPRIGHT) rc->top = rc->bottom - (ch + bh); else rc->bottom = rc->top + ch + bh;
        return TRUE; }
    case WM_SIZE: if (use_d3d && w != SIZE_MINIMIZED) d3d_resize(LOWORD(l), HIWORD(l)); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps); RECT rc; GetClientRect(h, &rc);
        if (use_d3d) { EndPaint(h, &ps); return 0; }
        { int cw = rc.right, ch = rc.bottom, dw = cw, dh = cw * SCREEN_H / SCREEN_W;      /* keep the 320x200 aspect: letterbox with black bars */
          if (dh > ch) { dh = ch; dw = ch * SCREEN_W / SCREEN_H; }
          int ox = (cw - dw) / 2, oy = (ch - dh) / 2;
          if (ox > 0) { PatBlt(dc, 0, 0, ox, ch, BLACKNESS); PatBlt(dc, ox + dw, 0, cw - ox - dw, ch, BLACKNESS); }
          if (oy > 0) { PatBlt(dc, 0, 0, cw, oy, BLACKNESS); PatBlt(dc, 0, oy + dh, cw, ch - oy - dh, BLACKNESS); }
          if (dw > 0 && dh > 0 && crt_on) {
              if (crt_w != dw || crt_h != dh) { free(crt_buf); crt_buf = malloc((size_t)dw * dh * 4); crt_w = dw; crt_h = dh; }
              BITMAPINFO cb = bmi; cb.bmiHeader.biWidth = dw; cb.bmiHeader.biHeight = -dh;
              crt_render(crt_buf, dw, dh, fb, SCREEN_W, SCREEN_H);
              StretchDIBits(dc, ox, oy, dw, dh, 0, 0, dw, dh, crt_buf, &cb, DIB_RGB_COLORS, SRCCOPY);
          } else if (dw > 0 && dh > 0) StretchDIBits(dc, ox, oy, dw, dh, 0, 0, SCREEN_W, SCREEN_H, bgra, &bmi, DIB_RGB_COLORS, SRCCOPY); }
        EndPaint(h, &ps); return 0; }
    }
    return DefWindowProc(h, m, w, l);
}

int WINAPI WinMain(HINSTANCE hi, HINSTANCE hp, LPSTR cmd, int show) {
    (void)hp; (void)cmd;
    const char *path = (cmd && *cmd) ? cmd : "data\\level1.bin";
    Game *g = game_load(path); g_game = g;
    if (!g) { MessageBoxA(NULL, "Could not load data\\level1.bin.\nRun tools\\export_level.py on your Cadaver disks first (see README).", "Cadaver", MB_ICONERROR); return 1; }
    WNDCLASSA wc = {0}; wc.lpfnWndProc = wndproc; wc.hInstance = hi; wc.hCursor = LoadCursor(NULL, IDC_ARROW); wc.lpszClassName = "CadaverNative";
    RegisterClassA(&wc);
    RECT r = {0, 0, SCREEN_W * SCALE, SCREEN_H * SCALE}; AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    HWND hw = CreateWindowA("CadaverNative", "Cadaver (native)", WS_OVERLAPPEDWINDOW | WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top, NULL, NULL, hi, NULL);
    bmi.bmiHeader.biSize = sizeof bmi.bmiHeader; bmi.bmiHeader.biWidth = SCREEN_W; bmi.bmiHeader.biHeight = -SCREEN_H;
    bmi.bmiHeader.biPlanes = 1; bmi.bmiHeader.biBitCount = 32; bmi.bmiHeader.biCompression = BI_RGB;
    if (!getenv("CADAVER_GDI")) use_d3d = d3d_init(hw, SCREEN_W, SCREEN_H);          /* set CADAVER_GDI=1 to force the old GDI path */
    timeBeginPeriod(1); audio_init();
    LARGE_INTEGER fq, t0; QueryPerformanceFrequency(&fq); QueryPerformanceCounter(&t0);
    double next = 0, tick = 1.0 / TICK_HZ;
    while (running) {
        MSG m; while (PeekMessage(&m, NULL, 0, 0, PM_REMOVE)) { if (m.message == WM_QUIT) running = 0; TranslateMessage(&m); DispatchMessage(&m); }
        LARGE_INTEGER now; QueryPerformanceCounter(&now); double t = (double)(now.QuadPart - t0.QuadPart) / fq.QuadPart;
        int steps = 0;
        while (t >= next && steps < 5) { game_tick(g, read_joy()); next += tick; steps++; }
        if (steps > 4) next = t;
        { int rl = game_level_request(g); if (rl) pending_level = rl; }
        if (pending_level) {
            char p[64]; snprintf(p, sizeof p, "data\\level%d.bin", pending_level); pending_level = 0;
            Game *ng = game_load(p); if (ng) { game_free(g); g = ng; g_game = g; next = t; }
        }
        audio_pump(g);
        game_render(g, fb);
        { static char last[512]; char dt[512];                      /* F12: debug line in the title and in cadaver_debug.txt */
          int on = game_debug_text(g, dt, sizeof dt);
          if (!on) strcpy(dt, "Cadaver (native)");
          if (strcmp(dt, last)) { strcpy(last, dt); SetWindowTextA(hw, dt); if (on) { FILE *df = fopen("cadaver_debug.txt", "a"); if (df) { fprintf(df, "%s\n", dt); fclose(df); } } } }
        if (use_d3d) d3d_present(fb, crt_on);
        else {
            for (int i = 0; i < SCREEN_W * SCREEN_H; i++) { uint32_t v = fb[i]; bgra[i] = (v & 0xFF) << 16 | (v & 0xFF00) | (v >> 16 & 0xFF); }
            InvalidateRect(hw, NULL, FALSE); UpdateWindow(hw);
        }
        Sleep(5);
    }
    if (use_d3d) d3d_shutdown();
    timeEndPeriod(1); game_free(g); return 0;
}
