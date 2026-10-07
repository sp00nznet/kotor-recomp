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
#include <shlwapi.h>

#include "native32.h"
#include "recomp_trace.h"
#include "oracle.h"
#include "input.h"

extern const uint32_t kotor_entry_va;  /* recomp_dispatch.c */

#define KOTOR_IMAGE_BASE 0x00400000u
#define ARG(n) MEM32(g_esp + 4 + 4 * (n))

static DWORD g_watchdog_s;
static int   g_modeswitch;   /* --modeswitch: let ChangeDisplaySettingsA through */
static int   g_gamma;        /* --gamma: let SetDeviceGammaRamp through */
static int   g_original;     /* --original: the shipping machine code (oracle.c) */
static int   g_private_ini_opt;   /* --private-ini: as headless does */

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
static const char* g_record;              /* --record out.mp4 */
static volatile int g_record_armed;       /* set when the main menu appears */

static void shim_AIL_open_stream(void) {
    static ail_open_stream_t real;
    if (!real) real = (ail_open_stream_t)GetProcAddress(GetModuleHandleA("mss32.dll"), "_AIL_open_stream@12");
    const char* name = (const char*)(uintptr_t)ARG(1);
    g_eax = real(ARG(0), name, (int32_t)ARG(2));
    printf("[host] stream %s -> %s\n", name ? name : "(null)", g_eax ? "open" : "FAILED");
    if (g_eax) input_menu_up();           /* the script's clock starts at the menu */
    if (g_eax) g_record_armed = 1;        /* recording starts at the menu, not on a 640x480 movie */
    g_esp += 4 + 3 * 4;
}

/* Module archives (modules\*.rim, *.mod) are opened when an area loads: the
 * Endar Spire's are the milestone after a new game, and the first frame drawn
 * after one is logged too (shim_SwapBuffers). Every other open passes through
 * unlogged; the bulk of the game's reads go through handles to the BIFs it
 * opened once at startup. */
static volatile LONG g_module_opened;

/* swkotor.ini: the game rewrites it when it exits or when a setting changes,
 * and test runs had turned the player's sound off in it (Sound Init=0) and
 * marked the movies seen. Headless runs (and --private-ini) read and write a
 * fresh copy, work\swkotor.ini, taken from the game's at start, so every test
 * starts from the same settings and the player's file is never touched. */
static char g_private_ini[MAX_PATH];
/* Saves too: the game writes saves\<n> - <name>\ beside itself, and a test that
 * quicksaves must not add to the player's. Private runs keep theirs in
 * work\saves\, which is also what lets a test start from a save of its own. */
static char g_private_saves[MAX_PATH];

/* The path the game asked for, or its private stand-in. buf holds the result
 * when it is rebuilt, so each caller passes its own. */
static const char* redirect(const char* name, char* buf) {
    if (!name || !g_private_ini[0]) return name;
    const char* base = strrchr(name, '\\');
    base = base ? base + 1 : name;
    if (!_stricmp(base, "swkotor.ini")) return g_private_ini;
    const char* p = name;
    if (p[0] == '.' && (p[1] == '\\' || p[1] == '/')) p += 2;
    if (!_strnicmp(p, "saves", 5) && (p[5] == '\\' || p[5] == '/' || p[5] == 0)) {
        _snprintf(buf, MAX_PATH - 1, "%s%s", g_private_saves, p + 5);
        buf[MAX_PATH - 1] = 0;
        static LONG once;
        if (!InterlockedExchange(&once, 1)) printf("[host] saves -> %s (first: %s)\n", g_private_saves, name);
        return buf;
    }
    return name;
}

#define PATH_ARG(n, buf) redirect((const char*)(uintptr_t)ARG(n), buf)

static void shim_CreateDirectoryA(void) {
    char b[MAX_PATH];
    g_eax = CreateDirectoryA(PATH_ARG(0, b), (LPSECURITY_ATTRIBUTES)(uintptr_t)ARG(1));
    g_esp += 4 + 2 * 4;
}
static void shim_RemoveDirectoryA(void) {
    char b[MAX_PATH];
    g_eax = RemoveDirectoryA(PATH_ARG(0, b));
    g_esp += 4 + 1 * 4;
}
static void shim_DeleteFileA(void) {
    char b[MAX_PATH];
    g_eax = DeleteFileA(PATH_ARG(0, b));
    g_esp += 4 + 1 * 4;
}
static void shim_GetFileAttributesA(void) {
    char b[MAX_PATH];
    g_eax = GetFileAttributesA(PATH_ARG(0, b));
    g_esp += 4 + 1 * 4;
}
static void shim_SetFileAttributesA(void) {
    char b[MAX_PATH];
    g_eax = SetFileAttributesA(PATH_ARG(0, b), ARG(1));
    g_esp += 4 + 2 * 4;
}
static void shim_FindFirstFileA(void) {
    char b[MAX_PATH];
    g_eax = (uint32_t)(uintptr_t)FindFirstFileA(PATH_ARG(0, b), (LPWIN32_FIND_DATAA)(uintptr_t)ARG(1));
    g_esp += 4 + 2 * 4;
}
static void shim_MoveFileA(void) {
    char a[MAX_PATH], b[MAX_PATH];
    g_eax = MoveFileA(PATH_ARG(0, a), PATH_ARG(1, b));
    g_esp += 4 + 2 * 4;
}
static void shim_CopyFileA(void) {
    char a[MAX_PATH], b[MAX_PATH];
    g_eax = CopyFileA(PATH_ARG(0, a), PATH_ARG(1, b), ARG(2));
    g_esp += 4 + 3 * 4;
}

static void shim_CreateFileA(void) {
    char rb[MAX_PATH];
    const char* name = PATH_ARG(0, rb);
    g_eax = (uint32_t)(uintptr_t)CreateFileA(name, ARG(1), ARG(2), (LPSECURITY_ATTRIBUTES)(uintptr_t)ARG(3),
                                             ARG(4), ARG(5), (HANDLE)(uintptr_t)ARG(6));
    size_t n = name ? strlen(name) : 0;
    if (n > 4 && (!_stricmp(name + n - 4, ".rim") || !_stricmp(name + n - 4, ".mod"))) {
        int ok = g_eax != (uint32_t)(uintptr_t)INVALID_HANDLE_VALUE;
        printf("[host] module %s -> %s\n", name, ok ? "open" : "missing");
        if (ok) InterlockedExchange(&g_module_opened, StrStrIA(name, "modules\\") ? 2 : 1);   /* 2: an area */
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
    if (g_eax == 0 && ARG(3)) input_on_directinput(*(void**)(uintptr_t)ARG(3));   /* the keyboard hook */
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
 * (REPO_RULES section 10/13). Sampled at 30 fps from the main menu on (the
 * intro movies are 640x480 and would fix the size); the size is fixed by the
 * first frame, and a frame of another size is skipped.
 * ponytail: a frame at another size is dropped, not scaled; scale it if the
 * movies need recording. */
#define GL_BGRA_EXT 0x80E1
#define GL_BACK_BUF 0x0405
static FILE* g_rec_pipe;
static SRWLOCK g_rec_lock = SRWLOCK_INIT;
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
    /* g_rec_next 0 is "not started": (int)GetTickCount() is negative after 24.8 days
     * of uptime, and comparing against 0 then skipped every frame. */
    if ((g_rec_next && (int)(now - g_rec_next) < 0) || !GetClientRect(WindowFromDC(dc), &rc)) return;
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
    AcquireSRWLockExclusive(&g_rec_lock);
    if (g_rec_pipe) fwrite(g_rec_buf, 4, (size_t)w * h, g_rec_pipe);
    ReleaseSRWLockExclusive(&g_rec_lock);
}

/* From the watchdog's thread as well as the main one. Closing the pipe while
 * the render thread was inside fwrite on it made the CRT fail fast
 * (0xC0000409), which ended every long recorded run at its watchdog with no
 * report: the lock keeps them apart, and a closed recording stays closed. */
static void record_close(void) {
    AcquireSRWLockExclusive(&g_rec_lock);
    if (g_rec_pipe) { _pclose(g_rec_pipe); g_rec_pipe = NULL; }
    g_record = NULL;
    ReleaseSRWLockExclusive(&g_rec_lock);
}

/* The game ends itself with ExitProcess, which skips everything the host would
 * do on the way out: the code is printed, stdout flushed, the recording closed. */
/* The C runtime's abort path ends the process with TerminateProcess, not
 * ExitProcess; the same report, then the real call. */
static void shim_TerminateProcess(void) {
    HANDLE h = (HANDLE)(uintptr_t)ARG(0);
    UINT code = ARG(1);
    if (h == GetCurrentProcess() || GetProcessId(h) == GetCurrentProcessId()) {
        printf("[host] TerminateProcess(self, %u) from sub_%08X\n", code, g_cur_func);
        fflush(stdout);
        native32_dump_icalls(8);
        record_close();
    }
    g_eax = TerminateProcess(h, code);
    g_esp += 4 + 2 * 4;
}

static void shim_ExitProcess(void) {
    UINT code = ARG(0);
    printf("[host] ExitProcess(%u) from sub_%08X\n", code, g_cur_func);
    fflush(stdout);
    record_close();
    ExitProcess(code);
}

/* The GL the game got, once: a run on a machine without the vendor's driver gets
 * Windows' generic 1.1 renderer, and that explains most of what happens next. */
static void print_gl(void) {
    typedef const char* (WINAPI *get_string_t)(unsigned);
    get_string_t gs = (get_string_t)GetProcAddress(GetModuleHandleA("opengl32.dll"), "glGetString");
    if (gs) printf("[host] GL: %s / %s / %s\n", gs(0x1F00), gs(0x1F01), gs(0x1F02));   /* vendor, renderer, version */
    fflush(stdout);                       /* a crash right after must not lose it */
}

/* wglGetProcAddress: the game calls wglSwapIntervalEXT through what this
 * returns without checking it (0x0044E381, with V-Sync), so a driver without
 * the extension (the generic renderer on a GPU-less session) sent it to 0.
 * Missing swap-interval entry points get no-op stand-ins; everything else is
 * passed through, null included. */
static BOOL WINAPI no_swap_interval(int n) { (void)n; return TRUE; }
static int WINAPI no_get_swap_interval(void) { return 1; }

static void shim_wglGetProcAddress(void) {
    typedef PROC (WINAPI *gpa_t)(LPCSTR);
    static gpa_t real;
    if (!real) real = (gpa_t)GetProcAddress(GetModuleHandleA("opengl32.dll"), "wglGetProcAddress");
    const char* name = (const char*)(uintptr_t)ARG(0);
    PROC p = real(name);
    if (!p && name && !strcmp(name, "wglSwapIntervalEXT")) p = (PROC)no_swap_interval;
    if (!p && name && !strcmp(name, "wglGetSwapIntervalEXT")) p = (PROC)no_get_swap_interval;
    g_eax = (uint32_t)(uintptr_t)p;
    g_esp += 4 + 1 * 4;
}

static void shim_wglMakeCurrent(void) {
    static LONG printed;
    g_eax = wglMakeCurrent((HDC)(uintptr_t)ARG(0), (HGLRC)(uintptr_t)ARG(1));
    if (g_eax && ARG(1) && !InterlockedExchange(&printed, 1)) print_gl();
    g_esp += 4 + 2 * 4;
}

static void shim_SwapBuffers(void) {
    LONG n = InterlockedIncrement(&g_frames);
    if (n == 1 || n == 60 || n % 1000 == 0) printf("[host] frame %ld presented\n", n);
    LONG mod = InterlockedExchange(&g_module_opened, 0);
    if (mod) printf("[host] frame %ld presented after a module load\n", n);
    if (mod == 2) input_area_up();
    HDC dc = (HDC)(uintptr_t)ARG(0);
    input_on_frame(WindowFromDC(dc));     /* every frame: the game recreates its window */
    if (g_record && g_record_armed) record_frame(dc);
    g_eax = SwapBuffers(dc);
    g_esp += 4 + 1 * 4;
}

static native32_shim_t g_shims[] = {
    { "CreateWindowExA", shim_CreateWindowExA },
    { "ShowWindow", shim_ShowWindow },
    { "MessageBoxA", shim_MessageBoxA },
    { "SwapBuffers", shim_SwapBuffers },
    { "wglGetProcAddress", shim_wglGetProcAddress },
    { "wglMakeCurrent", shim_wglMakeCurrent },
    { "ExitProcess", shim_ExitProcess },
    { "TerminateProcess", shim_TerminateProcess },
    { "ChangeDisplaySettingsA", shim_ChangeDisplaySettingsA },
    { "SetDeviceGammaRamp", shim_SetDeviceGammaRamp },
    { "GetModuleHandleA", shim_GetModuleHandleA },
    { "GetModuleFileNameA", shim_GetModuleFileNameA },
    { "GetCommandLineA", shim_GetCommandLineA },
    { "DirectInput8Create", shim_DirectInput8Create },
    { "_AIL_open_stream@12", shim_AIL_open_stream },
    { "_BinkOpen@8", shim_BinkOpen },
    { "CreateFileA", shim_CreateFileA },
    { "CreateDirectoryA", shim_CreateDirectoryA },
    { "RemoveDirectoryA", shim_RemoveDirectoryA },
    { "DeleteFileA", shim_DeleteFileA },
    { "GetFileAttributesA", shim_GetFileAttributesA },
    { "SetFileAttributesA", shim_SetFileAttributesA },
    { "FindFirstFileA", shim_FindFirstFileA },
    { "MoveFileA", shim_MoveFileA },
    { "CopyFileA", shim_CopyFileA },
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

/* 1: muted; 2: there is no audio device; 0: failed. */
static int mute_process(void) {
    IMMDeviceEnumerator* en = NULL;
    IMMDevice* dev = NULL;
    IAudioSessionManager* mgr = NULL;
    ISimpleAudioVolume* vol = NULL;
    HRESULT hr = CoCreateInstance(&kCLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL,
                                  &kIID_IMMDeviceEnumerator, (void**)&en);
    if (SUCCEEDED(hr)) hr = IMMDeviceEnumerator_GetDefaultAudioEndpoint(en, eRender, eConsole, &dev);
    /* No output device at all (a test VM): nothing can be heard, which is what
     * muting is for. Any other failure still stops the run. */
    if (hr == HRESULT_FROM_WIN32(ERROR_NOT_FOUND)) { IMMDeviceEnumerator_Release(en); return 2; }
    if (SUCCEEDED(hr)) hr = IMMDevice_Activate(dev, &kIID_IAudioSessionManager, CLSCTX_ALL, NULL, (void**)&mgr);
    if (SUCCEEDED(hr)) hr = IAudioSessionManager_GetSimpleAudioVolume(mgr, &GUID_NULL, FALSE, &vol);
    if (SUCCEEDED(hr)) hr = ISimpleAudioVolume_SetMute(vol, TRUE, NULL);
    if (vol) ISimpleAudioVolume_Release(vol);    /* the session keeps its mute */
    if (mgr) IAudioSessionManager_Release(mgr);
    if (dev) IMMDevice_Release(dev);
    if (en) IMMDeviceEnumerator_Release(en);
    return SUCCEEDED(hr);
}

/* Muted unless --sound; 0 when it could not be made silent and the run must stop. */
static int audio_setup(int sound) {
    int m = sound ? 1 : mute_process();
    if (!m) {
        fprintf(stderr, "cannot mute the process audio session; pass --sound to run with sound\n");
        return 0;
    }
    printf("  audio: %s\n", sound ? "on (--sound)" : m == 2 ? "muted (no audio device)"
                                                    : "muted (--sound to hear it)");
    return 1;
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
    if (argc == 2 && !strcmp(argv[1], "--selftest-input")) return input_selftest();
    for (int i = 1; i < argc; i++) {
        int n = recomp_trace_arg(argc, argv, i);
        if (!n) n = input_arg(argc, argv, i);
        if (n) { i += n - 1; continue; }
        if (!strcmp(argv[i], "--run")) run = 1;
        else if (!strcmp(argv[i], "--sound")) sound = 1;
        else if (!strcmp(argv[i], "--headless")) g_headless = 1;
        else if (!strcmp(argv[i], "--modeswitch")) g_modeswitch = 1;
        else if (!strcmp(argv[i], "--gamma")) g_gamma = 1;
        else if (!strcmp(argv[i], "--original")) g_original = 1;
        else if (!strcmp(argv[i], "--private-ini")) g_private_ini_opt = 1;
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
            printf("usage: kotor [--run] [--headless] [--sound] [--modeswitch] [--gamma] [--original] [--private-ini] [--record out.mp4] [--exe work\\swkotor.exe] [--game game]\n"
                   "             [--watchdog S] [--native-trace] [--callbacks]\n");
            input_help();
            recomp_trace_help();
            return argv[i][1] == 'h' || argv[i][2] == 'h' ? 0 : 1;
        }
    }
    GetFullPathNameA(exe, MAX_PATH, exe_full, NULL);
    GetFullPathNameA(game, MAX_PATH, game_full, NULL);
    if (g_headless || g_private_ini_opt) {      /* before the run chdirs into game\ */
        char src[MAX_PATH];
        CreateDirectoryA("work", NULL);
        GetFullPathNameA("work\\swkotor.ini", MAX_PATH, g_private_ini, NULL);
        _snprintf(src, sizeof src - 1, "%s\\swkotor.ini", game_full);
        if (!CopyFileA(src, g_private_ini, FALSE)) {
            fprintf(stderr, "cannot copy %s to %s\n", src, g_private_ini);
            return 1;
        }
        printf("  swkotor.ini: a fresh copy at %s\n", g_private_ini);
        GetFullPathNameA("work\\saves", MAX_PATH, g_private_saves, NULL);
        CreateDirectoryA(g_private_saves, NULL);   /* kept between runs: a test may load one */
        printf("  saves: %s\n", g_private_saves);
    }
    _snprintf(g_guest_exe, sizeof g_guest_exe - 1, "%s\\swkotor.exe", game_full);
    _snprintf(g_guest_cmdline, sizeof g_guest_cmdline - 1, "\"%s\"", g_guest_exe);
    /* mss32.dll and binkw32.dll ship in the game folder; imports bind from there. */
    SetDllDirectoryA(game_full);

    if (g_original) {
        /* The same shims, mute, folder and watchdog around the original code. */
        if (!SetCurrentDirectoryA(game_full)) { fprintf(stderr, "cannot enter %s\n", game_full); return 1; }
        CoInitialize(NULL);
        if (!audio_setup(sound)) return 1;
        if (g_watchdog_s) CloseHandle(CreateThread(NULL, 0, watchdog, NULL, 0, NULL));
        input_start(g_headless);
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
    if (!audio_setup(sound)) return 1;  /* fail closed: a run is silent or it does not start */
    if (g_watchdog_s) CloseHandle(CreateThread(NULL, 0, watchdog, NULL, 0, NULL));
    input_start(g_headless);
    printf("  entering 0x%08X\n\n", kotor_entry_va);
    fflush(stdout);
    native32_call_guest(kotor_entry_va, 0, NULL);
    record_close();
    printf("\nentry returned eax=%08X\n", g_eax);
    return (int)g_eax;
}
