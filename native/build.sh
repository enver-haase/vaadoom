#!/usr/bin/env bash
# Build the lunatix SUBLEQ VM. Two targets from ONE source (vm.c):
#   ./build.sh wasm     (default) -> WebAssembly module for the Vaadin add-on
#   ./build.sh native             -> native SDL3 binary (the unchanged upstream build)
set -euo pipefail
cd "$(dirname "$0")"

TARGET="${1:-wasm}"

IMG_URL="https://raw.githubusercontent.com/adriancable/eternal/main/ioccc/vmlinux.bootimage.xz"
RES="../src/main/resources/META-INF/resources/vaadoom"

if [ "$TARGET" = "image" ]; then
  # Fetch the upstream (adriancable/eternal) NOMMU fbdoom boot image and repack it
  # as gzip, which the browser decompresses natively (DecompressionStream). The
  # gz is bundled in the add-on JAR (kept out of git; run this before mvn package).
  mkdir -p "$RES"
  [ -f vmlinux.bootimage.xz ] || { echo ">> downloading $IMG_URL"; curl -fSL "$IMG_URL" -o vmlinux.bootimage.xz; }
  echo ">> decompressing + repacking as gzip"
  xz -dkf vmlinux.bootimage.xz
  gzip -9 -c vmlinux.bootimage > "$RES/vmlinux.bootimage.gz"
  echo ">> done: $RES/vmlinux.bootimage.gz ($(du -h "$RES/vmlinux.bootimage.gz" | cut -f1))"
  exit 0
fi

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
