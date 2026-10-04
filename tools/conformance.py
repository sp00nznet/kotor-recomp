#!/usr/bin/env python3
"""KotOR conformance harness (REPO_RULES section 9).

Two fixed corpora, one pass/fail count each, compared against the committed
baseline in conformance.json; a regression fails the run:

* **Boot milestones**: a headless, muted run of build/kotor.exe, scored by the
  lines the host prints at each stage. The original game is the ground truth:
  each milestone is something it does on every start.
* **Lift health**: from the generated tree. Lift errors, bodies with no
  terminator, and RECOMP_ITAIL labels that cannot resolve at run time (their
  target is not in the dispatch table).

The game is not in the repo. Without game/ and build/kotor.exe this skips with
a message and exits 0, so it can sit in CI without the corpus.

    py -3 tools/conformance.py              # run, compare, print the table
    py -3 tools/conformance.py --update     # ...and accept the result as the baseline
"""
import argparse
import glob
import json
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HOST = os.path.join(ROOT, 'build', 'kotor.exe')
GEN = os.path.join(ROOT, 'src', 'recomp', 'gen')
BASELINE = os.path.join(ROOT, 'conformance.json')

# (name, what the host prints when it is reached). Order is boot order.
MILESTONES = [
    ('image mapped and imports bound', r'guest exe '),
    ('audio muted', r'audio: muted'),
    ('entry point entered', r'entering 0x006FB38D'),
    ('window created', r'\[host\] CreateWindowExA\('),
    ('first frame presented', r'\[host\] frame 1 presented'),
    ('60 frames presented', r'\[host\] frame 60 presented'),
]


def boot(host, seconds):
    try:
        p = subprocess.run([host, '--headless', '--run', '--watchdog', str(seconds)],
                           cwd=ROOT, capture_output=True, text=True, errors='replace',
                           timeout=seconds + 60)
        out, code = p.stdout + p.stderr, p.returncode
    except subprocess.TimeoutExpired as e:
        out, code = (e.stdout or '') + (e.stderr or ''), 'timeout'
        out = out if isinstance(out, str) else out.decode(errors='replace')
    passed = [name for name, pat in MILESTONES if re.search(pat, out)]
    last = [l for l in out.splitlines() if l.startswith(('===', '[not-lifted]', '[watchdog]', 'cannot'))]
    return passed, code, last[:2]


def lift_health():
    stats = json.load(open(os.path.join(ROOT, 'work', 'lift_stats.json')))
    disp = set(re.findall(r'\{ 0x([0-9A-F]{8})u,', open(os.path.join(GEN, 'recomp_dispatch.c')).read()))
    unresolved = sum(1 for fn in glob.glob(os.path.join(GEN, 'recomp_0*.c'))
                     for t in re.findall(r'L_([0-9A-F]{8}): RECOMP_ITAIL', open(fn).read())
                     if t not in disp)
    return {'lifted': stats['lifted'], 'errors': stats['errors'],
            'no_terminator': stats['no_terminator'], 'unresolved_itail': unresolved}


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--update', action='store_true', help='accept this run as the baseline')
    ap.add_argument('--seconds', type=int, default=60, help='headless run length')
    ap.add_argument('--host', default=HOST, help='the host to run (default build/kotor.exe)')
    args = ap.parse_args()
    if not (os.path.exists(args.host) and os.path.isdir(os.path.join(ROOT, 'game'))):
        print('conformance: skipped -- needs game/ (your copy) and build/kotor.exe '
              '(README, Building from source)')
        return 0

    passed, code, last = boot(os.path.abspath(args.host), args.seconds)
    health = lift_health()
    now = {'milestones': len(passed), 'of': len(MILESTONES), **health}
    base = json.load(open(BASELINE)) if os.path.exists(BASELINE) else None

    print('boot milestones: %d/%d  (exit %s)' % (len(passed), len(MILESTONES), code))
    for name, _ in MILESTONES:
        print('  [%s] %s' % ('x' if name in passed else ' ', name))
    for l in last:
        print('  stopped: ' + l)
    print('lift: %(lifted)d functions, %(errors)d errors, %(no_terminator)d with no '
          'terminator, %(unresolved_itail)d unresolvable ITAIL labels' % health)

    worse = []
    if base:
        if now['milestones'] < base['milestones']:
            worse.append('milestones %d -> %d' % (base['milestones'], now['milestones']))
        for k in ('errors', 'no_terminator', 'unresolved_itail'):
            if now[k] > base[k]:
                worse.append('%s %d -> %d' % (k, base[k], now[k]))
    if args.update or not base:
        json.dump(now, open(BASELINE, 'w'), indent=1)
        print('baseline written to conformance.json')
    if worse:
        print('REGRESSION: ' + '; '.join(worse))
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
