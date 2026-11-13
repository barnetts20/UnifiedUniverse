#!/usr/bin/env python3
"""D-235 (UE-66's first build: 'use of undeclared identifier UNI_NEBULA_MIP'): every macro, static
const, UNI_FN function and struct a shader reaches is declared before its first use, in the order the
shader compiler reads the files (each .usf with its includes expanded once, comments stripped;
preprocessor conditions are not uses). The C++ harness cannot see this: UniHlslShim.h defines its
knob macros before including the cores, so a default defined late compiles there and fails in UE.
Prints one line per problem and 'N shaders, M problems'."""
import os
import re
import sys

root = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
core = os.path.join(root, 'Shaders/Private/Core')
priv = os.path.join(root, 'Shaders/Private')
def strip(t):
    t = re.sub(r'/\*.*?\*/', lambda m: '\n' * m.group(0).count('\n'), t, flags=re.S)
    return re.sub(r'//[^\n]*', '', t)
def expand(path, seen, out):
    if path in seen: return
    seen.add(path)
    for line in strip(open(path, encoding='utf-8', errors='replace').read()).split('\n'):
        m = re.match(r'\s*#include\s+"([^"]+)"', line)
        if m:
            name = m.group(1)
            cands = [os.path.join(os.path.dirname(path), name), os.path.join(core, os.path.basename(name)), os.path.join(priv, name.replace('/Plugin/UnifiedUniverse/Private/', ''))]
            for c in cands:
                if os.path.isfile(c): expand(c, seen, out); break
            continue
        out.append((os.path.relpath(path, root), line))
problems = set()
for usf in sorted(os.listdir(priv)):
    if not usf.endswith('.usf'): continue
    lines = []
    expand(os.path.join(priv, usf), set(), lines)
    # Preprocessor conditions (#ifndef X before #define X, include guards, pass switches) are not uses.
    text = '\n'.join('' if re.match(r'\s*#\s*(ifndef|ifdef|if|elif|undef)\b', l) else l for _, l in lines)
    # declared names with their first position
    decl = {}
    for m in re.finditer(r'#define\s+([A-Za-z_]\w*)', text): decl.setdefault(m.group(1), m.start())
    for m in re.finditer(r'static\s+const\s+\w+\s+([A-Za-z_]\w*)', text): decl.setdefault(m.group(1), m.start())
    for m in re.finditer(r'UNI_FN\s+[\w<>]+\s+([A-Za-z_]\w*)\s*\(', text): decl.setdefault(m.group(1), m.start())
    for m in re.finditer(r'\bstruct\s+([A-Za-z_]\w*)', text): decl.setdefault(m.group(1), m.start())
    # uses
    for name, pos in decl.items():
        m = re.search(r'\b' + re.escape(name) + r'\b', text)
        if m and m.start() < pos:
            # find the line
            ln = text.count('\n', 0, m.start())
            problems.add((usf, name, lines[ln][0]))
for p in sorted(problems): print('%s: %s used before its declaration (in %s)' % p)
print('%d shaders, %d problems' % (len([f for f in os.listdir(priv) if f.endswith('.usf')]), len(problems)))
sys.exit(1 if problems else 0)
