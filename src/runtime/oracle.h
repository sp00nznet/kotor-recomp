/* --original: the shipping machine code under the same host (oracle.c). */
#pragma once
#include <stdint.h>
#include "native32.h"

typedef struct { uint32_t va; recomp_func_t fn; } oracle_hook_t;

/* Map exe_full at base with its code executable, bind its imports (shims by
 * name, the rest native), patch the hooks in, and run its entry point. */
int oracle_run(const char* exe_full, uint32_t base, native32_shim_t* shims, int nshims,
               const oracle_hook_t* hooks, int nhooks);
