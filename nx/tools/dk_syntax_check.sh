#!/usr/bin/env bash
# Syntax-check the deko3d backend for the Switch (devkitA64 container), without building the whole NRO.
#   SUDO_PW=... bash /mnt/d/CODE/SDNX/nx/tools/dk_syntax_check.sh [file.cpp]
# Files are mounted read-only; nothing is written.
set -uo pipefail
FILES="${*:-/sdk/src/graphics/sdnx/deko3d_backend.cpp}"
docker_cmd() {
  if docker info >/dev/null 2>&1; then docker "$@";
  elif [[ -n "${SUDO_PW:-}" ]]; then echo "$SUDO_PW" | sudo -S -p "" docker "$@"; else sudo docker "$@"; fi
}
docker_cmd run --rm \
  -v /mnt/d/CODE/me-sdk:/sdk:ro \
  -v /mnt/d/CODE/SDNX/nx/deko3d:/deko:ro \
  devkitpro/devkita64 bash -lc '
    set -u
    CXX=$DEVKITPRO/devkitA64/bin/aarch64-none-elf-g++
    rc=0
    for f in '"$FILES"'; do
      echo "== $f"
      $CXX -std=c++23 -fsyntax-only -w -D_GNU_SOURCE -DSPDLOG_NO_TZ_OFFSET \
        -D__SWITCH__ -I/sdk/include -I/sdk/src/graphics -I/sdk/src/graphics/sdnx \
        -I/sdk/thirdparty/vulkan-headers/include -I/sdk/thirdparty/spdlog/include \
        -I/sdk/thirdparty/fmt/include \
        -I/deko/include -I$DEVKITPRO/libnx/include -I$DEVKITPRO/portlibs/switch/include \
        "$f" 2>&1 | head -40
      [ ${PIPESTATUS[0]} -eq 0 ] || rc=1
    done
    exit $rc'
