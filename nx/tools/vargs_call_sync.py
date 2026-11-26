#!/usr/bin/env python3
"""SDNX 16.248: narrows the v0-v13 sync around direct calls (codegen vector_args_as_local).

With vector_args_as_local the code generator keeps v0-v13 in locals inside a function and wraps every direct call as
    { ctx.v0 = v0; ... ctx.v13 = v13; sub_X(ctx, base); v0 = ctx.v0; ... v13 = ctx.v13; }
storing and reloading every localized register. A callee that never touches some of v0-v13 (itself or through the
functions it calls) neither reads them from ctx nor writes them there, so those copies do nothing. This pass finds, for
every generated function, the set of v0-v13 it touches: ctx.vN or a local vN loaded from ctx.vN in its body, plus the
sets of its direct callees, to a fixpoint. An indirect call, a register sync placeholder expansion, or a callee that is
not a generated function counts as touching all fourteen. Each wrapped direct call then keeps only the copies of the
callee's set. Returns and tail calls keep the full sync (the caller's caller may read any of them).

It changes no line count, so the PGO profile's line checksums stay as they were. Run after codegen, after or before
hot_no_volatile.py (independent).  usage: vargs_call_sync.py <generated dir>
"""
import glob
import os
import re
import sys

ALL = frozenset(range(14))
DEF = re.compile(r'^DEFINE_REX_FUNC\((\w+)\)')
CTXV = re.compile(r'\bctx\.v(\d+)\b')
CALL = re.compile(r'\b(\w+)\(ctx, base\);')
WRAP = re.compile(r'\{ ((?:ctx\.v\d+ = v\d+; )+)(\w+)\(ctx, base\);((?: v\d+ = ctx\.v\d+;)+) \}')


def main():
    gen = sys.argv[1]
    files = sorted(glob.glob(os.path.join(gen, '*_recomp.*.cpp')))
    touched = {}
    callees = {}
    texts = {}
    for path in files:
        text = open(path, encoding='utf-8', errors='replace').read()
        texts[path] = text
        cur = None
        for line in text.split('\n'):
            m = DEF.match(line)
            if m:
                cur = m.group(1)
                touched[cur] = set()
                callees[cur] = set()
                continue
            if cur is None:
                continue
            code = line.split('//', 1)[0]
            for n in CTXV.findall(code):
                if int(n) <= 13:
                    touched[cur].add(int(n))
            if 'REX_CALL_INDIRECT_FUNC' in code or 'REX_TAIL_INDIRECT_FUNC' in code:
                touched[cur] |= ALL
            for name in CALL.findall(code):
                callees[cur].add(name)
    # A callee that is not a generated function (import, native hook) touches everything.
    unknown = set()
    for f, cs in callees.items():
        for c in cs:
            if c not in touched:
                unknown.add(c)
    changed = True
    rounds = 0
    while changed:
        changed = False
        rounds += 1
        for f, cs in callees.items():
            s = touched[f]
            before = len(s)
            for c in cs:
                if c in touched:
                    s |= touched[c]
                else:
                    s |= ALL
                if len(s) == 14:
                    break
            if len(s) != before:
                changed = True
    sites = kept = dropped = 0
    files_changed = 0

    def narrow(m):
        nonlocal sites, kept, dropped
        callee = m.group(2)
        regs = touched.get(callee, ALL)
        stores = re.findall(r'ctx\.v(\d+) = v\1; ', m.group(1))
        loads = re.findall(r' v(\d+) = ctx\.v\1;', m.group(3))
        sites += 1
        keep_s = [n for n in stores if int(n) in regs]
        keep_l = [n for n in loads if int(n) in regs]
        kept += len(keep_s) + len(keep_l)
        dropped += len(stores) - len(keep_s) + len(loads) - len(keep_l)
        return ('{ ' + ''.join(f'ctx.v{n} = v{n}; ' for n in keep_s) + f'{callee}(ctx, base);' +
                ''.join(f' v{n} = ctx.v{n};' for n in keep_l) + ' }')

    for path, text in texts.items():
        new = WRAP.sub(narrow, text)
        if new != text:
            open(path, 'w', encoding='utf-8').write(new)
            files_changed += 1
    none = sum(1 for s in touched.values() if not s)
    print(f'vargs_call_sync: {len(touched)} functions ({none} touch none of v0-v13, {len(unknown)} unknown callees), '
          f'{rounds} rounds; {sites} wrapped direct calls: {kept} copies kept, {dropped} dropped; '
          f'{files_changed} files rewritten')


if __name__ == '__main__':
    main()
