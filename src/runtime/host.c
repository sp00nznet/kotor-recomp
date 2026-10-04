/*
 * Star Wars: Knights of the Old Republic - static recompilation host.
 *
 * A 32-bit host on pcrecomp's runtime/native32 (the native bridge, callbacks
 * and machine lock; see its header). Every import goes to the real DLL --
 * OpenGL, Miles, Bink, DirectInput -- so what is here is only what is specific
 * to this game: where the image goes, the command line, the few calls that
 * would reach outside the game's own window, muting, and the fault report.
 * docs/architecture.md has the reasoning.
 *
 * Linked at /BASE:0x60000000 (CMakeLists.txt) so the guest's
 * 0x00400000..0x00836000 is free when main() maps the image.
 */
#define WIN32_LEAN_AND_MEAN
#define COBJMACROS
#include <windows.h>
#include <mmdeviceapi.h>
#include <audiopolicy.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "native32.h"
#include "recomp_trace.h"
#include "oracle.h"

extern const uint32_t kotor_entry_va;  /* recomp_dispatch.c */

#define KOTOR_IMAGE_BASE 0x00400000u
#define ARG(n) MEM32(g_esp + 4 + 4 * (n))

static DWORD g_watchdog_s;
static int   g_modeswitch;   /* --modeswitch: let ChangeDisplaySettingsA through */
static int   g_gamma;        /* --gamma: let SetDeviceGammaRamp through */
static int   g_original;     /* --original: the shipping machine code (oracle.c) */

/* ---- calls that reach outside the window -----------------------------------
 * ChangeDisplaySettingsA changes the mode of the primary display, not of the
 * monitor the window is on: under offstage it pulled the virtual monitor out
 * from under the recording, and on a desktop it resizes the user's real
 * screen. SetDeviceGammaRamp changes the real monitor's gamma and outlives a
 * crash. Both are answered "done" without doing it unless asked for. */
static void shim_ChangeDisplaySettingsA(void) {
    DEVMODEA* dm = (DEVMODEA*)(uintptr_t)ARG(0);
    if (g_modeswitch) {
        g_eax = (uint32_t)ChangeDisplaySettingsA(dm, ARG(1));
    } else {
        if (dm)
            fprintf(stderr, "[host] ChangeDisplaySettingsA %lux%lu %lubpp: skipped (--modeswitch)\n",
                    dm->dmPelsWidth, dm->dmPelsHeight, dm->dmBitsPerPel);
        g_eax = DISP_CHANGE_SUCCESSFUL;
    }
    g_esp += 4 + 2 * 4;
}

static void shim_SetDeviceGammaRamp(void) {
    g_eax = g_gamma ? SetDeviceGammaRamp((HDC)(uintptr_t)ARG(0), (LPVOID)(uintptr_t)ARG(1)) : TRUE;
    g_esp += 4 + 2 * 4;
}

/* ---- the process is the host, the module is the guest ----------------------
 * GetModuleHandleA(NULL) and GetModuleFileNameA(NULL) would name the host;
 * the game finds its own folder and resources through them. */
static char g_guest_exe[MAX_PATH], g_guest_cmdline[MAX_PATH + 8];

static void shim_GetModuleHandleA(void) {
    g_eax = ARG(0) ? (uint32_t)(uintptr_t)GetModuleHandleA((LPCSTR)(uintptr_t)ARG(0))
                   : KOTOR_IMAGE_BASE;
    g_esp += 4 + 1 * 4;
}

static void shim_GetModuleFileNameA(void) {
    uint32_t h = ARG(0), size = ARG(2);
    char* out = (char*)(uintptr_t)ARG(1);
    if (h == 0 || h == KOTOR_IMAGE_BASE) {
        uint32_t n = (uint32_t)strlen(g_guest_exe);
        if (size) {
            uint32_t k = n < size ? n : size - 1;
            memcpy(out, g_guest_exe, k);
            out[k] = 0;
            n = k;
        }
        g_eax = n;
    } else {
        g_eax = GetModuleFileNameA((HMODULE)(uintptr_t)h, out, size);
    }
    g_esp += 4 + 3 * 4;
}

static void shim_GetCommandLineA(void) {
    g_eax = (uint32_t)(uintptr_t)g_guest_cmdline;
    g_esp += 4;
}

/* The game's own media opens, logged as milestones (tools/conformance.py):
 * each intro movie through Bink, then the main menu's music as a Miles stream.
 * Pass-through; a run is muted at the session, not here. */
typedef uint32_t (WINAPI *ail_open_stream_t)(uint32_t, const char*, int32_t);
typedef uint32_t (WINAPI *bink_open_t)(const char*, uint32_t);
static HANDLE g_menu_up;                 /* set when the menu music opens: --click's clock */

static void shim_AIL_open_stream(void) {
    static ail_open_stream_t real;
    if (!real) real = (ail_open_stream_t)GetProcAddress(GetModuleHandleA("mss32.dll"), "_AIL_open_stream@12");
    const char* name = (const char*)(uintptr_t)ARG(1);
    g_eax = real(ARG(0), name, (int32_t)ARG(2));
    printf("[host] stream %s -> %s\n", name ? name : "(null)", g_eax ? "open" : "FAILED");
    if (g_eax && g_menu_up) SetEvent(g_menu_up);
    g_esp += 4 + 3 * 4;
}

/* Module archives (modules\*.rim, *.mod) are opened when an area loads: the
 * Endar Spire's are the milestone after a new game, and the first frame drawn
 * after one is logged too (shim_SwapBuffers). Every other open passes through
 * unlogged; the bulk of the game's reads go through handles to the BIFs it
 * opened once at startup. */
static volatile LONG g_module_opened;

static void shim_CreateFileA(void) {
    const char* name = (const char*)(uintptr_t)ARG(0);
    g_eax = (uint32_t)(uintptr_t)CreateFileA(name, ARG(1), ARG(2), (LPSECURITY_ATTRIBUTES)(uintptr_t)ARG(3),
                                             ARG(4), ARG(5), (HANDLE)(uintptr_t)ARG(6));
    size_t n = name ? strlen(name) : 0;
    if (n > 4 && (!_stricmp(name + n - 4, ".rim") || !_stricmp(name + n - 4, ".mod"))) {
        int ok = g_eax != (uint32_t)(uintptr_t)INVALID_HANDLE_VALUE;
        printf("[host] module %s -> %s\n", name, ok ? "open" : "missing");
        if (ok) InterlockedExchange(&g_module_opened, 1);
    }
    g_esp += 4 + 7 * 4;
}

static void shim_BinkOpen(void) {
    static bink_open_t real;
    if (!real) real = (bink_open_t)GetProcAddress(GetModuleHandleA("binkw32.dll"), "_BinkOpen@8");
    const char* name = (const char*)(uintptr_t)ARG(0);
    g_eax = real(name, ARG(1));
    printf("[host] movie %s -> %s\n", name ? name : "(null)", g_eax ? "open" : "FAILED");
    g_esp += 4 + 2 * 4;
}

/* ---- scripted input ---------------------------------------------------------
 * --click x,y@s: s seconds after the main menu appears (its music stream
 * opens, shim_AIL_open_stream), move, press and release the left button at
 * client (x, y), top-left origin, the coordinates of a --record frame. Timed
 * from the menu, not from entry: how long the intro takes varies by a factor
 * of three between runs, and clicks timed from entry landed on the wrong
 * screens. The game takes mouse buttons and positions from window messages
 * (its window procedure, around 0x00403000, flips y with the window height),
 * so posting them reaches it even with the window hidden. They go to the
 * window it renders into, taken from the SwapBuffers DC. */
#define MAX_CLICKS 32
static struct { int x, y; DWORD ms; } g_clicks[MAX_CLICKS];
static int g_nclicks;
static volatile HWND g_render_hwnd;
static DWORD g_t0;

static DWORD WINAPI input_script(LPVOID unused) {
    (void)unused;
    WaitForSingleObject(g_menu_up, INFINITE);
    g_t0 = GetTickCount();
    printf("[input] main menu up, script starts\n");
    for (int i = 0; i < g_nclicks; i++) {
        DWORD now = GetTickCount() - g_t0;
        if (g_clicks[i].ms > now) Sleep(g_clicks[i].ms - now);
        HWND h = g_render_hwnd;
        LPARAM at = MAKELPARAM(g_clicks[i].x, g_clicks[i].y);
        printf("[input] click %d,%d at %.1f s -> %p\n", g_clicks[i].x, g_clicks[i].y,
               (GetTickCount() - g_t0) / 1000.0, (void*)h);
        fflush(stdout);
        if (!h) continue;
        PostMessageA(h, WM_MOUSEMOVE, 0, at);
        Sleep(100);
        PostMessageA(h, WM_LBUTTONDOWN, MK_LBUTTON, at);
        Sleep(100);
        PostMessageA(h, WM_LBUTTONUP, 0, at);
    }
    return 0;
}

static int click_arg(const char* a) {
    double s;
    if (g_nclicks >= MAX_CLICKS ||
        sscanf(a, "%d,%d@%lf", &g_clicks[g_nclicks].x, &g_clicks[g_nclicks].y, &s) != 3) return 0;
    g_clicks[g_nclicks++].ms = (DWORD)(s * 1000);
    return 1;
}

/* DirectInput8Create checks that its HINSTANCE is a module Windows loaded, and
 * the guest image is not one: it passes GetModuleHandleA(NULL), which is the
 * image base above. Refused, the game carried on with no input object, and
 * its input manager read through null (0x005E2F93, then 0x005E4053, in both
 * the lift and the original). The host's own instance stands in for it. */
typedef HRESULT (WINAPI *di8create_t)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN);
static void shim_DirectInput8Create(void) {
    static di8create_t real;
    if (!real) real = (di8create_t)GetProcAddress(LoadLibraryA("dinput8.dll"), "DirectInput8Create");
    HINSTANCE h = (HINSTANCE)(uintptr_t)ARG(0);
    if ((uint32_t)(uintptr_t)h == KOTOR_IMAGE_BASE) h = GetModuleHandleA(NULL);
    g_eax = (uint32_t)real(h, ARG(1), (REFIID)(uintptr_t)ARG(2), (LPVOID*)(uintptr_t)ARG(3),
                           (LPUNKNOWN)(uintptr_t)ARG(4));
    printf("[host] DirectInput8Create -> 0x%08X\n", g_eax);
    g_esp += 4 + 5 * 4;
}

/* ---- milestones and headless -------------------------------------------------
 * The conformance harness (tools/conformance.py) scores a run by these lines.
 * --headless keeps the game's window hidden and prints its message boxes, so a
 * run never lands on a screen (over RDP, a phone's); OpenGL still renders into
 * the hidden window. */
static int g_headless;
static volatile LONG g_frames;

static void shim_CreateWindowExA(void) {
    uint32_t style = ARG(3);
    if (g_headless) style &= ~WS_VISIBLE;
    HWND h = CreateWindowExA(ARG(0), (LPCSTR)(uintptr_t)ARG(1), (LPCSTR)(uintptr_t)ARG(2), style,
                             ARG(4), ARG(5), ARG(6), ARG(7), (HWND)(uintptr_t)ARG(8),
                             (HMENU)(uintptr_t)ARG(9), (HINSTANCE)(uintptr_t)ARG(10), (LPVOID)(uintptr_t)ARG(11));
    printf("[host] CreateWindowExA(\"%s\", %dx%d) -> %p\n",
           ARG(2) ? (const char*)(uintptr_t)ARG(2) : "", (int)ARG(6), (int)ARG(7), (void*)h);
    g_eax = (uint32_t)(uintptr_t)h;
    g_esp += 4 + 12 * 4;
}

static void shim_ShowWindow(void) {
    g_eax = g_headless ? 0 : ShowWindow((HWND)(uintptr_t)ARG(0), ARG(1));
    g_esp += 4 + 2 * 4;
}

static void shim_MessageBoxA(void) {
    const char* text = ARG(1) ? (const char*)(uintptr_t)ARG(1) : "";
    const char* cap = ARG(2) ? (const char*)(uintptr_t)ARG(2) : "";
    printf("[host] MessageBoxA \"%s\": %s\n", cap, text);
    if (g_headless) {
        uint32_t b = ARG(3) & MB_TYPEMASK;
        g_eax = (b == MB_YESNO || b == MB_YESNOCANCEL) ? IDNO : IDOK;
    } else {
        g_eax = MessageBoxA((HWND)(uintptr_t)ARG(0), text, cap, ARG(3));
    }
    g_esp += 4 + 4 * 4;
}

/* --record out.mp4: the back buffer, read with glReadPixels just before each
 * swap and piped to ffmpeg, so a run shows what it drew with no display at all
 * (REPO_RULES section 10/13). Sampled at 30 fps; the size is fixed by the
 * first frame, and a frame of another size (the movie window) is skipped.
 * ponytail: a frame at another size is dropped, not scaled; scale it if the
 * movies need recording. */
#define GL_BGRA_EXT 0x80E1
#define GL_BACK_BUF 0x0405
static const char* g_record;
static FILE* g_rec_pipe;
static int g_rec_w, g_rec_h;
static uint8_t* g_rec_buf;
static DWORD g_rec_next;

static void record_frame(HDC dc) {
    typedef void (WINAPI *read_pixels_t)(int, int, int, int, unsigned, unsigned, void*);
    typedef void (WINAPI *read_buffer_t)(unsigned);
    static read_pixels_t read_pixels;
    static read_buffer_t read_buffer;
    RECT rc;
    DWORD now = GetTickCount();
    if ((int)(now - g_rec_next) < 0 || !GetClientRect(WindowFromDC(dc), &rc)) return;
    g_rec_next = now + 33;
    int w = rc.right & ~1, h = rc.bottom & ~1;        /* yuv420p wants even sizes */
    if (w <= 0 || h <= 0) return;
    if (!g_rec_pipe) {
        HMODULE gl = GetModuleHandleA("opengl32.dll");
        read_pixels = (read_pixels_t)GetProcAddress(gl, "glReadPixels");
        read_buffer = (read_buffer_t)GetProcAddress(gl, "glReadBuffer");
        char cmd[MAX_PATH + 256];
        _snprintf(cmd, sizeof cmd - 1,
                  "ffmpeg -v error -y -f rawvideo -pix_fmt bgra -s %dx%d -r 30 -i - "
                  "-vf vflip -pix_fmt yuv420p \"%s\"", w, h, g_record);
        g_rec_pipe = _popen(cmd, "wb");
        g_rec_w = w, g_rec_h = h;
        g_rec_buf = (uint8_t*)malloc((size_t)w * h * 4);
        if (!g_rec_pipe || !g_rec_buf || !read_pixels) { fprintf(stderr, "[record] cannot start ffmpeg\n"); g_record = NULL; return; }
        printf("[record] %dx%d -> %s\n", w, h, g_record);
    }
    if (w != g_rec_w || h != g_rec_h) return;
    if (read_buffer) read_buffer(GL_BACK_BUF);
    read_pixels(0, 0, w, h, GL_BGRA_EXT, 0x1401 /* GL_UNSIGNED_BYTE */, g_rec_buf);
    fwrite(g_rec_buf, 4, (size_t)w * h, g_rec_pipe);
}

static void record_close(void) {
    if (g_rec_pipe) { _pclose(g_rec_pipe); g_rec_pipe = NULL; }
}

static void shim_SwapBuffers(void) {
    LONG n = InterlockedIncrement(&g_frames);
    if (n == 1 || n == 60 || n % 1000 == 0) printf("[host] frame %ld presented\n", n);
    if (InterlockedExchange(&g_module_opened, 0)) printf("[host] frame %ld presented after a module load\n", n);
    HDC dc = (HDC)(uintptr_t)ARG(0);
    HWND rw = WindowFromDC(dc);           /* every frame: the game recreates its window */
    if (rw) g_render_hwnd = rw;
    if (g_record) record_frame(dc);
    g_eax = SwapBuffers(dc);
    g_esp += 4 + 1 * 4;
}

static native32_shim_t g_shims[] = {
    { "CreateWindowExA", shim_CreateWindowExA },
    { "ShowWindow", shim_ShowWindow },
    { "MessageBoxA", shim_MessageBoxA },
    { "SwapBuffers", shim_SwapBuffers },
    { "ChangeDisplaySettingsA", shim_ChangeDisplaySettingsA },
    { "SetDeviceGammaRamp", shim_SetDeviceGammaRamp },
    { "GetModuleHandleA", shim_GetModuleHandleA },
    { "GetModuleFileNameA", shim_GetModuleFileNameA },
    { "GetCommandLineA", shim_GetCommandLineA },
    { "DirectInput8Create", shim_DirectInput8Create },
    { "_AIL_open_stream@12", shim_AIL_open_stream },
    { "_BinkOpen@8", shim_BinkOpen },
    { "CreateFileA", shim_CreateFileA },
};

/* ---- muted by default -------------------------------------------------------
 * Every run is silent until sound is being tested (--sound). Muting the
 * process's own audio session catches Miles, Bink and anything else the game
 * opens, without touching swkotor.ini or the system volume; the session is
 * per process, so it is gone when the run ends. */
static const GUID kCLSID_MMDeviceEnumerator =
    { 0xBCDE0395, 0xE52F, 0x467C, { 0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E } };
static const GUID kIID_IMMDeviceEnumerator =
    { 0xA95664D2, 0x9614, 0x4F35, { 0xA7, 0x46, 0xDE, 0x8D, 0xB6, 0x36, 0x17, 0xE6 } };
static const GUID kIID_IAudioSessionManager =
    { 0xBFA971F1, 0x4D5E, 0x40BB, { 0x93, 0x5E, 0x96, 0x70, 0x39, 0xBF, 0xBE, 0xE4 } };

static int mute_process(void) {
    IMMDeviceEnumerator* en = NULL;
    IMMDevice* dev = NULL;
    IAudioSessionManager* mgr = NULL;
    ISimpleAudioVolume* vol = NULL;
    HRESULT hr = CoCreateInstance(&kCLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL,
                                  &kIID_IMMDeviceEnumerator, (void**)&en);
    if (SUCCEEDED(hr)) hr = IMMDeviceEnumerator_GetDefaultAudioEndpoint(en, eRender, eConsole, &dev);
    if (SUCCEEDED(hr)) hr = IMMDevice_Activate(dev, &kIID_IAudioSessionManager, CLSCTX_ALL, NULL, (void**)&mgr);
    if (SUCCEEDED(hr)) hr = IAudioSessionManager_GetSimpleAudioVolume(mgr, &GUID_NULL, FALSE, &vol);
    if (SUCCEEDED(hr)) hr = ISimpleAudioVolume_SetMute(vol, TRUE, NULL);
    if (vol) ISimpleAudioVolume_Release(vol);    /* the session keeps its mute */
    if (mgr) IAudioSessionManager_Release(mgr);
    if (dev) IMMDevice_Release(dev);
    if (en) IMMDeviceEnumerator_Release(en);
    return SUCCEEDED(hr);
}

/* ---- reports ---------------------------------------------------------------- */
recomp_func_t recomp_lookup_manual(uint32_t va) {
    (void)va;
    return NULL;
}

void recomp_not_lifted(uint32_t va) {
    fprintf(stderr,
        "\n[not-lifted] sub_%08X  (called from 0x%08X)\n"
        "  Widen the closure:  py -3 run_lift.py --roots 0x%08X  (or --max N, or --all)\n",
        va, g_cur_func, va);
    recomp_dump_trace("not-lifted");
    native32_dump_icalls(8);
    fflush(stderr);
    TerminateProcess(GetCurrentProcess(), 2);
}

/* Added after native32's own handler, so callbacks are resolved first and only
 * real faults get here. The report goes out through WriteFile from a static
 * buffer, not stdio: a fault while another thread holds the CRT's stderr lock
 * otherwise ends with no report at all (The Movies). */
static char g_crash_buf[4096];
static int g_crash_len;
static void crash_emit(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = _vsnprintf(g_crash_buf + g_crash_len, sizeof g_crash_buf - 1 - g_crash_len, fmt, ap);
    va_end(ap);
    if (n > 0) g_crash_len += n;
}

static LONG CALLBACK crash(EXCEPTION_POINTERS* ep) {
    static volatile LONG once;
    EXCEPTION_RECORD* r = ep->ExceptionRecord;
    if ((r->ExceptionCode & 0xF0000000u) != 0xC0000000u) return EXCEPTION_CONTINUE_SEARCH;
    if (InterlockedExchange(&once, 1)) TerminateProcess(GetCurrentProcess(), 3);
    crash_emit("\n=== fault 0x%08lX at 0x%p, thread %lu ===\n", r->ExceptionCode,
               r->ExceptionAddress, GetCurrentThreadId());
    if (r->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && r->NumberParameters >= 2) {
        ULONG_PTR op = r->ExceptionInformation[0];
        uint32_t at = (uint32_t)r->ExceptionInformation[1];
        crash_emit("  %s of 0x%08X%s\n", op == 0 ? "read" : op == 1 ? "write" : "execute", at,
                   native32_in_guest(at) ? " (inside the guest image)" : at < 0x10000 ? " (null/low)" : "");
    }
    crash_emit("  in lifted sub_%08X, last native call %s\n", g_cur_func, g_cur_import);
    crash_emit("  eax=%08X ecx=%08X edx=%08X ebx=%08X esp=%08X ebp=%08X esi=%08X edi=%08X\n",
               g_eax, g_ecx, g_edx, g_ebx, g_esp, g_ebp, g_esi, g_edi);
    crash_emit("last indirect calls (newest first):\n");
    for (int i = 1; i <= 12 && i <= (int)g_icall_trace_idx; i++) {
        uint32_t k = (g_icall_trace_idx - i) & (ICALL_TRACE_SIZE - 1);
        const char* nm = native32_name(g_icall_trace[k]);
        crash_emit("  0x%08X  from 0x%08X  %s\n", g_icall_trace[k], g_icall_from[k], nm ? nm : "");
    }
    DWORD w;
    WriteFile(GetStdHandle(STD_ERROR_HANDLE), g_crash_buf, (DWORD)g_crash_len, &w, NULL);
    recomp_trace_flush();
    TerminateProcess(GetCurrentProcess(), 3);
    return EXCEPTION_CONTINUE_SEARCH;
}

static DWORD WINAPI watchdog(LPVOID unused) {
    (void)unused;
    Sleep(g_watchdog_s * 1000);
    fprintf(stderr, "\n[watchdog] %lu s: in sub_%08X, last native call %s, %u indirect calls, %ld frames\n",
            g_watchdog_s, g_cur_func, g_cur_import, g_icall_count, g_frames);
    native32_dump_icalls(8);
    record_close();
    recomp_trace_flush();
    fflush(stdout);                     /* the milestone lines the harness reads */
    fflush(stderr);
    TerminateProcess(GetCurrentProcess(), 4);
    return 0;
}

int main(int argc, char** argv) {
    const char* exe = "work\\swkotor.exe";
    const char* game = "game";
    char exe_full[MAX_PATH], game_full[MAX_PATH];
    int run = 0, sound = 0;
    for (int i = 1; i < argc; i++) {
        int n = recomp_trace_arg(argc, argv, i);
        if (n) { i += n - 1; continue; }
        if (!strcmp(argv[i], "--run")) run = 1;
        else if (!strcmp(argv[i], "--sound")) sound = 1;
        else if (!strcmp(argv[i], "--headless")) g_headless = 1;
        else if (!strcmp(argv[i], "--modeswitch")) g_modeswitch = 1;
        else if (!strcmp(argv[i], "--gamma")) g_gamma = 1;
        else if (!strcmp(argv[i], "--original")) g_original = 1;
        else if (!strcmp(argv[i], "--click") && i + 1 < argc && click_arg(argv[i + 1])) i++;
        else if (!strcmp(argv[i], "--record") && i + 1 < argc) {
            static char rec_full[MAX_PATH];     /* the run chdirs into game\ */
            GetFullPathNameA(argv[++i], MAX_PATH, rec_full, NULL);
            g_record = rec_full;
        }
        else if (!strcmp(argv[i], "--exe") && i + 1 < argc) exe = argv[++i];
        else if (!strcmp(argv[i], "--game") && i + 1 < argc) game = argv[++i];
        else if (!strcmp(argv[i], "--watchdog") && i + 1 < argc) g_watchdog_s = strtoul(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--native-trace")) native32_trace_native = 1;
        else if (!strcmp(argv[i], "--callbacks")) native32_trace_callbacks = 1;
        else {
            printf("usage: kotor [--run] [--headless] [--sound] [--modeswitch] [--gamma] [--original] [--record out.mp4] [--click x,y@s]... [--exe work\\swkotor.exe] [--game game]\n"
                   "             [--watchdog S] [--native-trace] [--callbacks]\n");
            recomp_trace_help();
            return argv[i][1] == 'h' || argv[i][2] == 'h' ? 0 : 1;
        }
    }
    GetFullPathNameA(exe, MAX_PATH, exe_full, NULL);
    GetFullPathNameA(game, MAX_PATH, game_full, NULL);
    _snprintf(g_guest_exe, sizeof g_guest_exe - 1, "%s\\swkotor.exe", game_full);
    _snprintf(g_guest_cmdline, sizeof g_guest_cmdline - 1, "\"%s\"", g_guest_exe);
    /* mss32.dll and binkw32.dll ship in the game folder; imports bind from there. */
    SetDllDirectoryA(game_full);

    if (g_original) {
        /* The same shims, mute, folder and watchdog around the original code. */
        if (!SetCurrentDirectoryA(game_full)) { fprintf(stderr, "cannot enter %s\n", game_full); return 1; }
        CoInitialize(NULL);
        if (!sound && !mute_process()) {
            fprintf(stderr, "cannot mute the process audio session; pass --sound to run with sound\n");
            return 1;
        }
        printf("  audio: %s\n", sound ? "on (--sound)" : "muted (--sound to hear it)");
        if (g_watchdog_s) CloseHandle(CreateThread(NULL, 0, watchdog, NULL, 0, NULL));
        g_t0 = GetTickCount();
        if (g_nclicks) {
            g_menu_up = CreateEventA(NULL, TRUE, FALSE, NULL);
            CloseHandle(CreateThread(NULL, 0, input_script, NULL, 0, NULL));
        }
        return oracle_run(exe_full, KOTOR_IMAGE_BASE, g_shims, (int)(sizeof g_shims / sizeof g_shims[0]), NULL, 0);
    }

    native32_init();
    AddVectoredExceptionHandler(0, crash);
    printf("KotOR recomp host\n  lifted functions in dispatch: %u\n", recomp_dispatch_count);

    uint32_t span = native32_map(exe_full, KOTOR_IMAGE_BASE);
    if (!span) { fprintf(stderr, "cannot map %s at 0x%08X\n", exe_full, KOTOR_IMAGE_BASE); return 1; }
    printf("  mapped %s: 0x%08X-0x%08X\n", exe, KOTOR_IMAGE_BASE, KOTOR_IMAGE_BASE + span);
    if (native32_bind(KOTOR_IMAGE_BASE, g_shims, (int)(sizeof g_shims / sizeof g_shims[0]))) return 1;
    printf("  guest exe %s\n", g_guest_exe);

    if (!run) {
        printf("\n(dry run: image mapped and bound; --run enters 0x%08X)\n", kotor_entry_va);
        return 0;
    }
    /* The game opens chitin.key, modules\ and swkotor.ini relative to its
     * working directory. */
    if (!SetCurrentDirectoryA(game_full)) { fprintf(stderr, "cannot enter %s\n", game_full); return 1; }
    CoInitialize(NULL);                 /* the game's own CoInitialize then answers S_FALSE */
    if (!sound && !mute_process()) {    /* fail closed: a run is silent or it does not start */
        fprintf(stderr, "cannot mute the process audio session; pass --sound to run with sound\n");
        return 1;
    }
    printf("  audio: %s\n", sound ? "on (--sound)" : "muted (--sound to hear it)");
    if (g_watchdog_s) CloseHandle(CreateThread(NULL, 0, watchdog, NULL, 0, NULL));
    g_t0 = GetTickCount();
    if (g_nclicks) {
        g_menu_up = CreateEventA(NULL, TRUE, FALSE, NULL);
        CloseHandle(CreateThread(NULL, 0, input_script, NULL, 0, NULL));
    }
    printf("  entering 0x%08X\n\n", kotor_entry_va);
    fflush(stdout);
    native32_call_guest(kotor_entry_va, 0, NULL);
    record_close();
    printf("\nentry returned eax=%08X\n", g_eax);
    return (int)g_eax;
}
