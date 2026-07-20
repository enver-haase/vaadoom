/* -----------------------------------------------------------------------------
 * vaadoom-worker.js — module Web Worker that runs the SUBLEQ VM (vaadoom.wasm),
 * boots the bundled NOMMU Linux, auto-launches fbdoom, and paints its framebuffer
 * onto a transferred OffscreenCanvas.
 *
 * Served raw from META-INF/resources/vaadoom/ (NOT Vite-bundled) so the relative
 * import of ./vaadoom.js and the fetch of the boot image resolve against this
 * file's URL. Render-only: no SharedArrayBuffer, so no COOP/COEP needed.
 * ---------------------------------------------------------------------------*/
import Factory from './vaadoom.js';

const SLICE       = 3_000_000;        // subleq steps per tick (~75ms of compute)
const INJECT_AT   = 1_300_000_000;    // steps after which DOOM is auto-launched
const LAUNCH_CMD  = 'cd /root/doom\n./doom\n';

let Module = null, totalSteps = 0, injected = false, halted = false;

/* USB HID usage codes (== SDL scancodes) understood by the subleq keyboard
 * driver: keydown -> +code, keyup -> -code. Enough to type the launch command. */
const HID = { ' ': 44, '/': 56, '.': 55, '\n': 40 };
for (let i = 0; i < 26; i++) HID[String.fromCharCode(97 + i)] = 4 + i; // a..z
function typeStr(s) {
  for (const ch of s) {
    const sc = HID[ch];
    if (!sc) continue;
    Module._em_kbd_push(sc);
    Module._em_kbd_push(-sc);
  }
}

onmessage = async (e) => {
  const msg = e.data;
  if (msg.type === 'start') {
    globalThis.__vaadoomCanvas = msg.canvas;   // transferred OffscreenCanvas
    try { await boot(); }
    catch (err) { postMessage({ type: 'error', message: String(err && err.stack || err) }); }
  } else if (msg.type === 'key' && Module) {
    Module._em_kbd_push(msg.code | 0);          // reserved for the playable stage
  }
};

async function boot() {
  postMessage({ type: 'status', phase: 'loading-engine' });
  Module = await Factory();

  postMessage({ type: 'status', phase: 'loading-image' });
  const res = await fetch(new URL('vmlinux.bootimage.gz', import.meta.url));
  if (!res.ok) throw new Error('boot image fetch failed: ' + res.status);
  const stream = res.body.pipeThrough(new DecompressionStream('gzip'));
  const bytes = new Uint8Array(await new Response(stream).arrayBuffer());
  Module.FS.writeFile('/boot.img', bytes);

  postMessage({ type: 'status', phase: 'booting' });
  Module.callMain([]);                          // load_image + em_video_init; returns
  tick();
}

function tick() {
  if (halted) return;
  const n = Module._em_run_slice(SLICE);
  totalSteps += n;
  if (n < SLICE) {                              // VM halted (STEP_HALT)
    halted = true;
    postMessage({ type: 'status', phase: 'halted' });
    return;
  }
  if (!injected && totalSteps >= INJECT_AT) {
    injected = true;
    typeStr(LAUNCH_CMD);
    postMessage({ type: 'status', phase: 'running' });
  }
  // Yield to the worker event loop so the OffscreenCanvas commits the latest
  // frame and queued messages are processed, then continue the next slice.
  setTimeout(tick, 0);
}
