#!/usr/bin/env bash
# Build the lunatix SUBLEQ VM. Two targets from ONE source (vm.c):
#   ./build.sh wasm     (default) -> WebAssembly module for the Vaadin add-on
#   ./build.sh native             -> native SDL3 binary (the unchanged upstream build)
#   ./build.sh test               -> node self-test of the zero-page devices
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

# Which VM source to build. The shipped NOMMU engine is vm_nommu.c (fast, no MMU);
# vm.c is the MMU-capable engine kept for the future MMU-DOOM path and selftest.
NATIVE_SRC="${NATIVE_SRC:-vm.c}"          # native/native-nommu pick this
WASM_SRC="${WASM_SRC:-vm_nommu.c}"        # shipped wasm engine

if [ "$TARGET" = "native" ] || [ "$TARGET" = "native-nommu" ]; then
  CC="${CC:-cc}"
  SRC="$NATIVE_SRC"; OUTBIN=lunavm
  [ "$TARGET" = "native-nommu" ] && { SRC=vm_nommu.c; OUTBIN=vmnommu; }
  echo ">> native SDL3 build ($SRC) -> ./$OUTBIN"
  # shellcheck disable=SC2046
  $CC -std=gnu17 -O3 -fwrapv -DSDL_MAIN_HANDLED $(pkg-config sdl3 --cflags) \
      "$SRC" -o "$OUTBIN" $(pkg-config sdl3 --libs)
  echo ">> done: ./native/$OUTBIN   (run: ./$OUTBIN < image.bootimage)"
  exit 0
fi

if [ "$TARGET" = "test" ]; then
  # Device self-test: a small-memory wasm build driven by a hand-written subleq
  # program (test/test-devices.mjs) that exercises the host-file registers and
  # proves the devices stay inert until em_dev_enable() is called.
  : "${EMSDK:=$HOME/emsdk}"
  # shellcheck disable=SC1091
  source "$EMSDK/emsdk_env.sh" >/dev/null 2>&1 || true
  TMP="$(mktemp -d)"
  emcc -O1 -fwrapv -DMEM_WORDS=262144 "$WASM_SRC" opl_shim.c third_party/nuked/opl3.c -o "$TMP/testvm.js" \
    -sMODULARIZE=1 -sEXPORT_ES6=1 -sENVIRONMENT=web,worker,node \
    -sINVOKE_RUN=0 -sEXIT_RUNTIME=0 -sALLOW_MEMORY_GROWTH=1 -sSTACK_SIZE=1048576 \
    -sEXPORTED_RUNTIME_METHODS=callMain,FS,ccall,HEAPU8,HEAP32 \
    -sEXPORTED_FUNCTIONS=_main,_em_run_slice,_em_kbd_push,_em_dev_enable,_em_hf_set,_em_opl_writes,_em_opl_trace_enable,_em_hf_served,_em_mem_base,_malloc,_free
  node test/test-devices.mjs "$TMP/testvm.js"
  rc=$?
  rm -rf "$TMP"
  exit $rc
fi

# ---- wasm (default) --------------------------------------------------------
: "${EMSDK:=$HOME/emsdk}"
# shellcheck disable=SC1091
source "$EMSDK/emsdk_env.sh" >/dev/null 2>&1 || true

OUT="../src/main/resources/META-INF/resources/vaadoom"
mkdir -p "$OUT"

echo ">> wasm build ($WASM_SRC) -> $OUT/vaadoom.{js,wasm}"
emcc -O3 -fwrapv "$WASM_SRC" opl_shim.c third_party/nuked/opl3.c -o "$OUT/vaadoom.js" \
  -sMODULARIZE=1 -sEXPORT_ES6=1 \
  -sENVIRONMENT=web,worker,node \
  -sINVOKE_RUN=0 -sEXIT_RUNTIME=0 \
  -sALLOW_MEMORY_GROWTH=1 -sMAXIMUM_MEMORY=2147483648 \
  -sSTACK_SIZE=1048576 \
  -sEXPORTED_RUNTIME_METHODS=callMain,FS,ccall,HEAPU8,HEAP32 \
  -sEXPORTED_FUNCTIONS=_main,_em_run_slice,_em_kbd_push,_em_dev_enable,_em_hf_set,_em_opl_writes,_em_opl_trace_enable,_em_hf_served,_em_mem_base,_malloc,_free

echo ">> done."
ls -la "$OUT"/vaadoom.js "$OUT"/vaadoom.wasm
