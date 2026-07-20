/* -----------------------------------------------------------------------------
 * em_backend.h — WebAssembly (Emscripten) backend for the lunatix SUBLEQ VM.
 *
 * Included by vm.c ONLY when compiling with emcc (__EMSCRIPTEN__ defined). It
 * replaces the three SDL3 touch-points — video present, keyboard poll, and the
 * SDL init in main() — so the native SDL3 build stays unchanged behind #ifndef.
 *
 * Definitions live here (header-with-definitions) because vm.c is a single
 * translation unit and its run()/step() are static; this keeps the whole VM in
 * one compile with no extra object file.
 *
 * Derived from Adrian Cable's SUBLEQ VM ("cable"/"eternal", MIT). MIT licensed.
 * ---------------------------------------------------------------------------*/
#ifndef LUNATIX_EM_BACKEND_H
#define LUNATIX_EM_BACKEND_H

#include <emscripten.h>
#include <stdint.h>

/* --------------------------------------------------------------------- video */
/* The worker stores an OffscreenCanvas on globalThis.__vaadoomCanvas before it
 * calls main(); em_video_init() grabs its 2D context and an ImageData scratch. */
EM_JS(void, em_video_init, (int w, int h), {
  var cv = globalThis.__vaadoomCanvas;
  if (!cv) return;                                  /* headless (node smoke test) */
  cv.width = w; cv.height = h;
  globalThis.__vdCtx   = cv.getContext('2d');
  globalThis.__vdImage = globalThis.__vdCtx.createImageData(w, h);
});

/* Present FB_BYTES bytes at `ptr` (32-bit pixels in the wasm heap) to the canvas.
 * DOOM/the kernel writes little-endian XRGB (bytes B,G,R,X); canvas ImageData is
 * R,G,B,A — so swap R<->B and force alpha. (Flip this if colors look wrong.) */
EM_JS(void, em_fb_present, (const uint8_t *ptr, int bytes), {
  var ctx = globalThis.__vdCtx;
  if (!ctx) return;                                 /* headless: no-op */
  var img = globalThis.__vdImage, dst = img.data;
  for (var i = 0; i < bytes; i += 4) {
    dst[i]     = HEAPU8[ptr + i + 2];               /* R */
    dst[i + 1] = HEAPU8[ptr + i + 1];               /* G */
    dst[i + 2] = HEAPU8[ptr + i];                   /* B */
    dst[i + 3] = 255;                               /* A */
  }
  ctx.putImageData(img, 0, 0);
});

/* ------------------------------------------------------------------ keyboard */
/* A tiny ring the JS side fills (keydown -> +scancode, keyup -> -scancode, the
 * same signed encoding the native SDL path produced) and the VM drains one code
 * per MMIO poll. Inert for render-only (nothing pushes) until the playable
 * stage wires keyboard events in the worker. */
#define EM_KBD_CAP 256
static int32_t em_kbd_ring[EM_KBD_CAP];
static int     em_kbd_head = 0, em_kbd_tail = 0;

/* Called from JS on key events: Module._em_kbd_push(code). */
EMSCRIPTEN_KEEPALIVE void em_kbd_push(int32_t code) {
  int nt = (em_kbd_tail + 1) % EM_KBD_CAP;
  if (nt != em_kbd_head) { em_kbd_ring[em_kbd_tail] = code; em_kbd_tail = nt; }
}

/* Returns the next queued scancode, or 0 if none. */
static int32_t em_kbd_poll(void) {
  if (em_kbd_head == em_kbd_tail) return 0;
  int32_t c = em_kbd_ring[em_kbd_head];
  em_kbd_head = (em_kbd_head + 1) % EM_KBD_CAP;
  return c;
}

#endif /* LUNATIX_EM_BACKEND_H */
