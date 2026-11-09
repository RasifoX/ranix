#!/bin/sh
# SPDX-FileCopyrightText: 2026 RasifoX
# SPDX-License-Identifier: EUPL-1.2
# Pre-merge gate (kod-kalitesi-denetimi.md §3.6, §5). Run from the source root:
#   tools/kapi.sh            check the tree against tools/kapi-taban.txt
#   tools/kapi.sh --taban    write the current exceptions as the new baseline
# Fails (exit 1) on anything new that is not in the baseline:
#   extern  an extern declaration in a kernel .c file (put it in a header)
#   fn      a function over 120 lines, or a listed one that grew
#   mode    a device node registered world-writable (mode & 0002)
#   ad      "agent", "ajan" or "AI" in kernel code (principle 1: neutral names)
#   test    a userspace/test_*.c that no script under tests/ runs
# The baseline only shrinks: when you fix an exception, run --taban and commit it.
cd "$(dirname "$0")/.." || exit 2
exec python3 -I - "$@" <<'PY'
import os, re, sys
TABAN = 'tools/kapi-taban.txt'
KOD = ('kernel', 'drivers', 'arch', 'include')
DIS = ('acpica', 'linuxkpi', 'doomgeneric')    # vendored code keeps its own style
MAXF = 120

def files(exts, roots=KOD):
    for r in roots:
        for dp, dn, fn in os.walk(r):
            dn.sort()
            for f in sorted(fn):
                p = os.path.join(dp, f)
                if f.endswith(exts) and not any(d in p for d in DIS): yield p

def code_lines(p):
    """(line number, text) with comments blanked, strings kept."""
    out, inc = [], False
    for i, l in enumerate(open(p, errors='replace'), 1):
        s, j, res = l.rstrip('\n'), 0, ''
        while j < len(s):
            if inc:
                k = s.find('*/', j)
                if k < 0: j = len(s); break
                inc, j = False, k + 2
            elif s.startswith('/*', j): inc, j = True, j + 2
            elif s.startswith('//', j): break
            elif s[j] == '"':
                k = j + 1
                while k < len(s) and s[k] != '"': k += 2 if s[k] == '\\' else 1
                res += s[j:k + 1]; j = k + 1
            else: res += s[j]; j += 1
        out.append((i, res))
    return out

found = set()
# extern in .c
for p in files(('.c',)):
    if p.startswith('include'): continue
    for i, l in code_lines(p):
        if re.search(r'\bextern\b', l): found.add(('extern', p, ' '.join(l.split())))
# long functions: a body opens with "{" at column 0 (or a column-0 signature ending in "{")
for p in files(('.c',)):
    if p.startswith('include'): continue
    L = open(p, errors='replace').read().split('\n'); name, start, sig = None, None, ''
    for i, l in enumerate(L):
        if start is None:
            if re.match(r'^[A-Za-z_]', l) and not re.match(r'^(typedef|struct\s+\w+\s*\{|enum|union)', l): sig = l
            elif not l.startswith(('\t', ' ', '{')) and l.strip() and not l.startswith('#'): sig = ''
            elif l.startswith((' ', '\t')) and sig: sig += ' ' + l.strip()
            opens = l.startswith('{') or (re.match(r'^[A-Za-z_]', l) and l.rstrip().endswith('{') and '(' in l and '=' not in l)
            m = re.search(r'(\w+)\s*\(', sig) if opens else None
            if m: name, start = m.group(1), i
        elif l.startswith('}'):
            n = i - start + 1
            if n > MAXF: found.add(('fn', p, name, n))
            name, start, sig = None, None, ''
# world-writable device nodes
for p in files(('.c',)):
    txt = '\n'.join(l for _, l in code_lines(p))
    for m in re.finditer(r'devfs_register_device\w*\s*\(([^;]*?)\)\s*;', txt, re.S):
        call = ' '.join(m.group(1).split())
        for mode in re.findall(r'\b0[0-7]{3,4}\b', call):
            if int(mode, 8) & 0o002: found.add(('mode', p, call.split(',')[0].strip()))
# neutral names (principle 1): identifiers, strings and comments alike
for p in files(('.c', '.h', '.S')):
    n = sum(len(re.findall(r'agent|ajan', l, re.I)) + len(re.findall(r'\bAI\b', l))
            for l in open(p, errors='replace'))
    if n: found.add(('ad', p, n))
# every userspace test is run by some script under tests/
run = ''.join(open(p, errors='replace').read() for p in files(('.sh', '.py', ''), ('tests',)) if os.path.isfile(p))
for p in sorted(os.listdir('userspace')):
    m = re.match(r'(test_\w+)\.c$', p)
    if m and not re.search(r'\b' + m.group(1) + r'\b', run): found.add(('test', m.group(1)))

def fmt(e): return '\t'.join(str(x) for x in e)
if sys.argv[1:] == ['--taban']:
    with open(TABAN, 'w') as f:
        f.write('# tools/kapi.sh baseline: existing exceptions, to be fixed and removed, never added to.\n')
        for e in sorted(found, key=fmt): f.write(fmt(e) + '\n')
    print(f'kapi: baseline written, {len(found)} entries'); sys.exit(0)
base = {}
for l in open(TABAN):
    if l.startswith('#') or not l.strip(): continue
    e = l.rstrip('\n').split('\t'); base[tuple(e[:-1]) if e[0] in ('fn', 'ad') else tuple(e)] = e[-1]
bad, note = [], []
for e in sorted(found, key=fmt):
    if e[0] in ('fn', 'ad'):
        k = tuple(str(x) for x in e[:-1]); was = base.get(k)
        if was is None: bad.append(fmt(e) + ('   (new function over %d lines)' % MAXF if e[0] == 'fn' else '   (new)'))
        elif int(e[-1]) > int(was): bad.append(fmt(e) + f'   (grew from {was})')
        elif int(e[-1]) < int(was): note.append(fmt(e) + f'   (was {was}: run --taban)')
    elif tuple(str(x) for x in e) not in base: bad.append(fmt(e))
keys = {tuple(str(x) for x in (e[:-1] if e[0] in ('fn', 'ad') else e)) for e in found}
note += ['\t'.join(k) + '   (fixed: run --taban)' for k in base if k not in keys]
for n in note: print('kapi: note  ', n)
for b in bad: print('kapi: FAIL  ', b)
print(f'kapi: {len(found)} known exceptions, {len(bad)} new' + (' -- needs a fix or a human decision' if bad else ', ok'))
sys.exit(1 if bad else 0)
PY
