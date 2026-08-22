/* test-opl-flush.mjs — a song change must not leave a note hanging.
 *
 *   node test-opl-flush.mjs ./testvm.js
 *
 * The regression test for the bug fixed in 1.2.1. A song change reaches the OPL
 * device as OPL_CMD_FLUSH, which drops the scheduled-write queue — and with it the
 * key-offs the old song had planned for the notes still sounding. Releasing those
 * voices is not enough on its own either: DOOM's GENMIDI bank contains instruments
 * with a release rate of 1, which ring for tens of seconds after a key-off, so the
 * device has to force a fast release and put the instrument's own rate back once
 * the envelope has run out.
 *
 * A hand-written subleq program drives the device through four phases, and the test
 * asserts on the audio each one produced:
 *
 *   1  key a voice whose release rate is 1     loud
 *   2  flush, the way a song change does       SILENT   <- the 1.2.0 bug: still loud
 *   3  key the same channel again              loud     <- the envelope retriggers
 *   4  key off, this time without a flush      still audible: the instrument's own
 *                                              slow release came back
 *
 * The guest has no clock of its own here, so it waits on a flag word between phases
 * and this script sets it — which also means the sample index of every phase
 * boundary is known, instead of being guessed from instruction counts.
 */
const Factory = (await import(process.argv[2])).default;

const OPL = 67, DEV_SOUND = 1, RATE = 49716;
const FLUSH = 0x1FF;                      /* not a register: OPL_CMD_FLUSH */
const FNUM = 408, BLOCK = 4;
const KEY_ON  = 0x20 | (BLOCK << 2) | ((FNUM >> 8) & 3);
const KEY_OFF = KEY_ON & ~0x20;

/* A sustaining voice (0x20 bit 5) at full sustain level whose release rate is 1
 * (0x80 = 0x01) — the shape that made D_INTRO's channel 11 carrier ring on. */
const VOICE = [
  [0x105, 0x01],                          /* OPL3 mode, as the guest sets it */
  [0x20, 0x21], [0x40, 0x10], [0x60, 0xF0], [0x80, 0x01], [0xE0, 0x00],
  [0x23, 0x21], [0x43, 0x00], [0x63, 0xF0], [0x83, 0x01], [0xE3, 0x00],
  [0xC0, 0x30],
  [0xA0, FNUM & 0xFF],
];

/* ---- the program ---------------------------------------------------------- */
const img = new Int32Array(4096);
let pc = 0, data = 1000;
const here = () => pc * 4;
const ins = (a, b, c) => { const nx = (pc + 3) * 4; img[pc++] = a * 4; img[pc++] = b * 4; img[pc++] = (c === undefined ? nx : c); };
const word = (v) => { img[data] = v; return data++; };

const TMP = word(0);

/* Two properties of this machine shape the program, and both bite silently.
 *
 * Word 0 is the timer-interrupt vector: every 800k steps, if mem[0] is non-zero,
 * the VM saves pc in mem[1] and jumps to mem[0] — which for a program this small
 * means jumping into its own data and executing it. So mem[0] has to stay zero,
 * and the neatest way is to let the first instruction use word 0 as both operands:
 * 0 - 0 leaves it zero and always branches.
 *
 * And the device registers live in the zero page (RTC at 64, OPL at 67, host-file
 * and PCM just above), so code that grows past word 64 would sit on an MMIO cell
 * and write the device every time it is fetched. Hence the jump clear of it. */
const ORIGIN = 128;
ins(0, 0, ORIGIN * 4);
pc = ORIGIN;

/* mem[OPL] takes the value of the operand, so one instruction per register write. */
const opl = (reg, val) => ins(word(((reg & 0x1FF) << 8) | (val & 0xFF)), OPL);

/* Spin until this script pokes the flag negative. TMP is zeroed, then the flag is
 * subtracted from it: while the flag is still 0 the result is 0 and the branch back
 * is taken; once it is -1 the result is +1 and execution falls through. */
const flags = [];
const waitForFlag = () => {
  const flag = word(0);
  flags.push(flag);
  const loop = here();
  ins(TMP, TMP, (pc + 3) * 4);            /* TMP = 0, then carry on regardless */
  ins(flag, TMP, loop);
};

VOICE.forEach(([r, v]) => opl(r, v));
opl(0xB0, KEY_ON);                        /* phase 1 */
waitForFlag();
opl(FLUSH, 0x00);                         /* phase 2 */
waitForFlag();
opl(0xB0, KEY_ON);                        /* phase 3 */
waitForFlag();
opl(0xB0, KEY_OFF);                       /* phase 4 */
const halt = word(0);
ins(halt, halt, here());

/* ---- run it -------------------------------------------------------------- */
const M = await Factory();
M.FS.writeFile('/boot.img', new Uint8Array(img.buffer));
M.callMain([]);

const pcm = [];                            /* left channel, in order */
globalThis.__vdAudioOpl = (block) => { for (let i = 0; i < block.length; i += 2) pcm.push(block[i]); };
M._em_dev_enable(DEV_SOUND);

const base = M._em_mem_base() >> 2;
const poke = (w, v) => { M.HEAP32[base + w] = v; };
const slice = async () => { M._em_run_slice(50_000); await new Promise((r) => setTimeout(r, 20)); };

/* Let each phase sound for a while, then release the guest into the next one and
 * wait for the register write to actually land. */
const bounds = [];
const PHASE_SLICES = 16;
for (let phase = 0; phase <= flags.length; phase++) {
  const from = pcm.length;
  for (let i = 0; i < PHASE_SLICES; i++) await slice();
  bounds.push([from, pcm.length]);
  if (phase === flags.length) break;
  const before = M._em_opl_writes();
  poke(flags[phase], -1);
  let waited = 0;
  while (M._em_opl_writes() === before && waited++ < 40) await slice();
  if (M._em_opl_writes() === before) {
    console.error(`FAIL: phase ${phase + 2} never issued its register write (guest stuck?)`);
    process.exit(1);
  }
}

/* ---- what each phase has to sound like ----------------------------------- */
const rms = (from, to) => {
  let sum = 0;
  for (let i = from; i < to; i++) sum += pcm[i] * pcm[i];
  return 20 * Math.log10(Math.sqrt(sum / Math.max(to - from, 1)) / 32768 + 1e-12);
};
const tailOf = ([from, to]) => rms(Math.max(from, to - Math.round(RATE * 0.1)), to);   /* last 100 ms */

const label = ['keyed', 'flushed', 're-keyed', 'keyed off'];
const level = bounds.map(tailOf);
const report = () => bounds.map(([a, b], i) =>
  `    ${label[i].padEnd(10)} ${(a / RATE).toFixed(2)}-${(b / RATE).toFixed(2)} s   last 100 ms ${level[i].toFixed(1)} dBFS`).join('\n');
const fail = (msg) => { console.error(`FAIL: ${msg}\n${report()}`); process.exit(1); };

if (pcm.length < RATE / 2) fail(`only ${pcm.length} samples came out of the chip`);
if (level[0] < -30) fail(`the voice never sounded (${level[0].toFixed(1)} dBFS)`);
if (level[1] > -60) fail(`the flush left the voice ringing at ${level[1].toFixed(1)} dBFS — ` +
                         `this is the 1.2.0 bug: a note stays after a song change`);
if (level[2] < -30) fail(`the channel did not sound again after the flush (${level[2].toFixed(1)} dBFS) — ` +
                         `an OPL3 envelope only retriggers if the voice was keyed off`);
if (level[3] < -55) fail(`a plain key-off cut the voice dead (${level[3].toFixed(1)} dBFS) — the release ` +
                         `rate the flush forced was not put back`);

console.log(`OK  ${pcm.length} samples`);
console.log(report());
