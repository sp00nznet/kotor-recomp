/*
 * Input the host gives the game: scripted clicks and keys, and a gamepad.
 *
 * Mouse: the game takes buttons and positions from window messages (its window
 * procedure, around 0x00403000, flips y with the window height), so a click is
 * a move, press and release posted to the window it renders into. That works
 * with the window hidden.
 *
 * Keyboard: the game reads DirectInput, not messages: a buffered device,
 * GetDeviceData(cb = 0x14, 256 entries) every frame (0x005E3B60). Its window is
 * NONEXCLUSIVE | FOREGROUND, so a hidden or unfocused window gets no real keys
 * at all. The keyboard device's GetDeviceData is hooked: the real events, then
 * the injected ones, and DI_OK even when DirectInput refused (not acquired),
 * so a script drives the game headless.
 *
 * Gamepad: KotOR on PC has no controller support. An XInput pad is read at
 * 120 Hz and turned into the game's own keys (keymap.2da defaults, tools/
 * keymap.py) plus a cursor and clicks; the table is in docs/gamepad.md. It is
 * off in --headless runs unless asked for, so a test never reads a pad that
 * happens to be plugged in.
 *
 * Script times are seconds after the main menu appears (its music opens),
 * because how long the intro takes varies several-fold with machine load. A
 * time written `a<seconds>` counts from the first frame drawn after an area
 * (modules\*.rim) loads instead, for keys meant for the game itself.
 */
#define WIN32_LEAN_AND_MEAN
#define DIRECTINPUT_VERSION 0x0800
#include <windows.h>
#include <dinput.h>
#include <stdio.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "input.h"

/* ---- injected keys -----------------------------------------------------------
 * A real key press lands in the buffer of every keyboard device an application
 * has open, so an injected one does too: each device gets its own queue and a
 * key event goes into all of them. The game opens more keyboard devices when an
 * area loads (four by the Endar Spire) and reads more than one: handing each
 * event to only the first device that read let dialogue take keys while the
 * player never walked.
 * ponytail: released devices keep their queue (it fills and drops). A new
 * device at a reused address inherits it; harmless while the game reads only
 * keyboards with GetDeviceData (the mouse is GetDeviceState). Hook Release if
 * that changes. */
#define KEYQ 256
#define MAX_KBD 16
typedef struct {
    IDirectInputDevice8A* dev;
    struct { DWORD dik, down; } q[KEYQ];
    int head, tail;
} kbd_t;
static CRITICAL_SECTION g_kq_lock;
static kbd_t g_kbd[MAX_KBD];
static int g_nkbd;
static DWORD g_kq_seq = 0x10000000u;        /* clear of DirectInput's own sequence numbers */

static kbd_t* keyboard(IDirectInputDevice8A* dev) {
    for (int i = 0; i < g_nkbd; i++)
        if (g_kbd[i].dev == dev) return &g_kbd[i];
    return NULL;
}

static void key_event(DWORD dik, int down) {
    EnterCriticalSection(&g_kq_lock);
    for (int i = 0; i < g_nkbd; i++) {
        kbd_t* k = &g_kbd[i];
        int next = (k->tail + 1) % KEYQ;
        if (next == k->head) continue;        /* full: drop rather than block a pad thread */
        k->q[k->tail].dik = dik;
        k->q[k->tail].down = down;
        k->tail = next;
    }
    LeaveCriticalSection(&g_kq_lock);
}

typedef HRESULT (WINAPI *get_data_t)(IDirectInputDevice8A*, DWORD, LPDIDEVICEOBJECTDATA, LPDWORD, DWORD);
typedef HRESULT (WINAPI *create_device_t)(IDirectInput8A*, REFGUID, LPDIRECTINPUTDEVICE8A*, LPUNKNOWN);
static get_data_t g_real_get_data;
static create_device_t g_real_create_device;

/* One vtable is shared by every device of a class (the mouse too), so the hook
 * checks which device it was called on. */
static HRESULT WINAPI hk_GetDeviceData(IDirectInputDevice8A* dev, DWORD cb, LPDIDEVICEOBJECTDATA data,
                                       LPDWORD inout, DWORD flags) {
    kbd_t* k = inout ? keyboard(dev) : NULL;
    if (!k) return g_real_get_data(dev, cb, data, inout, flags);
    DWORD cap = *inout;
    HRESULT hr = g_real_get_data(dev, cb, data, inout, flags);
    if (!data || cb != sizeof(DIDEVICEOBJECTDATA) || (flags & DIGDD_PEEK))
        return hr;                            /* a flush or a peek: leave the queue alone */
    DWORD n = SUCCEEDED(hr) ? *inout : 0;
    EnterCriticalSection(&g_kq_lock);
    while (n < cap && k->head != k->tail) {
        DIDEVICEOBJECTDATA* e = &data[n++];
        memset(e, 0, sizeof *e);
        e->dwOfs = k->q[k->head].dik;
        e->dwData = k->q[k->head].down ? 0x80 : 0;
        e->dwTimeStamp = GetTickCount();
        e->dwSequence = g_kq_seq++;
        k->head = (k->head + 1) % KEYQ;
    }
    LeaveCriticalSection(&g_kq_lock);
    *inout = n;
    return DI_OK;                             /* injected keys count even when DirectInput refused */
}

static void patch_slot(void** vt, int slot, void* fn, void** real) {
    DWORD old;
    if (*real) return;                        /* the class's vtable is patched once */
    VirtualProtect(&vt[slot], sizeof(void*), PAGE_READWRITE, &old);
    *real = vt[slot];
    vt[slot] = fn;
    VirtualProtect(&vt[slot], sizeof(void*), old, &old);
}

static HRESULT WINAPI hk_CreateDevice(IDirectInput8A* di, REFGUID g, LPDIRECTINPUTDEVICE8A* out, LPUNKNOWN u) {
    HRESULT hr = g_real_create_device(di, g, out, u);
    if (SUCCEEDED(hr) && out && *out && IsEqualGUID(g, &GUID_SysKeyboard)) {
        EnterCriticalSection(&g_kq_lock);
        kbd_t* k = keyboard(*out);            /* a reused address keeps its slot */
        if (!k && g_nkbd < MAX_KBD) k = &g_kbd[g_nkbd++];
        if (k) { k->dev = *out; k->head = k->tail = 0; }
        LeaveCriticalSection(&g_kq_lock);
        patch_slot(*(void***)*out, 10, (void*)hk_GetDeviceData, (void**)&g_real_get_data);   /* GetDeviceData */
        printf("[input] keyboard device %p hooked (%d)\n", (void*)*out, g_nkbd);
    }
    return hr;
}

static void lock_init(void) {                /* from whichever comes first */
    static volatile LONG state;               /* 0 none, 1 initialising, 2 ready */
    if (InterlockedCompareExchange(&state, 1, 0) == 0) { InitializeCriticalSection(&g_kq_lock); state = 2; }
    while (state != 2) Sleep(0);
}

void input_on_directinput(void* di8) {
    lock_init();
    if (di8) patch_slot(*(void***)di8, 3, (void*)hk_CreateDevice, (void**)&g_real_create_device);   /* CreateDevice */
}

/* ---- the window and the cursor ------------------------------------------------- */
static volatile HWND g_hwnd;
static POINT g_cursor = { 400, 300 };       /* client coordinates, top-left origin */

/* A hidden window is never activated or focused, and the game keys its world
 * input off WM_SETFOCUS (0x004028C6 -> 0x005EDA10(1, 0)) and WM_ACTIVATEAPP
 * (0x00402951): without them its menus and dialogue took keys but the player
 * would not walk. In a headless run each new game window is told it is the
 * active, focused one, as Windows tells a shown window that has the focus. */
static int g_headless_input;

void input_on_frame(void* hwnd) {
    if (!hwnd || (HWND)hwnd == g_hwnd) return;
    g_hwnd = (HWND)hwnd;
    if (g_headless_input) {
        PostMessageA(g_hwnd, WM_ACTIVATEAPP, TRUE, 0);
        PostMessageA(g_hwnd, WM_SETFOCUS, 0, 0);
    }
}

static void mouse_move(int x, int y) {
    HWND h = g_hwnd;
    RECT rc;
    if (!h) return;
    if (GetClientRect(h, &rc)) {
        x = x < 0 ? 0 : x >= rc.right ? rc.right - 1 : x;
        y = y < 0 ? 0 : y >= rc.bottom ? rc.bottom - 1 : y;
    }
    g_cursor.x = x, g_cursor.y = y;
    PostMessageA(h, WM_MOUSEMOVE, 0, MAKELPARAM(x, y));
    if (IsWindowVisible(h) && GetForegroundWindow() == h) {   /* a shown window: the real cursor too */
        POINT p = g_cursor;
        ClientToScreen(h, &p);
        SetCursorPos(p.x, p.y);
    }
}

static void mouse_button(int down) {
    HWND h = g_hwnd;
    if (h) PostMessageA(h, down ? WM_LBUTTONDOWN : WM_LBUTTONUP, down ? MK_LBUTTON : 0,
                        MAKELPARAM(g_cursor.x, g_cursor.y));
}

/* ---- the script ---------------------------------------------------------------- */
#define MAX_EVENTS 128
typedef struct { int area; DWORD ms; int kind; int x, y; DWORD dik; } event_t;   /* kind: 0 click, 1 key down, 2 key up */
static event_t g_ev[MAX_EVENTS];
static int g_nev;
static HANDLE g_menu_up, g_area_up;

static const struct { const char* name; DWORD dik; } g_keys[] = {
    { "ESCAPE", DIK_ESCAPE }, { "ESC", DIK_ESCAPE }, { "SPACE", DIK_SPACE }, { "TAB", DIK_TAB },
    { "ENTER", DIK_RETURN }, { "RETURN", DIK_RETURN }, { "LSHIFT", DIK_LSHIFT }, { "LCTRL", DIK_LCONTROL },
    { "DELETE", DIK_DELETE }, { "UP", DIK_UP }, { "DOWN", DIK_DOWN }, { "LEFT", DIK_LEFT }, { "RIGHT", DIK_RIGHT },
    { "A", DIK_A }, { "B", DIK_B }, { "C", DIK_C }, { "D", DIK_D }, { "E", DIK_E }, { "F", DIK_F },
    { "G", DIK_G }, { "H", DIK_H }, { "I", DIK_I }, { "J", DIK_J }, { "K", DIK_K }, { "L", DIK_L },
    { "M", DIK_M }, { "N", DIK_N }, { "O", DIK_O }, { "P", DIK_P }, { "Q", DIK_Q }, { "R", DIK_R },
    { "S", DIK_S }, { "T", DIK_T }, { "U", DIK_U }, { "V", DIK_V }, { "W", DIK_W }, { "X", DIK_X },
    { "Y", DIK_Y }, { "Z", DIK_Z }, { "1", DIK_1 }, { "2", DIK_2 }, { "3", DIK_3 }, { "4", DIK_4 },
    { "5", DIK_5 }, { "6", DIK_6 }, { "7", DIK_7 }, { "8", DIK_8 }, { "9", DIK_9 }, { "0", DIK_0 },
    { "F1", DIK_F1 }, { "F2", DIK_F2 }, { "F3", DIK_F3 }, { "F4", DIK_F4 }, { "F5", DIK_F5 },
    { "F6", DIK_F6 }, { "F7", DIK_F7 }, { "F8", DIK_F8 }, { "F9", DIK_F9 }, { "F10", DIK_F10 },
};

static DWORD dik_from_name(const char* s, size_t n) {
    for (size_t i = 0; i < sizeof g_keys / sizeof g_keys[0]; i++)
        if (strlen(g_keys[i].name) == n && !_strnicmp(g_keys[i].name, s, n)) return g_keys[i].dik;
    return 0;
}

static int cmp_event(const void* a, const void* b) {
    const event_t *x = a, *y = b;
    if (x->area != y->area) return x->area - y->area;   /* menu-timed first */
    return x->ms < y->ms ? -1 : x->ms > y->ms ? 1 : 0;
}

/* "12.5" from the menu, "a12.5" from the area; returns the anchor, -1 if bad. */
static int parse_time(const char* s, double* sec) {
    int area = (*s == 'a' || *s == 'A');
    return sscanf(s + area, "%lf", sec) == 1 ? area : -1;
}

static DWORD WINAPI script_thread(LPVOID unused) {
    (void)unused;
    WaitForSingleObject(g_menu_up, INFINITE);
    DWORD t0 = GetTickCount();
    printf("[input] main menu up, script starts\n");
    fflush(stdout);
    qsort(g_ev, g_nev, sizeof g_ev[0], cmp_event);
    int anchor = 0;
    for (int i = 0; i < g_nev; i++) {
        if (g_ev[i].area && !anchor) {
            WaitForSingleObject(g_area_up, INFINITE);
            t0 = GetTickCount();
            anchor = 1;
            printf("[input] area up, the a-timed script starts\n");
        }
        DWORD now = GetTickCount() - t0;
        if (g_ev[i].ms > now) Sleep(g_ev[i].ms - now);
        if (g_ev[i].kind == 0) {
            printf("[input] click %d,%d at %.1f s -> %p\n", g_ev[i].x, g_ev[i].y,
                   (GetTickCount() - t0) / 1000.0, (void*)g_hwnd);
            mouse_move(g_ev[i].x, g_ev[i].y);
            Sleep(100);
            mouse_button(1);
            Sleep(100);
            mouse_button(0);
        } else {
            printf("[input] key 0x%02lX %s at %.1f s\n", g_ev[i].dik, g_ev[i].kind == 1 ? "down" : "up",
                   (GetTickCount() - t0) / 1000.0);
            key_event(g_ev[i].dik, g_ev[i].kind == 1);
        }
        fflush(stdout);
    }
    return 0;
}

void input_menu_up(void) {
    if (g_menu_up) SetEvent(g_menu_up);
}

void input_area_up(void) {
    if (g_area_up) SetEvent(g_area_up);
}

/* ---- the gamepad ----------------------------------------------------------------
 * Mapping: docs/gamepad.md. Keys are the game's defaults from keymap.2da. */
typedef struct { WORD buttons; BYTE lt, rt; SHORT lx, ly, rx, ry; } pad_t;   /* XINPUT_GAMEPAD */
typedef DWORD (WINAPI *xinput_get_state_t)(DWORD, void*);

#define PAD_STICK_DEADZONE 9000      /* of 32767: a resting stick reads up to ~7800 */
#define PAD_TRIGGER_ON     60        /* of 255 */
#define PAD_CURSOR_SPEED   12.0      /* pixels per 120 Hz tick at full deflection */

static int g_gamepad = -1;           /* -1: default (on unless headless) */

static const struct { WORD mask; DWORD dik; int click; } g_pad_buttons[] = {
    { 0x1000, 0, 1 },                 /* A: left click */
    { 0x2000, DIK_ESCAPE, 0 },        /* B: back / close */
    { 0x4000, DIK_R, 0 },             /* X: DefaultAction */
    { 0x8000, DIK_SPACE, 0 },         /* Y: Pause */
    { 0x0100, DIK_Q, 0 },             /* LB: SelectPrev (target) */
    { 0x0200, DIK_E, 0 },             /* RB: SelectNext */
    { 0x0004, DIK_1, 0 },             /* D-pad left: TargetLeftAct / reply 1 */
    { 0x0001, DIK_2, 0 },             /* D-pad up: TargetMiddleAct / reply 2 */
    { 0x0008, DIK_3, 0 },             /* D-pad right: TargetRightAct / reply 3 */
    { 0x0002, DIK_4, 0 },             /* D-pad down: PersonalPowerAct / reply 4 */
    { 0x0020, DIK_TAB, 0 },           /* Back: ChangeChar (party leader) */
    { 0x0010, DIK_ESCAPE, 0 },        /* Start: the game menu */
};

/* One poll's worth of the mapping, with no Windows in it, so input_selftest()
 * can drive it: the pad's state in, key / button / cursor events out. */
typedef struct { WORD prev; int held[6]; double cx, cy; } pad_state_t;
typedef struct {
    void (*key)(DWORD dik, int down);
    void (*button)(int down);
    void (*move)(int dx, int dy);
} pad_out_t;

static void pad_step(const pad_t* p, pad_state_t* st, const pad_out_t* out) {
    const struct { int want; DWORD dik; } axes[6] = {
        { p->ly > PAD_STICK_DEADZONE, DIK_W },     /* ActionUp */
        { p->ly < -PAD_STICK_DEADZONE, DIK_S },    /* ActionDown */
        { p->lx < -PAD_STICK_DEADZONE, DIK_Z },    /* ActionLeft: strafe */
        { p->lx > PAD_STICK_DEADZONE, DIK_C },     /* ActionRight: strafe */
        { p->lt > PAD_TRIGGER_ON, DIK_A },         /* CameraRotateLeft */
        { p->rt > PAD_TRIGGER_ON, DIK_D },         /* CameraRotateRight */
    };
    for (int i = 0; i < 6; i++)
        if (axes[i].want != st->held[i]) { out->key(axes[i].dik, axes[i].want); st->held[i] = axes[i].want; }
    for (size_t i = 0; i < sizeof g_pad_buttons / sizeof g_pad_buttons[0]; i++) {
        WORD m = g_pad_buttons[i].mask;
        if ((p->buttons & m) == (st->prev & m)) continue;
        int down = (p->buttons & m) != 0;
        if (g_pad_buttons[i].click) out->button(down);
        else out->key(g_pad_buttons[i].dik, down);
    }
    st->prev = p->buttons;
    /* Right stick: the cursor, squared so small pushes are fine and full ones fast. */
    double fx = abs(p->rx) > PAD_STICK_DEADZONE ? p->rx / 32767.0 : 0;
    double fy = abs(p->ry) > PAD_STICK_DEADZONE ? -p->ry / 32767.0 : 0;
    st->cx += fx * fabs(fx) * PAD_CURSOR_SPEED;
    st->cy += fy * fabs(fy) * PAD_CURSOR_SPEED;
    int dx = (int)st->cx, dy = (int)st->cy;
    st->cx -= dx, st->cy -= dy;
    if (dx || dy) out->move(dx, dy);
}

static void pad_move(int dx, int dy) { mouse_move(g_cursor.x + dx, g_cursor.y + dy); }
static const pad_out_t g_pad_out = { key_event, mouse_button, pad_move };

static DWORD WINAPI gamepad_thread(LPVOID unused) {
    (void)unused;
    HMODULE x = LoadLibraryA("xinput1_4.dll");
    if (!x) x = LoadLibraryA("xinput9_1_0.dll");
    xinput_get_state_t get = x ? (xinput_get_state_t)GetProcAddress(x, "XInputGetState") : NULL;
    if (!get) { printf("[gamepad] no XInput on this system\n"); return 0; }
    struct { DWORD packet; pad_t pad; } xs;
    pad_state_t st = { 0 };
    int connected = -1;
    for (;;) {
        Sleep(8);
        int ok = get(0, &xs) == ERROR_SUCCESS;
        if (ok != connected) { printf("[gamepad] %s\n", ok ? "connected" : "not connected"); fflush(stdout); connected = ok; }
        if (!ok) memset(&xs, 0, sizeof xs);       /* unplugged: everything is released */
        pad_step(&xs.pad, &st, &g_pad_out);
    }
}

/* ---- selftest ---------------------------------------------------------------------
 * kotor.exe --selftest-input: the gamepad mapping and the --key/--click parser. */
static char g_log[512];
static void t_key(DWORD dik, int down) { _snprintf(g_log + strlen(g_log), 32, "k%02lX%c ", dik, down ? 'd' : 'u'); }
static void t_button(int down) { strcat(g_log, down ? "bd " : "bu "); }
static int g_mx, g_my;
static void t_move(int dx, int dy) { g_mx += dx, g_my += dy; }

int input_selftest(void) {
    pad_out_t o = { t_key, t_button, t_move };
    pad_state_t st = { 0 };
    pad_t p = { 0 };
#define STEP(expect) do { g_log[0] = 0; pad_step(&p, &st, &o); \
        if (strcmp(g_log, expect)) { printf("input selftest: got \"%s\", want \"%s\" (line %d)\n", g_log, expect, __LINE__); return 1; } } while (0)
    STEP("");                                            /* at rest: nothing */
    p.ly = 7000; STEP("");                               /* inside the deadzone */
    p.ly = 32000; STEP("k11d ");                         /* push up: W down */
    STEP("");                                            /* held: no repeat */
    p.ly = 0; p.lx = -32000; STEP("k11u k2Cd ");         /* to the left: W up, Z down */
    p.lx = 0; p.rt = 255; STEP("k2Cu k20d ");            /* RT: D (camera right) */
    p.rt = 0; p.buttons = 0x1000; STEP("k20u bd ");      /* A: left button down */
    p.buttons = 0x1000 | 0x0004; STEP("k02d ");          /* + D-pad left: key 1 */
    p.buttons = 0; STEP("bu k02u ");
    p.buttons = 0x2000 | 0x0010; STEP("k01d k01d ");     /* B and Start are both Escape */
    p.buttons = 0; STEP("k01u k01u ");
    g_mx = g_my = 0; p.rx = 32767; p.ry = -32767;        /* right stick down-right, full */
    for (int i = 0; i < 10; i++) STEP("");
    if (g_mx < 110 || g_mx > 120 || g_my < 110 || g_my > 120) {
        printf("input selftest: cursor moved %d,%d in 10 ticks, want ~120,120\n", g_mx, g_my);
        return 1;
    }
#undef STEP
    char* a1[] = { "kotor", "--key", "W@a30:3000", "--click", "488,293@3", "--key", "Q@x" };
    int n0 = g_nev;
    if (input_arg(7, a1, 1) != 2 || input_arg(7, a1, 3) != 2 || input_arg(7, a1, 5) != 0 ||
        g_nev != n0 + 3 || g_ev[n0].area != 1 || g_ev[n0].ms != 30000 || g_ev[n0 + 1].ms != 33000 ||
        g_ev[n0 + 2].area != 0 || g_ev[n0 + 2].ms != 3000 || g_ev[n0 + 2].x != 488) {
        printf("input selftest: --key/--click parsing\n");
        return 1;
    }
    g_nev = n0;
    printf("input selftest: ok\n");
    return 0;
}

/* ---- options -------------------------------------------------------------------- */
int input_arg(int argc, char** argv, int i) {
    const char* a = argv[i];
    double s;
    if (!strcmp(a, "--gamepad")) { g_gamepad = 1; return 1; }
    if (!strcmp(a, "--no-gamepad")) { g_gamepad = 0; return 1; }
    if (i + 1 >= argc || g_nev + 2 > MAX_EVENTS) return 0;
    const char* v = argv[i + 1];
    if (!strcmp(a, "--click")) {
        event_t* e = &g_ev[g_nev];
        const char* at = strchr(v, '@');
        if (!at || sscanf(v, "%d,%d", &e->x, &e->y) != 2 || (e->area = parse_time(at + 1, &s)) < 0) return 0;
        e->kind = 0, e->ms = (DWORD)(s * 1000);
        g_nev++;
        return 2;
    }
    if (!strcmp(a, "--key")) {
        const char* at = strchr(v, '@');
        unsigned hold_ms = 100;
        DWORD dik = at ? dik_from_name(v, (size_t)(at - v)) : 0;
        int area = dik ? parse_time(at + 1, &s) : -1;
        const char* colon = at ? strchr(at, ':') : NULL;
        if (area < 0 || (colon && sscanf(colon + 1, "%u", &hold_ms) != 1)) return 0;
        g_ev[g_nev++] = (event_t){ area, (DWORD)(s * 1000), 1, 0, 0, dik };
        g_ev[g_nev++] = (event_t){ area, (DWORD)(s * 1000) + hold_ms, 2, 0, 0, dik };
        return 2;
    }
    return 0;
}

void input_help(void) {
    printf("  input: [--click x,y@s]... [--key NAME@s[:hold_ms]]... [--gamepad | --no-gamepad]\n"
           "         times are seconds after the main menu appears, or a<seconds> after an area loads;\n"
           "         keys: A-Z 0-9 F1-F10 SPACE ESC\n"
           "         TAB ENTER LSHIFT LCTRL DELETE UP DOWN LEFT RIGHT. The pad is on unless --headless.\n");
}

void input_start(int headless) {
    g_headless_input = headless;
    lock_init();
    g_menu_up = CreateEventA(NULL, TRUE, FALSE, NULL);
    g_area_up = CreateEventA(NULL, TRUE, FALSE, NULL);
    if (g_nev) CloseHandle(CreateThread(NULL, 0, script_thread, NULL, 0, NULL));
    if (g_gamepad == 1 || (g_gamepad == -1 && !headless))
        CloseHandle(CreateThread(NULL, 0, gamepad_thread, NULL, 0, NULL));
}
