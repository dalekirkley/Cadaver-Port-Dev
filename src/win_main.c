/* Windows front end for the native Cadaver: GDI window (2x scale), keyboard input, fixed-rate loop.
 * Game logic lives in game.c and never touches the OS. */
#define WIN32_LEAN_AND_MEAN
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
#include <windows.h>
#include <mmsystem.h>
#include <stdio.h>
#include "game.h"

#define SCALE 2
static BITMAPINFO bmi;
static uint32_t fb[SCREEN_W * SCREEN_H];
static uint32_t bgra[SCREEN_W * SCREEN_H];
static int running = 1;
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
    if (K(VK_CONTROL) || K(VK_SPACE)) j |= JOY_FIRE;
    if (K(VK_F1) || K('M')) j |= JOY_MAP;
    if (K(VK_F12)) j |= JOY_DEBUG;
    return j;
}

static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_DESTROY: running = 0; PostQuitMessage(0); return 0;
    case WM_KEYDOWN:
        if (w == VK_ESCAPE) { running = 0; PostQuitMessage(0); }
        if (w >= VK_F6 && w <= VK_F10) pending_level = (int)(w - VK_F6) + 1;   /* debug level switch; F1 stays free for the map */
        return 0;
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps; HDC dc = BeginPaint(h, &ps); RECT rc; GetClientRect(h, &rc);
        StretchDIBits(dc, 0, 0, rc.right, rc.bottom, 0, 0, SCREEN_W, SCREEN_H, bgra, &bmi, DIB_RGB_COLORS, SRCCOPY);
        EndPaint(h, &ps); return 0; }
    }
    return DefWindowProc(h, m, w, l);
}

int WINAPI WinMain(HINSTANCE hi, HINSTANCE hp, LPSTR cmd, int show) {
    (void)hp; (void)cmd;
    const char *path = (cmd && *cmd) ? cmd : "data\\level1.bin";
    Game *g = game_load(path);
    if (!g) { MessageBoxA(NULL, "Could not load data\\level1.bin.\nRun tools\\export_level.py on your Cadaver disks first (see README).", "Cadaver", MB_ICONERROR); return 1; }
    WNDCLASSA wc = {0}; wc.lpfnWndProc = wndproc; wc.hInstance = hi; wc.hCursor = LoadCursor(NULL, IDC_ARROW); wc.lpszClassName = "CadaverNative";
    RegisterClassA(&wc);
    RECT r = {0, 0, SCREEN_W * SCALE, SCREEN_H * SCALE}; AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    HWND hw = CreateWindowA("CadaverNative", "Cadaver (native)", WS_OVERLAPPEDWINDOW | WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top, NULL, NULL, hi, NULL);
    bmi.bmiHeader.biSize = sizeof bmi.bmiHeader; bmi.bmiHeader.biWidth = SCREEN_W; bmi.bmiHeader.biHeight = -SCREEN_H;
    bmi.bmiHeader.biPlanes = 1; bmi.bmiHeader.biBitCount = 32; bmi.bmiHeader.biCompression = BI_RGB;
    timeBeginPeriod(1); audio_init();
    LARGE_INTEGER fq, t0; QueryPerformanceFrequency(&fq); QueryPerformanceCounter(&t0);
    double next = 0, tick = 1.0 / TICK_HZ;
    while (running) {
        MSG m; while (PeekMessage(&m, NULL, 0, 0, PM_REMOVE)) { if (m.message == WM_QUIT) running = 0; TranslateMessage(&m); DispatchMessage(&m); }
        LARGE_INTEGER now; QueryPerformanceCounter(&now); double t = (double)(now.QuadPart - t0.QuadPart) / fq.QuadPart;
        int steps = 0;
        while (t >= next && steps < 5) { game_tick(g, read_joy()); next += tick; steps++; }
        if (steps > 4) next = t;
        if (pending_level) {
            char p[64]; snprintf(p, sizeof p, "data\\level%d.bin", pending_level); pending_level = 0;
            Game *ng = game_load(p); if (ng) { game_free(g); g = ng; next = t; }
        }
        audio_pump(g);
        game_render(g, fb);
        for (int i = 0; i < SCREEN_W * SCREEN_H; i++) { uint32_t v = fb[i]; bgra[i] = (v & 0xFF) << 16 | (v & 0xFF00) | (v >> 16 & 0xFF); }
        InvalidateRect(hw, NULL, FALSE); UpdateWindow(hw);
        Sleep(5);
    }
    timeEndPeriod(1); game_free(g); return 0;
}
