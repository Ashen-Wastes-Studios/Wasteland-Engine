#!/usr/bin/env python3
"""Guard Nova post-process location caching after Slang uniform-name changes.

A zero-initialized location is a valid GL location, not a missing-uniform
sentinel. Uploading uncached ColorGamma left the shader at gamma=0 and made
all captured RGB pixels round to black. Run with --self-test to prove the
guard detects a missing lookup without modifying the source file.
"""
from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CPP = ROOT / 'Wasteland/src/Wasteland/Renderer/Renderer3D.cpp'


def check(source: str) -> list[str]:
    source = re.sub(r'/\*.*?\*/|//[^\n]*', '', source, flags=re.S)
    start = source.index('int Loc_BloomEnabled')
    end = source.index('int Loc_FilmGrainEnabled', start)
    end = source.index(';', end)
    locations = re.findall(r'\bLoc_\w+\b', source[start:end])
    errors = []
    for location in locations:
        uniform = 'NovaConstants_0.u_' + location.removeprefix('Loc_') + '_0'
        lookup = (r's_Data\.' + location + r'\s*=\s*glGetUniformLocation\('
                  r'\s*rtProg\s*,\s*"' + re.escape(uniform) + r'"\s*\)')
        if not re.search(lookup, source):
            errors.append(f'{location}: missing lookup for {uniform}')
    # Slang wraps cbuffer arrays in generated structs. Looking up only the
    # aggregate name returns -1 and drops all analytic/volumetric array uploads.
    for table in ['Loc_Volumetric', 'Loc_ALights']:
        if not re.search(r's_Data\.' + table + r'\[progIndex\]\[i\]\s*=\s*'
                         r'\(progIndex\s*==\s*0\)\s*\?\s*resolveNovaLocation\(prog,\s*names\[i\]\)', source):
            errors.append(f'{table}: Slang array members must resolve through active-uniform reflection')
    return errors


def main() -> int:
    source = CPP.read_text(encoding='utf-8')
    errors = check(source)
    if '--self-test' in sys.argv and not errors:
        mutant = re.sub(r's_Data\.Loc_ColorGamma\s*=\s*glGetUniformLocation\([^;]+;', '', source)
        assert any('Loc_ColorGamma:' in e for e in check(mutant)), 'Missing gamma lookup went undetected'
        print('PASS: mutation self-test detects missing ColorGamma lookup')
        mutant = source.replace('? resolveNovaLocation(prog, names[i])', '? glGetUniformLocation(prog, names[i])')
        assert sum('Slang array members' in e for e in check(mutant)) == 2, 'Aggregate array lookups went undetected'
        print('PASS: mutation self-test detects dropped analytic/volumetric array reflection')
    for error in errors:
        print('FAIL:', error)
    if errors:
        return 1
    print('PASS: all Nova post-process uploads have matching cached Slang uniform locations')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
