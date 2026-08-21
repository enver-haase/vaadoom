/* -----------------------------------------------------------------------------
 * test-pitch.mjs — does the engine render an OPL note at the frequency the
 * register pair asks for?
 *
 *   node test-pitch.mjs ./testvm.js out.wav
 *
 * Drives the chip from a hand-written subleq program (no boot image, no guest)
 * and writes what comes out. The OPL contract is
 *     f = fnum * 49716 / 2^(20 - block)
 * so the values below ask for a specific frequency and the WAV can be measured
 * against it. Written after our rendering was found to sit exactly one octave
 * below another DOOM port's on the same score.
 * ---------------------------------------------------------------------------*/
import { writeFileSync } from 'node:fs';

const Factory = (await import(process.argv[2] ?? './testvm.js')).default;
const outPath = process.argv[3] ?? 'pitch.wav';
const OPL = 67, DEV_SOUND = 1, OPL_RATE = 49716;

/* One 2-op voice, then key a note: fnum 408, block 4.
 * f = 408 * 49716 / 2^16 = 309.4 Hz  (roughly D#4) */
const FNUM = 408, BLOCK = 4;
const EXPECT = FNUM * OPL_RATE / Math.pow(2, 20 - BLOCK);
const PATCH = [
  [0x105, 0x01],
  [0x20, 0x01], [0x40, 0x10], [0x60, 0xF0], [0x80, 0x77],
  [0x23, 0x01], [0x43, 0x00], [0x63, 0xF0], [0x83, 0x77],
  [0xC0, 0x30],
  [0xA0, FNUM & 0xFF], [0xB0, 0x20 | (BLOCK << 2) | ((FNUM >> 8) & 3)],
];

const M = await Factory();
const img = new Int32Array(2048);
let pc = 0;
const here = () => pc * 4;
const ins = (a, b, c) => {
  const next = (pc + 3) * 4;
  img[pc++] = a * 4; img[pc++] = b * 4; img[pc++] = (c === undefined ? next : c);
};
PATCH.forEach(([reg, val], i) => { img[220 + i] = (reg << 8) | val; ins(220 + i, OPL); });
img[300] = 0;
ins(300, 300, here());                      // spin, so slices keep running

M.FS.writeFile('/boot.img', new Uint8Array(img.buffer));
M.callMain([]);

const blocks = [];
globalThis.__vdAudioOpl = (pcm) => blocks.push(pcm);
M._em_dev_enable(DEV_SOUND);
for (let i = 0; i < 30; i++) {
  M._em_run_slice(50_000);
  await new Promise((r) => setTimeout(r, 40));
}

const frames = blocks.reduce((n, b) => n + (b.length >> 1), 0);
const data = Buffer.alloc(frames * 4);
let o = 0;
for (const b of blocks) for (let i = 0; i < b.length; i++) { data.writeInt16LE(b[i], o); o += 2; }
const hdr = Buffer.alloc(44);
hdr.write('RIFF', 0); hdr.writeUInt32LE(36 + data.length, 4); hdr.write('WAVE', 8);
hdr.write('fmt ', 12); hdr.writeUInt32LE(16, 16); hdr.writeUInt16LE(1, 20); hdr.writeUInt16LE(2, 22);
hdr.writeUInt32LE(OPL_RATE, 24); hdr.writeUInt32LE(OPL_RATE * 4, 28);
hdr.writeUInt16LE(4, 32); hdr.writeUInt16LE(16, 34);
hdr.write('data', 36); hdr.writeUInt32LE(data.length, 40);
writeFileSync(outPath, Buffer.concat([hdr, data]));
console.log(`wrote ${outPath}: ${frames} frames at ${OPL_RATE} Hz`);
console.log(`the registers ask for ${EXPECT.toFixed(1)} Hz (fnum ${FNUM}, block ${BLOCK})`);
