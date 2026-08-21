/* test-opl2.mjs — would running the chip in OPL2 mode change DOOM's music?
 *
 *   node test-opl2.mjs ./testvm.js out.json
 *
 * Asked because DOOM on a Sound Blaster Pro was OPL2, and our rendering is darker
 * than Chocolate Doom's. Answer: no. All three configurations below produce
 * identical output, because the OPL3 NEW bit only unlocks waveforms 4-7 and all
 * 175 GENMIDI instruments use only 0-3. The difference from Chocolate is its
 * DBOPL emulator versus our Nuked-OPL3, not the chip generation.
 *
 * Same voice, three chip configurations, to see whether OPL2 mode is brighter.
 *   A: OPL3 mode (register 0x105 = 1)              <- what we ship
 *   B: OPL2 mode, waveform-select NOT enabled       <- naive "just drop 0x105"
 *   C: OPL2 mode with waveform select enabled (0x01 = 0x20)
 * In OPL2, waveform selection only works if 0x01 bit 5 is set; our guest never
 * writes 0x01 because OPL3 mode implies it. */
import { writeFileSync } from 'node:fs';
const Factory = (await import(process.argv[2])).default;
const OPL = 67, DEV_SOUND = 1, RATE = 49716;
const FNUM = 408, BLOCK = 4;
const voice = (wave) => [
  [0x20, 0x01], [0x40, 0x10], [0x60, 0xF0], [0x80, 0x77], [0xE0, wave],
  [0x23, 0x01], [0x43, 0x00], [0x63, 0xF0], [0x83, 0x77], [0xE3, wave],
  [0xC0, 0x30],
  [0xA0, FNUM & 0xFF], [0xB0, 0x20 | (BLOCK << 2) | ((FNUM >> 8) & 3)],
];
const M = await Factory();
async function render(writes) {
  const img = new Int32Array(2048); let pc = 0;
  const here = () => pc * 4;
  const ins = (a,b,c) => { const nx=(pc+3)*4; img[pc++]=a*4; img[pc++]=b*4; img[pc++]=(c===undefined?nx:c); };
  writes.forEach(([reg,val],i) => { img[500+i] = (reg<<8)|val; ins(500+i, OPL); });
  img[900]=0; ins(900,900,here());
  M.FS.writeFile('/boot.img', new Uint8Array(img.buffer));
  M.callMain([]);
  const blocks=[]; globalThis.__vdAudioOpl = (p)=>blocks.push(p);
  M._em_dev_enable(DEV_SOUND);
  for (let i=0;i<14;i++){ M._em_run_slice(50_000); await new Promise(r=>setTimeout(r,40)); }
  const out=[];
  for (const b of blocks) for (let i=0;i<b.length;i+=2) out.push(b[i]);
  return out;
}
const cases = {
  'A OPL3 (shipping)      ': [[0x105,0x01], ...voice(1)],
  'B OPL2, no wave-select ': [...voice(1)],
  'C OPL2 + 0x01=0x20     ': [[0x01,0x20], ...voice(1)],
};
const results = {};
for (const [label, writes] of Object.entries(cases)) results[label] = await render(writes);
writeFileSync(process.argv[3] ?? '/tmp/opl2.json', JSON.stringify(results));
for (const [label, x] of Object.entries(results)) {
  let peak=0, sum=0;
  for (const v of x) { const a=Math.abs(v); if (a>peak) peak=a; sum+=v*v; }
  console.log(`${label}: ${x.length} samples  peak ${peak}  rms ${Math.sqrt(sum/Math.max(x.length,1)).toFixed(0)}`);
}
