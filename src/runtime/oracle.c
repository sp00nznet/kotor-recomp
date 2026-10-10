/*
 * --original: run the unwrapped swkotor.exe's own machine code, natively,
 * inside this host, as the reference the recompilation is compared against.
 * Copied from Red Alert 2's host (redalert2-recomp src/runtime/oracle.c),
 * itself from civ3. ponytail: third copy; it belongs in pcrecomp's native32
 * once a fourth game wants it.
 *
 * The image is mapped at its base exactly as for the lifted run, but its code
 * is left executable and nothing is lifted: the IAT is bound to the real
 * functions, and the entry point is called on a thread of its own. The exe's
 * imports get the SAME shims the lifted run has (the display-mode and gamma
 * guards, headless, the module-name answers), so the two runs differ only
 * where the lift does.
 *
 * The shims are written for the lifted model: they read their arguments at
 * ARG(n), off g_esp, and pop them by advancing g_esp. A native caller reaches
 * one through a thunk (`mov eax, shim; jmp shim_stub`): the stub points g_esp
 * at the real stack, runs the shim, and returns with whatever purge the shim
 * applied. The register globals are saved and restored around it, because a
 * shim can re-enter (CreateWindowExA sends WM_CREATE to the game's window
 * procedure, which calls another shimmed import before the first returns).
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "native32.h"
#include "image_loader.h"
#include "oracle.h"

static CRITICAL_SECTION g_lock;        /* one shim at a time; recursive for re-entry */

static uint32_t __cdecl shim_dispatch(recomp_func_t fn, uint32_t esp, uint32_t* out_esp) {
    EnterCriticalSection(&g_lock);
    uint32_t se = g_esp, sa = g_eax;
    g_esp = esp;
    fn();
    *out_esp = g_esp;
    uint32_t r = g_eax;
    g_esp = se;
    g_eax = sa;
    LeaveCriticalSection(&g_lock);
    return r;
}

/* For a shim's blocking Windows call (host.c UNLOCKED). ponytail: one level;
 * a window shim reached from inside another shim still holds the outer claim. */
void oracle_release(void) { LeaveCriticalSection(&g_lock); }
void oracle_take(void) { EnterCriticalSection(&g_lock); }

/* eax = the shim. [esp] = the caller's return address, arguments above it. */
static __declspec(naked) void shim_stub(void) {
    __asm {
        mov ecx, esp            ; the stack as the caller left it
        sub esp, 4              ; room for the stack the shim leaves
        mov edx, esp
        push edx
        push ecx
        push eax
        call shim_dispatch
        add esp, 12
        pop edx                 ; where the shim left the stack
        mov ecx, [esp]          ; the return address
        mov esp, edx
        jmp ecx
    }
}

/* `mov eax, imm32; jmp rel32`, one per shim, in memory that may execute. */
static uint8_t* g_thunks;
static int g_nthunks;

static void* thunk(recomp_func_t fn) {
    uint8_t* t = g_thunks + 10 * g_nthunks++;
    t[0] = 0xB8;
    *(uint32_t*)(t + 1) = (uint32_t)(uintptr_t)fn;
    t[5] = 0xE9;
    *(int32_t*)(t + 6) = (int32_t)((uintptr_t)shim_stub - (uintptr_t)(t + 10));
    return t;
}

static int bind(uint32_t base, native32_shim_t* shims, int nshims) {
    BYTE* b = (BYTE*)(uintptr_t)base;
    IMAGE_NT_HEADERS32* nt = (IMAGE_NT_HEADERS32*)(b + ((IMAGE_DOS_HEADER*)b)->e_lfanew);
    IMAGE_DATA_DIRECTORY dd = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    int native = 0, shimmed = 0, missing = 0;
    for (IMAGE_IMPORT_DESCRIPTOR* d = (IMAGE_IMPORT_DESCRIPTOR*)(b + dd.VirtualAddress); d->Name; d++) {
        HMODULE h = LoadLibraryA((const char*)(b + d->Name));
        uint32_t* ilt = (uint32_t*)(b + (d->OriginalFirstThunk ? d->OriginalFirstThunk : d->FirstThunk));
        uint32_t* iat = (uint32_t*)(b + d->FirstThunk);
        for (; *ilt; ilt++, iat++) {
            int by_ord = (*ilt & 0x80000000u) != 0;
            const char* nm = by_ord ? (const char*)(uintptr_t)(*ilt & 0xFFFF) : (const char*)(b + *ilt + 2);
            void* fn = NULL;
            for (int i = 0; !by_ord && i < nshims; i++)
                if (!strcmp(nm, shims[i].name)) fn = thunk(shims[i].fn);
            if (fn) shimmed++;
            else if (h && (fn = (void*)GetProcAddress(h, nm))) native++;
            else missing++;
            *iat = (uint32_t)(uintptr_t)fn;
        }
    }
    printf("[original] imports: %d native, %d shimmed, %d unresolved\n", native, shimmed, missing);
    return missing;
}

/* First-chance exceptions, logged and passed on: the original handles its
 * own with SEH, so this only reports. */
static LONG CALLBACK note(EXCEPTION_POINTERS* ep) {
    static volatile LONG n;
    EXCEPTION_RECORD* r = ep->ExceptionRecord;
    if ((r->ExceptionCode & 0xF0000000u) == 0xC0000000u && InterlockedIncrement(&n) <= 8) {
        CONTEXT* c = ep->ContextRecord;
        /* A call through a null pointer faults at 0 with the caller at [esp]. */
        unsigned long ret = (r->ExceptionAddress == NULL && !IsBadReadPtr((void*)(uintptr_t)c->Esp, 4))
                            ? *(unsigned long*)(uintptr_t)c->Esp : 0;
        fprintf(stderr, "[original] exception 0x%08lX at %p (%s 0x%08lX) eax=%08lX ecx=%08lX esp=%08lX [esp]=%08lX\n",
                r->ExceptionCode, r->ExceptionAddress,
                r->NumberParameters >= 2 && r->ExceptionInformation[0] == 1 ? "write" : "read",
                r->NumberParameters >= 2 ? (unsigned long)r->ExceptionInformation[1] : 0,
                c->Eax, c->Ecx, c->Esp, ret);
        fflush(stderr);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

static DWORD WINAPI entry(LPVOID va) {
    ((void (*)(void))va)();               /* the CRT's entry: ends in ExitProcess */
    return 0;
}

int oracle_run(const char* exe_full, uint32_t base, native32_shim_t* shims, int nshims,
               const oracle_hook_t* hooks, int nhooks) {
    InitializeCriticalSection(&g_lock);
    g_thunks = (uint8_t*)VirtualAlloc(NULL, 10 * (nshims * 4 + nhooks + 16), MEM_COMMIT | MEM_RESERVE,
                                      PAGE_EXECUTE_READWRITE);
    uint32_t span = recomp_load_image(exe_full, base);
    if (!span) { fprintf(stderr, "cannot map %s at 0x%08X\n", exe_full, base); return 1; }
    IMAGE_NT_HEADERS32* nt = (IMAGE_NT_HEADERS32*)(uintptr_t)(base + ((IMAGE_DOS_HEADER*)(uintptr_t)base)->e_lfanew);
    IMAGE_SECTION_HEADER* s = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++, s++) {
        DWORD old;
        if (s->Characteristics & IMAGE_SCN_MEM_EXECUTE)
            VirtualProtect((void*)(uintptr_t)(base + s->VirtualAddress), s->Misc.VirtualSize,
                           PAGE_EXECUTE_READWRITE, &old);
    }
    printf("[original] mapped %s: 0x%08X-0x%08X, running the original machine code\n", exe_full, base, base + span);
    if (bind(base, shims, nshims)) return 1;
    /* The lift's hooks, as a jmp at the function's entry. */
    for (int i = 0; i < nhooks; i++) {
        uint8_t* at = (uint8_t*)(uintptr_t)hooks[i].va;
        uint8_t* t = (uint8_t*)thunk(hooks[i].fn);
        at[0] = 0xE9;
        *(int32_t*)(at + 1) = (int32_t)((uintptr_t)t - (uintptr_t)(at + 5));
    }
    uint32_t ep = base + nt->OptionalHeader.AddressOfEntryPoint;
    AddVectoredExceptionHandler(1, note);
    printf("  entering 0x%08X (original)\n\n", ep);
    fflush(stdout);
    HANDLE t = CreateThread(NULL, 16u << 20, entry, (LPVOID)(uintptr_t)ep, 0, NULL);
    WaitForSingleObject(t, INFINITE);
    return 0;
}
