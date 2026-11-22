#!/usr/bin/env bash
# Sleeping Dogs -> Switch: copy the native renderer (sdnx backend) from the desktop SDK fork (D:\CODE\me-sdk, where it
# is developed and tested on the bench) into the SDK the NRO builds against (~/masseffect-nx/sdk). Run inside WSL,
# then nx/tools/sd_build_nro.sh (the NRO build compiles the SDK from source: REXSDK_DIR).
#
# Copied: src/graphics/sdnx/, include/rex/graphics/sdnx/, the deko3d core (nx/deko3d -> thirdparty/sdnx-deko3d/
# include+src, for the Switch backend), and the three files that hook it in:
#   src/graphics/CMakeLists.txt (the sdnx sources and the deko3d ones), src/graphics/plugin_main.cpp (backend
#   choice: cvar sdnx_native_renderer), include/rex/ui/vulkan/functions/device_1_0.inc (vkCmdBlitImage, ...).
# The other differences between the two SDKs are emulation/debug changes of the desktop bench and stay out.
# Unchanged files keep their timestamps (no needless rebuild).
set -euo pipefail
SRC=/mnt/d/CODE/me-sdk
DST="$HOME/masseffect-nx/sdk"
DEKO=/mnt/d/CODE/SDNX/nx/deko3d
copy() {  # src dst
  mkdir -p "$(dirname "$2")"
  sed 's/\r$//' "$1" > "$2.tmp"
  if cmp -s "$2.tmp" "$2"; then rm "$2.tmp"; else mv "$2.tmp" "$2"; echo "updated ${1#"$SRC"/}"; fi
}
copyfile() {  # absolute src, absolute dst
  mkdir -p "$(dirname "$2")"
  sed 's/\r$//' "$1" > "$2.tmp"
  if cmp -s "$2.tmp" "$2"; then rm "$2.tmp"; else mv "$2.tmp" "$2"; echo "updated ${1#"$SRC"/}"; fi
}
for d in src/graphics/sdnx include/rex/graphics/sdnx; do
  (cd "$SRC" && find "$d" -type f) | while read -r f; do copyfile "$SRC/$f" "$DST/$f"; done
done
copyfile "$SRC/src/graphics/CMakeLists.txt" "$DST/src/graphics/CMakeLists.txt"
copyfile "$SRC/src/graphics/plugin_main.cpp" "$DST/src/graphics/plugin_main.cpp"
copyfile "$SRC/src/graphics/util/draw.cpp" "$DST/src/graphics/util/draw.cpp"
copyfile "$SRC/include/rex/ui/vulkan/functions/device_1_0.inc" "$DST/include/rex/ui/vulkan/functions/device_1_0.inc"
# deko3d core for the Switch backend.
for d in include src; do
  (cd "$DEKO" && find "$d" -type f) | while read -r f; do
    copyfile "$DEKO/$f" "$DST/thirdparty/sdnx-deko3d/$f"
  done
done
