#!/usr/bin/env python3
"""KotOR conformance harness (REPO_RULES section 9).

Two fixed corpora, one pass/fail count each, compared against the committed
baseline in conformance.json; a regression fails the run:

* **Boot milestones**: a headless, muted run of build/kotor.exe that starts a
  new game by scripted clicks, scored by the lines the host prints at each stage. The original game is the ground truth:
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
    ('intro movies opened (LucasArts, BioWare, legal)', r'movie .*legal\.bik -> open'),
    ('first frame presented', r'\[host\] frame 1 presented'),
    # The main menu starts its theme as a Miles stream; nothing else streams before it.
    ('main menu music started', r'\[host\] stream .* -> open'),
    ('1000 frames presented', r'\[host\] frame 1000 presented'),
    # NEW_GAME below: New Game, Scoundrel, Quick Character, portrait, random name, Play.
    ('Endar Spire module loaded', r'(?i)\[host\] module .*modules[\\/]end_m01aa\.rim -> open'),
    # The opening conversation (Trask) loads its lip sync.
    ('opening conversation started', r'(?i)\[host\] module .*lips[\\/]end_m01aa_loc\.mod -> open'),
    ('in game: frames presented after the module loaded', None),
]

# Clicks on the 800x600 menus, in seconds after the main menu appears
# (src/runtime/host.c, --click). The screen each one lands on is in
# docs/testing.md.
NEW_GAME = ['488,293@3', '180,280@12', '180,280@18', '536,186@25', '555,222@32',
            '264,485@39', '555,252@46', '272,485@53', '555,283@60']


def in_game(out):
    k = out.lower().find('end_m01aa.rim')
    return k >= 0 and re.search(r'\[host\] frame \d+ presented', out[k:]) is not None


def boot(host, seconds):
    try:
        clicks = [a for c in NEW_GAME for a in ('--click', c)]
        p = subprocess.run([host, '--headless', '--run', '--watchdog', str(seconds)] + clicks,
                           cwd=ROOT, capture_output=True, text=True, errors='replace',
                           timeout=seconds + 60)
        out, code = p.stdout + p.stderr, p.returncode
    except subprocess.TimeoutExpired as e:
        out, code = (e.stdout or '') + (e.stderr or ''), 'timeout'
        out = out if isinstance(out, str) else out.decode(errors='replace')
    open(os.path.join(ROOT, 'work', 'conformance.log'), 'w', encoding='utf-8').write(out)
    passed = [name for name, pat in MILESTONES if (re.search(pat, out) if pat else in_game(out))]
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
    ap.add_argument('--seconds', type=int, default=240,
                    help='headless run length (the Endar Spire loads about 2 minutes in)')
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
