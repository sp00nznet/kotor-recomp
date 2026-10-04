#!/usr/bin/env python3
"""KotOR lift driver: work/functions.json -> src/recomp/gen/.

The same shape as Red Alert 2's and The Movies' run_lift.py, on pcrecomp's
shared `tools/lift/generate.py` + `lift32` (docs/architecture.md):

* **Closure-limited lifting** (`generate.closure`). Lift the call-graph closure
  from the OEP and give every other catalogued function a stub that aborts
  naming itself, so a run says what to lift next. `--all` lifts everything
  (swkotor.exe is ~2.6M instructions).
* **Extents by walking the branches** (`generate.true_extent`).
* **Vtable slots as entries.** KotOR shipped without RTTI, so the slots come
  from `cpp/vtable_scan.py` (work/vtable_seeds.json): a method reachable only
  through a vtable is named by no call, and an unresolved slot answers eax = 0.

    py -3 run_lift.py                      # OEP closure, 3000 functions
    py -3 run_lift.py --max 20000
    py -3 run_lift.py --roots 0x006FB38D
    py -3 run_lift.py --all
"""
import argparse
import json
import os
import sys
import time

_HERE = os.path.dirname(os.path.abspath(__file__))
# PCRECOMP picks the toolkit checkout, the same knob CMakeLists.txt has: the
# lifter and the runtime header must come from the same tree.
_TOOLS = os.path.join(os.environ.get('PCRECOMP', os.path.join(_HERE, '..', 'pcrecomp')), 'tools')
sys.path.insert(0, os.path.join(_TOOLS, 'lift'))
sys.path.insert(0, os.path.join(_TOOLS, 'pe'))

from capstone import Cs, CS_ARCH_X86, CS_MODE_32          # noqa: E402
from generate import (EXTENT_REACH, closure, find_splits, true_extent,  # noqa: E402
                      linear_disassemble_function, lift_function_linear, write_chunk)
from lift32 import Lifter                                  # noqa: E402
from pe_analyze import analyze_pe, build_iat_map           # noqa: E402

EXE = os.path.join(_HERE, 'work', 'swkotor.exe')           # unwrapped by Setup (steamstub.py)
CATALOG = os.path.join(_HERE, 'work', 'functions.json')
SEEDS = os.path.join(_HERE, 'work', 'vtable_seeds.json')
OUT = os.path.join(_HERE, 'src', 'recomp', 'gen')
STATS = os.path.join(_HERE, 'work', 'lift_stats.json')

# Entries the catalog does not find, so a clean checkout lifts them too.
RUN_SEEDS = []


def load_seeds(path):
    """Addresses from a seed file: a list of {'address': va} or of plain VAs."""
    if not path or not os.path.exists(path):
        return set()
    return {e['address'] if isinstance(e, dict) else e for e in json.load(open(path))}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--exe', default=EXE)
    ap.add_argument('--catalog', default=CATALOG)
    ap.add_argument('--out', default=OUT)
    ap.add_argument('--roots', default='', help='comma-separated extra root VAs')
    ap.add_argument('--max', type=int, default=3000, help='closure size cap')
    ap.add_argument('--all', action='store_true', help='lift every function')
    ap.add_argument('--virtual', action='store_true', help='root the closure at every vtable slot too')
    ap.add_argument('--split', type=int, default=400, help='functions per .c file')
    ap.add_argument('--seeds', default=os.path.join(_HERE, 'work', 'run_seeds.json'),
                    help='seed_from_log.py JSON of unresolved targets from runs')
    args = ap.parse_args()
    if not os.path.exists(args.catalog):
        sys.exit('no catalog at %s -- run disasm32.py first (README, Step by step)' % args.catalog)

    info = analyze_pe(args.exe)
    iat = build_iat_map(info)
    cs, ce = info.code_start, info.code_end
    print('[*] base=0x%08X code=0x%08X-0x%08X IAT=%d' % (info.image_base, cs, ce, len(iat)))

    cat = json.load(open(args.catalog))
    byaddr = {f['address']: f for f in cat['functions'] if cs <= f['address'] < ce}
    print('[*] catalog: %d functions inside .text' % len(byaddr))

    # Bound each injected entry by the next known one: a slot landing mid-code
    # handed `ce` makes the extent walk descend the whole of .text.
    vslots = {a for a in load_seeds(SEEDS) if cs <= a < ce}
    want = {a for a in set(RUN_SEEDS) | vslots | load_seeds(args.seeds)
            if cs <= a < ce and a not in byaddr}
    known = sorted(set(byaddr) | want)
    nxt = {a: (known[i + 1] if i + 1 < len(known) else ce) for i, a in enumerate(known)}
    for a in want:
        byaddr[a] = {'address': a, 'end': min(nxt[a], a + 0x100), 'calls_to': [], 'entry_kind': 'start'}
    print('[*] seeded entries not in the catalog: %d' % len(want))

    entry = info.image_base + info.entry_point_rva
    roots = [entry] + [int(x, 0) for x in args.roots.split(',') if x.strip()]
    if args.virtual:
        roots += sorted(vslots)
    chosen = set(byaddr) if args.all else set(closure(byaddr, roots, args.max))
    print('[*] lifting %d of %d functions (%s)'
          % (len(chosen), len(byaddr), 'all' if args.all else 'closure, cap %d' % args.max))

    md = Cs(CS_ARCH_X86, CS_MODE_32)
    md.detail = True
    text = [s for s in info.sections if s.name == '.text'][0]
    code = open(args.exe, 'rb').read()[text.raw_offset:text.raw_offset + text.raw_size]

    # precise carry: adc/sbb take their carry from the flag state that set it.
    # The default reads a `_cf` that add/sub/cmp never write, which breaks every
    # 64-bit add and subtract (The Movies lost its menu video to it).
    lifter = Lifter(iat_map=iat, lifted=set(byaddr), precise_carry=True, precise_sbb=True)
    os.makedirs(args.out, exist_ok=True)
    for fn in os.listdir(args.out):                 # a smaller lift must not leave stale chunks
        if fn.startswith('recomp_') and fn.endswith('.c'):
            os.remove(os.path.join(args.out, fn))
    entries, chunk, idx, errors, dirty = [], [], 0, 0, 0
    ordered = sorted(byaddr)
    starts = {a for a in ordered if byaddr[a].get('entry_kind') != 'alias'}
    t_split = time.time()
    splits = find_splits(md, code, cs, ce, starts)
    starts -= splits          # still dispatchable; just not a wall inside its parent
    print('[*] split entries (the middle of the function before them): %d, %.0fs'
          % (len(splits), time.time() - t_split))
    t0 = time.time()

    def flush(force=False):
        nonlocal chunk, idx
        if chunk and (force or len(chunk) >= args.split):
            write_chunk(args.out, idx, chunk)
            idx += 1
            chunk = []

    def lift_one(addr, name, end, reached):
        nonlocal errors
        try:
            lo = min(reached) if reached else addr   # a chunk can sit below the entry
            insns, leaders = (linear_disassemble_function(md, code, cs, lo, end, reached=reached)
                              if end > addr else ([], None))
            if leaders is not None:
                leaders.add(addr)
            body = (lift_function_linear(lifter, name, insns, leaders, addr) if insns
                    else 'void %s(void) { }\n' % name)
        except Exception as e:                      # noqa: BLE001 -- counted, not hidden
            body = '/* ERROR %s: %s */\nvoid %s(void) { }\n' % (name, e, name)
            errors += 1
        chunk.append((body, addr, name))
        entries.append((addr, name))
        if len(chunk) >= args.split:
            flush()
            print('[*]   %d/%d (%d err)' % (len(entries), len(chosen), errors), flush=True)

    # A direct branch or call to an address nothing catalogued is a tail call
    # or a jump into shared code the catalog missed. The target has to be
    # dispatchable or the RECOMP_ITAIL cannot resolve, so each round's outside
    # targets are lifted in the next round, until none are new.
    todo, added = sorted(chosen), 0
    while todo:
        outside = set()
        for addr in todo:
            name = 'sub_%08X' % addr
            reached, behind, called = set(), set(), set()
            end, clean = true_extent(md, code, cs, addr, min(addr + EXTENT_REACH, ce), starts,
                                     reached=reached, behind=behind, called=called)
            outside |= {t for t in behind if t not in reached} | called
            dirty += not clean
            lift_one(addr, name, end, reached)
        todo = sorted(t for t in outside if cs <= t < ce and t not in byaddr)
        for t in todo:
            byaddr[t] = {'address': t, 'end': ce, 'calls_to': [], 'entry_kind': 'start'}
            chosen.add(t)
        added += len(todo)
    print('[*] branch and call targets outside the catalog, added as entries: %d' % added)

    stubs = [a for a in ordered if a not in chosen]
    for a in stubs:
        name = 'sub_%08X' % a
        chunk.append(('void %s(void) { RECOMP_NOT_LIFTED(0x%08Xu); }\n' % (name, a), a, name))
        entries.append((a, name))
        flush()
    flush(force=True)

    with open(os.path.join(args.out, 'recomp_funcs.h'), 'w', newline='\n') as f:
        f.write('/* swkotor.exe - AUTO-GENERATED by run_lift.py */\n#pragma once\n'
                '#include <stdint.h>\n\n'
                'void recomp_not_lifted(uint32_t va);\n'
                '#define RECOMP_NOT_LIFTED(va) recomp_not_lifted(va)\n\n')
        for a, n in entries:
            f.write('void %s(void);\n' % n)
    with open(os.path.join(args.out, 'recomp_dispatch.c'), 'w', newline='\n') as f:
        f.write('/* swkotor.exe - AUTO-GENERATED by run_lift.py */\n'
                '#include "recomp_types.h"\n#include "recomp_funcs.h"\n\n'
                'const recomp_dispatch_entry_t recomp_dispatch_table[] = {\n')
        for a, n in sorted(entries):
            f.write('    { 0x%08Xu, %s },\n' % (a, n))
        f.write('};\nconst uint32_t recomp_dispatch_count = %d;\n'
                'const uint32_t kotor_entry_va = 0x%08Xu;\n' % (len(entries), entry))

    lines = sum(sum(1 for _ in open(os.path.join(args.out, fn), encoding='utf-8', errors='replace'))
                for fn in os.listdir(args.out))
    stats = {'lifted': len(chosen), 'stubs': len(stubs), 'errors': errors,
             'no_terminator': dirty, 'files': idx, 'lines': lines}
    json.dump(stats, open(STATS, 'w'), indent=1)
    print('=' * 60)
    print('  lifted %d   not-lifted stubs %d   errors %d   no terminator %d'
          % (len(chosen), len(stubs), errors, dirty))
    print('  %s lines of C in %d files, %.1fs' % (format(lines, ','), idx, time.time() - t0))
    print('=' * 60)


if __name__ == '__main__':
    main()
