/* -----------------------------------------------------------------------------
 * render-music.mjs — boot the real image headless and record what the OPL3 chip
 * actually plays, to a WAV.
 *
 *   node render-music.mjs <vaadoom.js> <vmlinux.bootimage.gz> <IWAD> <out.wav> [seconds]
 *
 * Same engine, same boot image, same WAD as the browser — but no page, no worker,
 * no audio graph and no cache, so what comes out is the engine's music and
 * nothing else. Useful both for listening to it away from the browser and for
 * measuring it: the guest paces its sequencer off the wall clock, so this runs in
 * real time and takes about (boot + seconds) to finish.
 * ---------------------------------------------------------------------------*/
import { readFileSync, writeFileSync } from 'node:fs';
import { gunzipSync } from 'node:zlib';

const [enginePath, imagePath, wadPath, outPath, secsArg] = process.argv.slice(2);
if (!enginePath || !imagePath || !wadPath || !outPath) {
  console.error('usage: node render-music.mjs <vaadoom.js> <image.gz> <IWAD> <out.wav> [seconds]');
  process.exit(2);
}
const RECORD_SECONDS = Number(secsArg || 30);
/* The component does not play the chip's output raw: the page puts the music
 * through a gain stage (Vaadoom.setMusicGain, default 0.4) so it sits under the
 * sound effects. Rendering without it produces a file ~8 dB hotter than anything
 * a user hears, which makes an A/B against another port meaningless — and reads
 * as harshness in dense passages that is not actually there. */
const MUSIC_GAIN = Number(process.env.MUSIC_GAIN ?? 0.6);
const OPL_RATE = 49716;
const SLICE = 3_000_000;
const DEV_SOUND = 1, DEV_HOSTFILE = 2;

const Factory = (await import(enginePath)).default;
const M = await Factory();

/* The same three steps the worker performs, in the same order. */
console.error('decompressing the boot image…');
M.FS.writeFile('/boot.img', gunzipSync(readFileSync(imagePath)));
M.callMain([]);

const wad = readFileSync(wadPath);
const name = Buffer.from('doom1.wad');
const put = (buf) => { const p = M._malloc(buf.length); M.HEAPU8.set(buf, p); return p; };
M._em_hf_set(put(wad), wad.length, put(name), name.length);
M._em_dev_enable(DEV_SOUND | DEV_HOSTFILE);
M._em_opl_trace_enable(1);

/* Mirror the guest's register writes alongside the audio, so a rendering can be
 * explained afterwards: which note was keyed when, and when it was released. */
globalThis.__vdOplLog = [];
const music = [];
let recording = false, recordedFrames = 0;
globalThis.__vdAudioOpl = (pcm) => { if (recording) { music.push(pcm); recordedFrames += pcm.length >> 1; } };
globalThis.__vdAudio = () => {};                 /* SFX exists; not recorded here */

console.error('booting (DOOM starts after ~35 s of real time)…');
const started = Date.now();
let lastReport = 0;

/* Run the VM the way the worker does: bounded slices, yielding between them so
 * the wall clock — which is what the guest's sequencer follows — keeps moving. */
while (true) {
  M._em_run_slice(SLICE);
  await new Promise((r) => setTimeout(r, 0));

  const opl = M._em_opl_writes();
  if (!recording && opl > 0) {                   /* the guest has started the music */
    recording = true;
    console.error(`music started after ${((Date.now() - started) / 1000).toFixed(0)} s`);
  }
  const secs = recordedFrames / OPL_RATE;
  if (recording && secs - lastReport >= 5) {
    lastReport = secs;
    console.error(`  recorded ${secs.toFixed(0)} s (${opl} register writes)`);
  }
  if (recording && secs >= RECORD_SECONDS) break;
  if ((Date.now() - started) / 1000 > 120 + RECORD_SECONDS) {
    console.error('giving up: the guest never drove the chip');
    break;
  }
}

/* Concatenate and write a 16-bit stereo WAV at the OPL's native rate. */
const frames = recordedFrames;
const data = Buffer.alloc(frames * 4);
let o = 0;
for (const block of music) for (let i = 0; i < block.length; i++) {
  let v = Math.round(block[i] * MUSIC_GAIN);
  if (v > 32767) v = 32767; else if (v < -32768) v = -32768;
  data.writeInt16LE(v, o); o += 2;
}

const hdr = Buffer.alloc(44);
hdr.write('RIFF', 0); hdr.writeUInt32LE(36 + data.length, 4); hdr.write('WAVE', 8);
hdr.write('fmt ', 12); hdr.writeUInt32LE(16, 16); hdr.writeUInt16LE(1, 20); hdr.writeUInt16LE(2, 22);
hdr.writeUInt32LE(OPL_RATE, 24); hdr.writeUInt32LE(OPL_RATE * 4, 28);
hdr.writeUInt16LE(4, 32); hdr.writeUInt16LE(16, 34);
hdr.write('data', 36); hdr.writeUInt32LE(data.length, 40);
writeFileSync(outPath, Buffer.concat([hdr, data]));
writeFileSync(outPath.replace(/\.wav$/, '') + '-trace.json', JSON.stringify(globalThis.__vdOplLog));

let peak = 0, sum = 0;
for (const b of music) for (const raw of b) { const v = raw * MUSIC_GAIN; const a = Math.abs(v); if (a > peak) peak = a; sum += v * v; }
const rms = Math.sqrt(sum / Math.max(frames * 2, 1));
/* A full queue is not a cosmetic problem: the overflow path applies those writes
 * immediately instead of at their scheduled time, which scrambles the timing of
 * exactly the densest passages. It went unnoticed through several builds because
 * nothing reported it, so this exits non-zero rather than printing a warning. */
const overflows = M._em_opl_overflows();
if (overflows > 0) {
  console.error(`FAIL: ${overflows} scheduled writes were applied early because the ` +
    `queue was full — the music's timing is corrupted. Raise OPL_SCHED_MAX ` +
    `(native/em_devices.h) or shorten MUS_LOOKAHEAD_MS in the guest.`);
  process.exitCode = 1;
} else {
  console.error('queue never overflowed: every write landed at its scheduled sample');
}
console.error(`\nwrote ${outPath}: ${(frames / OPL_RATE).toFixed(1)} s, gain ${MUSIC_GAIN}, ` +
  `peak ${(20 * Math.log10(peak / 32768)).toFixed(1)} dBFS, rms ${(20 * Math.log10(rms / 32768)).toFixed(1)} dBFS`);
process.exit(0);
