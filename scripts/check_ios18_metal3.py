#!/usr/bin/env python3
"""Build and run the synthetic Metal 3 regression tests; no game data or full Xcode required.

Requires an Apple-silicon Mac with recent Command Line Tools. Writes diagnostics
only to --output. This checks the Mac compatibility backend, not iPhone gameplay.
"""
import argparse
import json
import os
from pathlib import Path
import subprocess
import sys

REPO = Path(__file__).resolve().parents[1]
CASES = (
    ('t_nvn', ()), ('t_feedback', ()), ('t_geometry', ()), ('t_vbuf', ()),
    ('t_copy_alias', ()), ('t_present_snap', ('suspend',)),
    ('t_present_snap', ('pacing',)), ('t_present_snap', ('copy',)),
)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, required=True, help='new directory for logs and results.json')
    args = parser.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    tests = REPO / 'dm/gfx/tests'
    binaries = list(dict.fromkeys(name for name, _ in CASES))
    with (out / 'build.log').open('w') as log:
        build = subprocess.run(['make', '-C', str(tests), 'GAME=010015100B514000', '-j3', *binaries],
                               stdout=log, stderr=subprocess.STDOUT)
    if build.returncode:
        print(f'Build failed: {out / "build.log"}', flush=True)
        return 1
    results = []
    for mode in ('0', '1'):
        env = dict(os.environ, DM_METAL_BACKEND='metal3', MTL_DEBUG_LAYER='1', DM_NVN_ENC_THREAD=mode)
        for name, arguments in CASES:
            label = '-'.join((name, *arguments, f'enc{mode}'))
            with (out / f'{label}.log').open('w') as log:
                try:
                    run = subprocess.run([str(tests / name), *arguments], cwd=tests, env=env,
                                         stdout=log, stderr=subprocess.STDOUT, timeout=60)
                    rc = run.returncode
                except subprocess.TimeoutExpired:
                    rc = 'timeout'
            results.append({'test': label, 'exit_code': rc})
            (out / 'results.json').write_text(json.dumps(results, indent=2) + '\n')
            print(f'{label}: {"PASS" if rc == 0 else "FAIL"} ({rc})', flush=True)
    print(f'Diagnostics: {out}', flush=True)
    return int(any(r['exit_code'] != 0 for r in results))


if __name__ == '__main__':
    sys.exit(main())
