"""Sleeping Dogs -> Switch (SDNX §16.146): guest loads/stores without `volatile` in chosen hot functions only.

The Switch build defines REX_GUEST_VOLATILE=volatile for the whole recompiled code (sd_build_nro.sh, GUEST_VOLATILE=ON,
§16.42): without it GCC reordered guest accesses and the game corrupted a memory pool. That costs a lot (§16.42: Wwise
27 -> 51 %, main thread 21 -> 33 % CPU). The functions listed in nx/sdretail/hot_nonvolatile.txt (the hottest ones
of the CPU profile that have no loop which could be a spin-wait: a loop re-reading guest memory through an unchanged
base register with no call, store, atomic or barrier in it) are wrapped in the generated .cpp files with

    #pragma push_macro("REX_GUEST_VOLATILE") / #undef / #define REX_GUEST_VOLATILE (empty) ... #pragma pop_macro

so their REX_LOAD_* / REX_STORE_* macros expand without volatile. Everything else keeps it. Idempotent; run after
every codegen (sd_codegen.sh) and before the NRO build.

  usage: python3 hot_no_volatile.py <generated dir> <list file> [--remove]
"""
import glob
import os
import re
import sys

MARK = '// SDNX hot_no_volatile'
PUSH = ['#pragma push_macro("REX_GUEST_VOLATILE")  ' + MARK, '#undef REX_GUEST_VOLATILE', '#define REX_GUEST_VOLATILE']
POP = '#pragma pop_macro("REX_GUEST_VOLATILE")  ' + MARK


def main():
    gen, list_path = sys.argv[1], sys.argv[2]
    remove = '--remove' in sys.argv[3:]
    names = set()
    for line in open(list_path, encoding='utf-8'):
        line = line.split('#', 1)[0].strip()
        if line:
            names.add(re.sub(r'^__imp__', '', line))
    wrapped = set()
    files_changed = 0
    for path in sorted(glob.glob(os.path.join(gen, '*_recomp.*.cpp'))):
        lines = open(path, encoding='utf-8', errors='replace').read().split('\n')
        # Drop any earlier wrapping first, so the list can shrink.
        clean = []
        skip = 0
        for line in lines:
            if skip:
                skip -= 1
                continue
            if line.startswith('#pragma push_macro("REX_GUEST_VOLATILE")') and MARK in line:
                skip = 2
                continue
            if line.startswith('#pragma pop_macro("REX_GUEST_VOLATILE")') and MARK in line:
                continue
            clean.append(line)
        out = []
        i = 0
        while i < len(clean):
            line = clean[i]
            m = re.match(r'^DEFINE_REX_FUNC\((\w+)\)', line)
            if m and m.group(1) in names and not remove:
                out.extend(PUSH)
                # copy up to and including the function's closing brace (a '}' at column 0)
                out.append(line)
                i += 1
                while i < len(clean) and not clean[i].startswith('}'):
                    out.append(clean[i])
                    i += 1
                if i < len(clean):
                    out.append(clean[i])
                    i += 1
                out.append(POP)
                wrapped.add(m.group(1))
                continue
            out.append(line)
            i += 1
        if out != lines:
            open(path, 'w', encoding='utf-8').write('\n'.join(out))
            files_changed += 1
    missing = sorted(names - wrapped) if not remove else []
    print('hot_no_volatile: %d of %d functions without volatile, %d files rewritten%s' %
          (len(wrapped), len(names), files_changed, ('; not found: ' + ' '.join(missing[:10])) if missing else ''))
    return 0


if __name__ == '__main__':
    sys.exit(main())
