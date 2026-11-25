"""SDNX 16.243: pixel shader variants without the texture sign handling, for the whole deko3d package.

Every texture fetch of a translated shader is followed by masseffectApplyTextureSigns (shader_common.h): a loop over
the four channels that reads the channel's TextureSign from the high byte of the fetch's descriptor word in the shared
block (0 unsigned, 2 biased, 3 piecewise-linear gamma) and converts it. With all four signs 0 (the usual case) the loop
changes nothing but still costs its shifts, compares and branches on every channel of every fetch. The FXAA shader
specialised this way (16.179, build_fxaa_variant.py) went from 2.4 ms to ~0.4 ms.

For each pixel shader of the installed package (and its _nat variant) this script:
  * checks that its GLSL (the dk_build.py work folder) compiled by the same uam reproduces the installed .dksh byte for
    byte - otherwise the shader is left alone;
  * replaces every read of a sign byte, "floatBitsToUint(g_UboShared.v[N].c) >> 24u", by 0u and compiles the result.
The renderer binds the variant only while the high byte of every replaced word (shared word N*4+c) is 0 at the draw,
so the variant computes exactly what the original would. Reads of those words in any other form stay as they are.

Output: <out>/<name>.dksh and <out>/index.txt, one line per variant:
  <name> <installed size> <installed FNV-1a 64 hex> <word,word,...>
usage: python3 build_zero_sign_variants.py <uam> <glsl dir> <installed package dir> <out dir> [jobs]
"""
import os
import re
import shutil
import subprocess
import sys
import tempfile
from concurrent.futures import ThreadPoolExecutor

SIGN_READ = re.compile(r'floatBitsToUint\(g_UboShared\.v\[(\d+)\]\.([xyzw])\) >> 24u')


def fnv1a64(data):
    h = 0xcbf29ce484222325
    for b in data:
        h ^= b
        h = (h * 0x100000001b3) & 0xFFFFFFFFFFFFFFFF
    return h


def compile_glsl(uam, path, out):
    r = subprocess.run([uam, '-s', 'frag', '-o', out, path], capture_output=True, text=True)
    return r.returncode == 0 and os.path.exists(out)


def main():
    uam, glsl_dir, package, out = sys.argv[1:5]
    jobs = int(sys.argv[5]) if len(sys.argv) > 5 else os.cpu_count()
    names = sorted(f[:-5] for f in os.listdir(package) if f.startswith('ps_') and f.endswith('.dksh'))
    shutil.rmtree(out, ignore_errors=True)
    os.makedirs(out)
    work = tempfile.mkdtemp(prefix='zs_')

    def job(name):
        src = os.path.join(glsl_dir, name + '.glsl')
        if not os.path.exists(src):
            return name, 'no-glsl', None
        text = open(src).read()
        reads = SIGN_READ.findall(text)
        if not reads:
            return name, 'no-signs', None
        installed = open(os.path.join(package, name + '.dksh'), 'rb').read()
        base = os.path.join(work, name + '.base.dksh')
        if not compile_glsl(uam, src, base):
            return name, 'uam-base', None
        if open(base, 'rb').read() != installed:
            return name, 'not-reproduced', None
        words = sorted({int(n) * 4 + 'xyzw'.index(c) for n, c in reads})
        variant_src = os.path.join(work, name + '.zs.glsl')
        open(variant_src, 'w').write(SIGN_READ.sub('0u', text))
        variant = os.path.join(work, name + '.zs.dksh')
        if not compile_glsl(uam, variant_src, variant):
            return name, 'uam-variant', None
        code = open(variant, 'rb').read()
        shutil.move(variant, os.path.join(out, name + '.dksh'))
        line = '%s %d %016x %s' % (name, len(installed), fnv1a64(installed), ','.join(map(str, words)))
        return name, 'ok', (line, len(installed), len(code), len(reads))

    with ThreadPoolExecutor(jobs) as ex:
        results = list(ex.map(job, names))
    shutil.rmtree(work, ignore_errors=True)
    lines, stats = [], {}
    before = after = reads_total = 0
    for name, status, info in results:
        stats[status] = stats.get(status, 0) + 1
        if status == 'ok':
            lines.append(info[0])
            before += info[1]
            after += info[2]
            reads_total += info[3]
    open(os.path.join(out, 'index.txt'), 'w').write('\n'.join(lines) + '\n')
    print('zero-sign variants: %d of %d pixel shaders (%s)' % (len(lines), len(names),
          ', '.join('%s %d' % kv for kv in sorted(stats.items()))))
    print('sign reads removed: %d; .dksh bytes %d -> %d' % (reads_total, before, after))


if __name__ == '__main__':
    main()
