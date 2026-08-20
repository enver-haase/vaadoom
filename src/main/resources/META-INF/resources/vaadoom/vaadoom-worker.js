/* -----------------------------------------------------------------------------
 * vaadoom-worker.js — module Web Worker that runs the SUBLEQ VM (vaadoom.wasm),
 * boots the bundled NOMMU Linux, launches fbdoom, and paints its framebuffer onto
 * a transferred OffscreenCanvas.
 *
 * Beyond running the VM it services the engine's two optional zero-page devices
 * (see native/em_devices.h):
 *   - the host-file channel, which hands a WAD fetched from `wadUrl` to the guest
 *     without the slow SUBLEQ CPU ever copying it, and
 *   - the PCM sound card, whose drained frames are forwarded to the page (a
 *     Worker has no AudioContext, so the viewport does the playback).
 *
 * Served raw from META-INF/resources/vaadoom/ (NOT Vite-bundled) so the relative
 * import of ./vaadoom.js and the fetch of the boot image resolve against this
 * file's URL. No SharedArrayBuffer, so no COOP/COEP headers are needed.
 * ---------------------------------------------------------------------------*/
import Factory from './vaadoom.js';

const SLICE       = 3_000_000;        // subleq steps per tick (~75ms of compute)
const INJECT_AT   = 1_300_000_000;    // steps after which DOOM is auto-launched
const LAUNCH_CMD  = 'cd /root/doom\n./doom\n';

const DEV_SOUND    = 1;               // em_dev_enable() flags
const DEV_HOSTFILE = 2;

/* Boot-image capabilities. The sidecar vmlinux.bootimage.json overrides these, so a
 * new image can announce that its init already launches DOOM and that it speaks the
 * sound / host-file ABI — without an API change in the component. */
const DEFAULT_IMAGE_INFO = { selfLaunch: false, sound: false, hostFile: false };

const STATS_EVERY = 200;              // slices between engine-stat reports (~15 s)

let Module = null, totalSteps = 0, injected = false, halted = false, slices = 0;
let image = DEFAULT_IMAGE_INFO, config = {};

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
    config = msg.config || {};
    try { await boot(); }
    catch (err) { postMessage({ type: 'error', message: String(err && err.stack || err) }); }
  } else if (msg.type === 'key' && Module) {
    Module._em_kbd_push(msg.code | 0);
  }
};

async function boot() {
  postMessage({ type: 'status', phase: 'loading-engine' });
  Module = await Factory();

  /* The page plays what the sound card produces; a Worker cannot open an
   * AudioContext, so hand each drained block over as a transferable. */
  globalThis.__vdAudio = (pcm, rate) => postMessage({ type: 'pcm', pcm, rate }, [pcm.buffer]);

  const wad = config.wadUrl ? await fetchWad(config.wadUrl) : null;

  postMessage({ type: 'status', phase: 'loading-image' });
  const base = new URL('vmlinux.bootimage.gz', import.meta.url);
  image = await loadImageInfo(base);
  const res = await fetch(base);
  if (!res.ok) throw new Error('boot image fetch failed: ' + res.status);
  const stream = res.body.pipeThrough(new DecompressionStream('gzip'));
  const bytes = new Uint8Array(await new Response(stream).arrayBuffer());
  Module.FS.writeFile('/boot.img', bytes);

  postMessage({ type: 'status', phase: 'booting' });
  Module.callMain([]);                          // load_image + em_video_init; returns

  if (wad) publishWad(wad);
  Module._em_dev_enable((image.sound && config.sound !== false ? DEV_SOUND : 0)
                        | (wad ? DEV_HOSTFILE : 0));
  tick();
}

/* The sidecar describes what the shipped boot image can do. It is optional: an
 * image without one is treated as the stock upstream (shell prompt, no devices). */
async function loadImageInfo(imageUrl) {
  try {
    const res = await fetch(new URL('vmlinux.bootimage.json', imageUrl));
    if (!res.ok) return DEFAULT_IMAGE_INFO;
    return { ...DEFAULT_IMAGE_INFO, ...(await res.json()) };
  } catch {
    return DEFAULT_IMAGE_INFO;
  }
}

/* ------------------------------------------------------------------ the WAD */

async function fetchWad(url) {
  postMessage({ type: 'status', phase: 'loading-wad' });
  let bytes;
  try {
    const res = await fetch(url, { mode: 'cors' });
    if (!res.ok) throw new Error('HTTP ' + res.status);
    bytes = new Uint8Array(await res.arrayBuffer());
  } catch (err) {
    /* A missing or cross-origin-blocked WAD is not fatal: the boot image carries
     * id Software's shareware doom1.wad, so DOOM still runs. */
    postMessage({ type: 'wad', ok: false, message: 'could not load ' + url + ': ' + err });
    return null;
  }
  const tag = String.fromCharCode(...bytes.subarray(0, 4));
  if (tag !== 'IWAD' && tag !== 'PWAD') {
    postMessage({ type: 'wad', ok: false, message: 'not a WAD file: ' + url });
    return null;
  }
  const name = guestWadName(bytes);
  postMessage({ type: 'wad', ok: true, name, bytes: bytes.length });
  return { bytes, name };
}

/* Pick the file name the guest should see. fbdoom keeps vanilla DOOM's
 * IdentifyVersion(), which infers the game mode from the IWAD's *name* — so the
 * name has to match what the lump directory actually contains:
 *   MAP01 -> doom2.wad (commercial)   E4M1 -> doomu.wad (retail/Ultimate)
 *   E2M1  -> doom.wad  (registered)   else -> doom1.wad (shareware)
 */
function guestWadName(bytes) {
  const dv = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  const numLumps = dv.getInt32(4, true), dirOfs = dv.getInt32(8, true);
  const lumps = new Set();
  if (numLumps > 0 && dirOfs > 0 && dirOfs + numLumps * 16 <= bytes.length) {
    for (let i = 0; i < numLumps; i++) {
      let name = '';
      for (let j = 0; j < 8; j++) {
        const ch = bytes[dirOfs + i * 16 + 8 + j];
        if (!ch) break;
        name += String.fromCharCode(ch);
      }
      lumps.add(name);
    }
  }
  if (lumps.has('MAP01')) return 'doom2.wad';
  if (lumps.has('E4M1'))  return 'doomu.wad';
  if (lumps.has('E2M1'))  return 'doom.wad';
  return 'doom1.wad';
}

/* Copy the WAD into the wasm heap and publish it on the host-file device. The
 * guest then pulls the bytes it needs through /dev/wad; nothing is copied by the
 * emulated CPU itself. */
function publishWad({ bytes, name }) {
  const dataPtr = Module._malloc(bytes.length);
  Module.HEAPU8.set(bytes, dataPtr);
  const nameBytes = new TextEncoder().encode(name);
  const namePtr = Module._malloc(nameBytes.length);
  Module.HEAPU8.set(nameBytes, namePtr);
  Module._em_hf_set(dataPtr, bytes.length, namePtr, nameBytes.length);
}

/* ----------------------------------------------------------------- run loop */

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
    if (!image.selfLaunch) typeStr(LAUNCH_CMD); // older images boot to a shell
    postMessage({ type: 'status', phase: 'running' });
  }
  if (++slices % STATS_EVERY === 0) {
    // How much of the WAD the guest has actually pulled through the host-file
    // device, and whether it is driving the OPL chip (music is not synthesized
    // yet). Useful to see that the fetched WAD really is the one being played.
    postMessage({ type: 'stats', wadBytesServed: Module._em_hf_served(),
                  oplWrites: Module._em_opl_writes(), steps: totalSteps });
  }
  // Yield to the worker event loop so the OffscreenCanvas commits the latest
  // frame and queued messages are processed, then continue the next slice.
  setTimeout(tick, 0);
}
