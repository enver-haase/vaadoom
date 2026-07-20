#!/usr/bin/env bash
# Build the lunatix SUBLEQ VM. Two targets from ONE source (vm.c):
#   ./build.sh wasm     (default) -> WebAssembly module for the Vaadin add-on
#   ./build.sh native             -> native SDL3 binary (the unchanged upstream build)
set -euo pipefail
cd "$(dirname "$0")"

TARGET="${1:-wasm}"

if [ "$TARGET" = "native" ]; then
  # The original build path — proves the #ifdef seam didn't disturb it. Needs SDL3.
  CC="${CC:-cc}"
  echo ">> native SDL3 build -> ./lunavm"
  # shellcheck disable=SC2046
  $CC -std=gnu17 -O3 -fwrapv -DSDL_MAIN_HANDLED $(pkg-config sdl3 --cflags) \
      vm.c -o lunavm $(pkg-config sdl3 --libs)
  echo ">> done: ./native/lunavm   (run: ./lunavm < image.bootimage)"
  exit 0
fi

# ---- wasm (default) --------------------------------------------------------
: "${EMSDK:=$HOME/emsdk}"
# shellcheck disable=SC1091
source "$EMSDK/emsdk_env.sh" >/dev/null 2>&1 || true

OUT="../src/main/resources/META-INF/resources/vaadoom"
mkdir -p "$OUT"

echo ">> wasm build -> $OUT/vaadoom.{js,wasm}"
emcc -O3 -fwrapv vm.c -o "$OUT/vaadoom.js" \
  -sMODULARIZE=1 -sEXPORT_ES6=1 \
  -sENVIRONMENT=web,worker,node \
  -sINVOKE_RUN=0 -sEXIT_RUNTIME=0 \
  -sALLOW_MEMORY_GROWTH=1 -sMAXIMUM_MEMORY=2147483648 \
  -sSTACK_SIZE=1048576 \
  -sEXPORTED_RUNTIME_METHODS=callMain,FS,ccall \
  -sEXPORTED_FUNCTIONS=_main,_em_run_slice,_em_kbd_push

echo ">> done."
ls -la "$OUT"/vaadoom.js "$OUT"/vaadoom.wasm
