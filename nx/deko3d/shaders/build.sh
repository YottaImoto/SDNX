#!/usr/bin/env bash
# SDNX §16.92: compile the depth/stencil extraction shaders with uam and embed them as a header in the backend.
#   bash /mnt/d/CODE/SDNX/nx/deko3d/shaders/build.sh   (WSL)
set -euo pipefail
HERE=/mnt/d/CODE/SDNX/nx/deko3d/shaders
# §16.104: the patched uam (SwitchWakerHD: deterministic scheduling, no dual issue). Stock uam 1.1.0 reads
# uninitialised memory while scheduling and gives different binaries run to run.
UAM="${UAM:-$HOME/tools/uam-reference-build/uam}"
OUT=/mnt/d/CODE/me-sdk/src/graphics/sdnx/deko3d_depth_shaders.h
WORK=$(mktemp -d)
"$UAM" -s vert -o "$WORK/vsh.dksh" "$HERE/dz_fullscreen_vsh.glsl"
"$UAM" -s frag -o "$WORK/depth.dksh" "$HERE/dz_depth_fsh.glsl"
"$UAM" -s frag -o "$WORK/stencil.dksh" "$HERE/dz_stencil_fsh.glsl"
"$UAM" -s vert -o "$WORK/ssbo_vsh.dksh" "$HERE/dz_ssbo_vsh.glsl"
"$UAM" -s frag -o "$WORK/ssbo_pass.dksh" "$HERE/dz_ssbo_pass_fsh.glsl"
"$UAM" -s frag -o "$WORK/ssbo_read.dksh" "$HERE/dz_ssbo_read_fsh.glsl"
"$UAM" -s frag -o "$WORK/ssbo_descriptor.dksh" "$HERE/dz_ssbo_descriptor_fsh.glsl"
"$UAM" -s frag -o "$WORK/restore_depth.dksh" "$HERE/dz_restore_depth_fsh.glsl"
"$UAM" -s frag -o "$WORK/restore_stencil.dksh" "$HERE/dz_restore_stencil_fsh.glsl"
"$UAM" -s frag -o "$WORK/raw_depth.dksh" "$HERE/dz_raw_depth_fsh.glsl"
"$UAM" -s frag -o "$WORK/raw_stencil.dksh" "$HERE/dz_raw_stencil_fsh.glsl"
"$UAM" -s frag -o "$WORK/raw_both.dksh" "$HERE/dz_raw_both_fsh.glsl"
"$UAM" -s frag -o "$WORK/cas.dksh" "$HERE/dz_cas_fsh.glsl"  # SDNX 16.148
python3 - "$WORK" "$OUT" <<'PY'
import sys, os, struct
work, out = sys.argv[1], sys.argv[2]
# Read the driver's actual descriptor without dereferencing the suspect address.
# UBO 15 compiles to c[17]; GLSL cannot name reserved c[0]. Patch only the single
# verified LDC instruction, preserving its scheduling and all DKSH metadata.
p = os.path.join(work, "ssbo_descriptor.dksh")
data = bytearray(open(p, "rb").read())
control_size, code_size = struct.unpack_from("<II", data, 8)
patched = 0
for at in range(control_size, control_size + code_size, 8):
    word, = struct.unpack_from("<Q", data, at)
    # §16.101: constant component picks compile to MOV Rd, c[17][0x760..0x76c] (opcode 0x4c98, bank in
    # bits 34..38, offset/4 in bits 20..33). LDC (0xef9) is refused: a dynamic index reads only component 0.
    assert (word >> 52) != 0xef9, "descriptor shader must not use LDC"
    if (word >> 48) == 0x4c98 and ((word >> 34) & 31) == 17:
        offset = ((word >> 20) & 0x3fff) * 4
        assert 0x760 <= offset < 0x770, "unexpected descriptor offset %#x" % offset
        struct.pack_into("<Q", data, at, word & ~(31 << 34))
        patched += 1
        print("patched MOV at %#x offset %#x" % (at, offset))
assert patched == 4, "expected four descriptor MOVs, got %d" % patched
open(p, "wb").write(data)

# §16.103: two variants of the verified read shader, patched in place, to split "LDC.64 does not deliver the high
# word" from "LDG drops the high word". The read shader is exactly: ctrl @0x80, MOV32I R3 @0x88, LDC.64 R0:R1 =
# c[0][0x760] @0x90, LDG.E R0 = [R0:R1+0x100] @0x98 (offsets in the code section).
# Control word: per instruction 21 bits = stall[0:3] yield[4] write barrier[5:7] read barrier[8:10] wait[11:16].
def sched_set(ctrl, slot, stall, keep_wait=True):
    s = (ctrl >> (21 * slot)) & 0x1FFFFF
    wait = (s >> 11) & 0x3F if keep_wait else 0
    s = (stall & 15) | (7 << 5) | (7 << 8) | (wait << 11)
    return (ctrl & ~(0x1FFFFF << (21 * slot))) | (s << (21 * slot))
src = bytearray(open(os.path.join(work, "ssbo_read.dksh"), "rb").read())
cs, = struct.unpack_from("<I", src, 8)
ctrl_at, mov32_at, ldc_at, ldg_at = cs + 0x80, cs + 0x88, cs + 0x90, cs + 0x98
words = {a: struct.unpack_from("<Q", src, a)[0] for a in (ctrl_at, mov32_at, ldc_at, ldg_at)}
assert words[mov32_at] >> 52 == 0x010, "read shader layout: MOV32I %016x" % words[mov32_at]
assert words[ldc_at] == 0xef9500007607ff00, "read shader layout: LDC.64 %016x" % words[ldc_at]
assert words[ldg_at] >> 48 == 0xeed4 and (words[ldg_at] >> 45) & 1, "read shader layout: LDG.E %016x" % words[ldg_at]
for slot in range(3):
    s = (words[ctrl_at] >> (21 * slot)) & 0x1FFFFF
    print("read shader sched slot %d: stall %d wbar %d rbar %d wait %#x" % (slot, s & 15, (s >> 5) & 7, (s >> 8) & 7, (s >> 11) & 63))
# Variant A (no memory access): LDG replaced by I2F.F32.U32 R0 = R1, so the pixel is float(high word LDC.64 gave).
a = bytearray(src)
struct.pack_into("<Q", a, ldg_at, 0x5cb8000000170a00)
struct.pack_into("<Q", a, ctrl_at, sched_set(words[ctrl_at], 2, 15))
open(os.path.join(work, "ssbo_ldchi.dksh"), "wb").write(a)
# Variant B: LDC.64 replaced by MOV R0 = c[0][0x760], MOV R1 = c[0][0x764] (the constant-operand path the descriptor
# readback proved correct); the LDG is unchanged (its wait on barrier 0 is satisfied at once: nothing sets it now).
b = bytearray(src)
struct.pack_into("<Q", b, mov32_at, 0x4c98078000070000 | ((0x760 >> 2) << 20))
struct.pack_into("<Q", b, ldc_at, 0x4c98078000070001 | ((0x764 >> 2) << 20))
ctrl = sched_set(words[ctrl_at], 0, 2, keep_wait=False)
ctrl = sched_set(ctrl, 1, 15, keep_wait=False)
struct.pack_into("<Q", b, ctrl_at, ctrl)
open(os.path.join(work, "ssbo_movread.dksh"), "wb").write(b)
print("variants: ldchi %016x, movread %016x %016x" % (0x5cb8000000170a00, 0x4c98078000070000 | (0x1d8 << 20), 0x4c98078000070001 | (0x1d9 << 20)))
parts = ["// Generated by nx/deko3d/shaders/build.sh (SDNX §16.92). Do not edit.", "#pragma once", "#include <cstdint>", ""]
for name, f in (("kDzFullscreenVsh", "vsh.dksh"), ("kDzDepthFsh", "depth.dksh"), ("kDzStencilFsh", "stencil.dksh"), ("kDzSsboVsh", "ssbo_vsh.dksh"), ("kDzSsboPassFsh", "ssbo_pass.dksh"), ("kDzSsboReadFsh", "ssbo_read.dksh"), ("kDzSsboDescriptorFsh", "ssbo_descriptor.dksh"), ("kDzSsboLdcHiFsh", "ssbo_ldchi.dksh"), ("kDzSsboMovReadFsh", "ssbo_movread.dksh"), ("kDzRestoreDepthFsh", "restore_depth.dksh"), ("kDzRestoreStencilFsh", "restore_stencil.dksh"), ("kDzRawDepthFsh", "raw_depth.dksh"), ("kDzRawStencilFsh", "raw_stencil.dksh"), ("kDzRawBothFsh", "raw_both.dksh"), ("kDzCasFsh", "cas.dksh")):
    data = open(os.path.join(work, f), "rb").read()
    body = ",".join(str(b) for b in data)
    parts.append("alignas(256) inline constexpr uint8_t %s[%d] = {%s};" % (name, len(data), body))
open(out, "w").write("\n".join(parts) + "\n")
print("wrote", out)
PY
ls -la "$WORK"
