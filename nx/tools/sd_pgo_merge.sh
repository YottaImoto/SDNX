#!/usr/bin/env bash
# SDNX 16.188: PGO step 2 of 3 (the workflow renut_nx_ultimate_base documents; our own script for the WSL build).
#
#   bash /mnt/d/CODE/SDNX/nx/tools/sd_pgo_merge.sh <folder with sdnx_pgo_*.stream> [--all]
#
# Step 1: the instrumented NRO (SDNX_PGO=generate bash nx/tools/sd_build_nro.sh -> nx/sdretail_pgogenerate.nro),
# played on the console: it writes pgo/sdnx_pgo_<N>.stream next to the NRO. Copy that pgo folder to the PC.
# This adds the session's counts into ~/masseffect-nx/pgo/*.gcda (gcov-tool merge-stream, in the devkitA64
# container: the streams name that folder) and fills in their summary (sd_pgo_summary.py: the stream API writes
# it as zero). Step 3: SDNX_PGO=use bash nx/tools/sd_build_nro.sh.
# merge-stream ADDS to existing .gcda files, so this stops when the folder already has some (move them aside).
# One training session = one stream; several only with --all (sessions of the SAME instrumented build).
set -euo pipefail
ME="$HOME/masseffect-nx"
OUT="$ME/pgo"
SRC="${1:?folder with sdnx_pgo_*.stream}"
ALL="${2:-}"
mapfile -t STREAMS < <(ls "$SRC"/sdnx_pgo_*.stream 2>/dev/null | sort)
(( ${#STREAMS[@]} )) || { echo "error: no sdnx_pgo_*.stream in $SRC" >&2; exit 1; }
for s in "${STREAMS[@]}"; do echo "  $(basename "$s") $(date -r "$s" '+%Y-%m-%d %H:%M') $(du -h "$s" | cut -f1)"; done
if (( ${#STREAMS[@]} > 1 )) && [[ "$ALL" != --all ]]; then
  echo "error: ${#STREAMS[@]} streams: keep only this session's, or pass --all for sessions of the same build" >&2; exit 1
fi
mkdir -p "$OUT"
if find "$OUT" -name '*.gcda' | grep -q .; then echo "error: $OUT already has .gcda files: move them aside" >&2; exit 1; fi
STAGE="$ME/pgo_streams"; rm -rf "$STAGE"; mkdir -p "$STAGE"
for s in "${STREAMS[@]}"; do cp "$s" "$STAGE/"; done
docker run --rm -v "$ME:$ME" -w "$ME" devkitpro/devkita64:latest bash -euo pipefail -c '
  for s in '"$STAGE"'/*.stream; do
    echo "merging $(basename "$s")"
    /opt/devkitpro/devkitA64/bin/aarch64-none-elf-gcov-tool merge-stream "$s"
  done
  chown -R '"$(id -u):$(id -g)"' '"$OUT"'
'
N=$(find "$OUT" -name '*.gcda' | wc -l)
(( N )) || { echo "error: no .gcda in $OUT after merging (the streams name another folder?)" >&2; exit 1; }
python3 /mnt/d/CODE/SDNX/nx/tools/sd_pgo_summary.py "$OUT" "${#STREAMS[@]}"
echo "${#STREAMS[@]} session(s) merged into $N file(s) in $OUT"
