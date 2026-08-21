/* -----------------------------------------------------------------------------
 * test-bank1.mjs — is the OPL3's second voice bank actually usable?
 *
 *   node test-bank1.mjs ./testvm.js
 *
 * The guest uses all eighteen voices; nine of them live in bank 1, addressed by
 * OR-ing 0x100 into the register. Those only come into play in dense passages,
 * so a mistake there would look like "the music falls apart when it gets busy" —
 * which is exactly what we were chasing at the time. Answer: bank 1 is fine
 * (identical output to bank 0, and two voices sum to twice the peak).
 * ---------------------------------------------------------------------------*/
import { writeFileSync } from 'node:fs';
const Factory = (await import(process.argv[2])).default;
const OPL = 67, DEV_SOUND = 1, RATE = 49716;
const FNUM = 408, BLOCK = 4;
const patch = (bank, ch, ops) => [
  [bank | (0x20 + ops[0]), 0x01], [bank | (0x40 + ops[0]), 0x10],
  [bank | (0x60 + ops[0]), 0xF0], [bank | (0x80 + ops[0]), 0x77],
  [bank | (0x20 + ops[1]), 0x01], [bank | (0x40 + ops[1]), 0x00],
  [bank | (0x60 + ops[1]), 0xF0], [bank | (0x80 + ops[1]), 0x77],
  [bank | (0xC0 + ch), 0x30],
  [bank | (0xA0 + ch), FNUM & 0xFF],
  [bank | (0xB0 + ch), 0x20 | (BLOCK << 2) | ((FNUM >> 8) & 3)],
];
const keyoff = (bank, ch) => [[bank | (0xB0 + ch), (BLOCK << 2) | ((FNUM >> 8) & 3)]];
const M = await Factory();
const runProgram = async (writes, slices) => {
  const img = new Int32Array(2048); let pc = 0;
  const here = () => pc * 4;
  const ins = (a,b,c) => { const nx=(pc+3)*4; img[pc++]=a*4; img[pc++]=b*4; img[pc++]=(c===undefined?nx:c); };
  writes.forEach(([reg,val],i) => { img[400+i] = (reg<<8)|val; ins(400+i, OPL); });
  img[900]=0; ins(900,900,here());
  M.FS.writeFile('/boot.img', new Uint8Array(img.buffer));
  M.callMain([]);
  const blocks=[]; globalThis.__vdAudioOpl = (p)=>blocks.push(p);
  M._em_dev_enable(DEV_SOUND);
  for (let i=0;i<slices;i++){ M._em_run_slice(50_000); await new Promise(r=>setTimeout(r,40)); }
  let peak=0, sum=0, n=0;
  for (const b of blocks) for (const v of b){ const a=Math.abs(v); if(a>peak)peak=a; sum+=v*v; n++; }
  return { peak, rms: Math.sqrt(sum/Math.max(n,1)), frames: n/2 };
};
const OPL3ON = [[0x105, 0x01]];
const v0 = await runProgram([...OPL3ON, ...patch(0x000, 0, [0x00, 0x03])], 12);
console.log(`voice 0  (bank 0): peak ${v0.peak.toFixed(0)}  rms ${v0.rms.toFixed(0)}`);
const v9 = await runProgram([...OPL3ON, ...patch(0x100, 0, [0x00, 0x03])], 12);
console.log(`voice 9  (bank 1): peak ${v9.peak.toFixed(0)}  rms ${v9.rms.toFixed(0)}`);
const both = await runProgram([...OPL3ON, ...patch(0x000, 0, [0x00, 0x03]), ...patch(0x100, 0, [0x00, 0x03])], 12);
console.log(`both together    : peak ${both.peak.toFixed(0)}  rms ${both.rms.toFixed(0)}`);
console.log(`\nbank-1 voice ${v9.rms > v0.rms*0.5 ? 'SOUNDS (ok)' : 'is SILENT or wrong <=='}`);
console.log(`two voices vs one: rms ratio ${(both.rms/Math.max(v0.rms,1)).toFixed(2)} (expect ~2 for two identical voices)`);
