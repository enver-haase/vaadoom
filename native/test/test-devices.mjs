/* -----------------------------------------------------------------------------
 * test-devices.mjs — self-test for the two optional zero-page devices, run under
 * node against a small-memory build of the engine (./build.sh test).
 *
 * The "guest" here is a hand-written subleq program: it drives the device
 * registers exactly as the kernel's raw-.word helpers do, and we check what the
 * host side made of it. It also proves the devices stay INERT until
 * em_dev_enable() is called — which is what keeps the stock upstream boot image
 * behaving exactly as before.
 * ---------------------------------------------------------------------------*/
const Factory = (await import(process.argv[2] ?? './testvm.js')).default;

const HF  = { SEL: 72, DEST: 73, OFF: 74, LEN: 75, SIZE: 76, DONE: 77 };
const PCM = { OPL: 67, BASE: 68, FRAMES: 69, WRITE: 70, RATE: 71 };
const DEV_SOUND = 1, DEV_HOSTFILE = 2;

const WAD  = new TextEncoder().encode('IWADABCDEFGHIJKLMNOPQRSTUVWXYZ');
const NAME = new TextEncoder().encode('doom2.wad');

let failures = 0;
const check = (name, got, want) => {
  const ok = Object.is(got, want);
  if (!ok) failures++;
  console.log(`${ok ? 'PASS' : 'FAIL'}  ${name}: got ${got}${ok ? '' : `, want ${want}`}`);
};

const M = await Factory();
const alloc = (bytes) => { const p = M._malloc(bytes.length); M.HEAPU8.set(bytes, p); return p; };

/* Assemble one program into a boot image. `ins(a, b, c)`: operands are BYTE
 * addresses; `c` defaults to the next instruction, `c = 0` halts. */
function image(build) {
  const img = new Int32Array(2048);
  let pc = 0;
  const here = () => pc * 4;          // byte address of the next instruction written
  const ins = (a, b, c) => {
    const next = (pc + 3) * 4;
    img[pc++] = a * 4; img[pc++] = b * 4; img[pc++] = (c === undefined ? next : c);
  };
  build(ins, img, here);
  return img;
}

/* Boot a fresh machine on `img` and hand back a view of its RAM. */
function boot(img) {
  M.FS.writeFile('/boot.img', new Uint8Array(img.buffer));
  M.callMain([]);
  const base = M._em_mem_base();
  return { base, word: (w) => M.HEAP32[(base >> 2) + w] };
}

/* ------------------------------------------------- host file (the WAD) ---- */

const C_ZERO = 200, C_DEST = 201, C_LEN = 202, OUT_DONE = 210, OUT_SIZE = 211, DEST_W = 300;

const hostFileProgram = image((ins, img) => {
  img[C_ZERO] = 0; img[C_DEST] = DEST_W; img[C_LEN] = 16;
  ins(C_ZERO, HF.SEL);        // SEL  = 0  (the file bytes)
  ins(C_DEST, HF.DEST);       // DEST = word 300
  ins(C_ZERO, HF.OFF);        // OFF  = 0
  ins(C_LEN,  HF.LEN);        // LEN  = 16 -> the transfer happens on this write
  ins(HF.DONE, OUT_DONE);     // OUT_DONE -= DONE   (sourcing a read register refreshes it)
  ins(HF.SIZE, OUT_SIZE, 0);  // OUT_SIZE -= SIZE, then halt
});

/* 1. devices disabled: every register write must act as an ordinary subleq store */
{
  const g = boot(hostFileProgram);
  M._em_hf_set(alloc(WAD), WAD.length, alloc(NAME), NAME.length);
  M._em_dev_enable(0);
  M._em_run_slice(100);
  check('inert: no transfer happened', g.word(DEST_W), 0);
  check('inert: LEN cell took the plain store', g.word(HF.LEN), -16);   // 0 -= 16
}

/* 2. enabled: the same program pulls 16 bytes out of the host's WAD */
{
  const g = boot(hostFileProgram);
  M._em_hf_set(alloc(WAD), WAD.length, alloc(NAME), NAME.length);
  M._em_dev_enable(DEV_HOSTFILE);
  M._em_run_slice(100);
  const copied = new Uint8Array(M.HEAPU8.buffer, g.base + DEST_W * 4, 16);
  check('transfer: 16 bytes copied', new TextDecoder().decode(copied), 'IWADABCDEFGHIJKL');
  check('MMIO_HF_DONE = bytes copied', -g.word(OUT_DONE), 16);
  check('MMIO_HF_SIZE = WAD size',     -g.word(OUT_SIZE), WAD.length);
}

/* 3. the name stream, and a request past the end of the file */
{
  const g = boot(image((ins, img) => {
    img[C_ZERO] = 0; img[C_DEST] = DEST_W; img[C_LEN] = 9; img[203] = 1; img[204] = 9999;
    ins(203,    HF.SEL);        // SEL = 1 (the suggested file name)
    ins(C_DEST, HF.DEST);
    ins(C_ZERO, HF.OFF);
    ins(C_LEN,  HF.LEN);        // copy all 9 bytes of "doom2.wad"
    ins(HF.SIZE, OUT_SIZE);     // OUT_SIZE -= SIZE (of the *name* stream)
    ins(204,    HF.OFF);        // OFF = 9999, past the end
    ins(C_LEN,  HF.LEN);        // -> rejected
    ins(HF.DONE, OUT_DONE, 0);
  }));
  M._em_hf_set(alloc(WAD), WAD.length, alloc(NAME), NAME.length);
  M._em_dev_enable(DEV_HOSTFILE);
  M._em_run_slice(100);
  const copied = new Uint8Array(M.HEAPU8.buffer, g.base + DEST_W * 4, 9);
  check('name stream: contents', new TextDecoder().decode(copied), 'doom2.wad');
  check('name stream: size',     -g.word(OUT_SIZE), NAME.length);
  check('offset past EOF is rejected', -g.word(OUT_DONE), -1);
}

/* --------------------------------------------------------- sound card ---- */

/* The guest arms the PCM ring exactly like drivers/char/subleq_sound.c does:
 * rate and capacity first, the producer counter and the base last. */
const RING_W = 400, RING_FRAMES = 4;
const soundProgram = image((ins, img) => {
  img[220] = 22050; img[221] = RING_FRAMES; img[222] = RING_W; img[223] = RING_FRAMES;
  ins(220, PCM.RATE);
  ins(221, PCM.FRAMES);
  ins(222, PCM.BASE);
  ins(223, PCM.WRITE, 0);      // publish 4 enqueued frames, then halt
});

{
  const g = boot(soundProgram);
  /* Fill the ring: one stereo frame per word, low 16 bits = L, high = R. */
  const frames = [[1000, -1000], [2000, -2000], [3000, -3000], [-32768, 32767]];
  frames.forEach(([l, r], i) => {
    M.HEAP32[(g.base >> 2) + RING_W + i] = (l & 0xFFFF) | (r << 16);
  });

  let got = null;
  globalThis.__vdAudio = (pcm, rate) => { got = { pcm, rate }; };
  M._em_dev_enable(DEV_SOUND);
  M._em_run_slice(100);        // audio_pump() runs at the end of every slice

  check('sound: a block reached the page', got !== null, true);
  check('sound: sample rate', got?.rate, 22050);
  check('sound: frame count', got ? got.pcm.length / 2 : 0, RING_FRAMES);
  check('sound: frames unpacked L/R', got ? Array.from(got.pcm).join(',') : '',
        frames.flat().join(','));

  got = null;
  M._em_run_slice(100);
  check('sound: drained ring produces nothing', got, null);
}

/* ------------------------------------------------------- OPL3 music ------ */

/* The guest programs one 2-op FM voice and keys a note on, writing packed
 * (reg << 8) | val words to MMIO_OPL exactly as the kernel's /dev/opl does. The
 * host runs Nuked-OPL3 and must hand the page a non-silent 49716 Hz stream. The
 * program then spins forever, so slices keep running and the wall-clock-driven
 * generator has time to produce audio (which is the whole point: generation has
 * to track real time for the register stamping to mean anything). */
const OPL_PATCH = [
  [0x105, 0x01],                                              // OPL3 mode
  [0x20, 0x01], [0x40, 0x1A], [0x60, 0xF2], [0x80, 0x25],     // modulator
  [0x23, 0x01], [0x43, 0x00], [0x63, 0xF2], [0x83, 0x15],     // carrier
  [0xC0, 0x3E],                                               // L+R, feedback, FM
  [0xA0, 0x98], [0xB0, 0x31],                                 // note, key-on
];

{
  const ZERO = 501;
  boot(image((ins, img, here) => {
    OPL_PATCH.forEach(([reg, val], i) => {
      img[220 + i] = (reg << 8) | val;
      ins(220 + i, PCM.OPL);
    });
    img[ZERO] = 0;
    ins(ZERO, ZERO, here());     // spin here forever: 0 -= 0 always branches to itself
  }));

  const blocks = [];
  globalThis.__vdAudioOpl = (pcm, rate) => blocks.push({ frames: pcm.length / 2, rate,
    peak: pcm.reduce((m, v) => Math.max(m, Math.abs(v)), 0) });
  M._em_dev_enable(DEV_SOUND);

  for (let i = 0; i < 8; i++) {                 // let real time pass between slices
    M._em_run_slice(50_000);
    await new Promise((r) => setTimeout(r, 40));
  }
  M._em_run_slice(50_000);

  const frames = blocks.reduce((a, b) => a + b.frames, 0);
  const peak = blocks.reduce((m, b) => Math.max(m, b.peak), 0);
  check('opl: register writes reached the chip', M._em_opl_writes(), OPL_PATCH.length);
  check('opl: music was generated', frames > 4000, true);
  check('opl: at the OPL3 native rate', blocks.length ? blocks[0].rate : 0, 49716);
  check('opl: the note is audible (non-silent)', peak > 1000, true);
  console.log(`      (${blocks.length} blocks, ${frames} frames, peak ${peak})`);
}

/* A scheduled write must land where the guest asked, not where it arrived. The
 * guest keys a note "200 ms from now"; nothing may sound before that, and it
 * must be sounding shortly after. This is the contract that keeps the score's
 * timing intact even though the guest's render loop runs at ~7 Hz. */
{
  const ZERO = 501, SCHED_MS = 200;
  const sched = (reg, val) => ((SCHED_MS << 17) | ((reg & 0x1FF) << 8) | (val & 0xFF));
  boot(image((ins, img, here) => {
    OPL_PATCH.forEach(([reg, val], i) => {         // patch now, note in the future
      const last = i >= OPL_PATCH.length - 2;      // 0xA0 / 0xB0 = the note itself
      img[220 + i] = last ? sched(reg, val) : ((reg << 8) | val);
      ins(220 + i, PCM.OPL);
    });
    img[ZERO] = 0;
    ins(ZERO, ZERO, here());
  }));

  const timeline = [];
  globalThis.__vdAudioOpl = (pcm, rate) => timeline.push(
    pcm.reduce((m, v) => Math.max(m, Math.abs(v)), 0));
  M._em_dev_enable(DEV_SOUND);

  const start = Date.now();
  let firstSoundMs = null, framesBefore = 0;
  for (let i = 0; i < 14; i++) {
    M._em_run_slice(50_000);
    await new Promise((r) => setTimeout(r, 40));
    if (firstSoundMs === null && timeline.some((p) => p > 500)) firstSoundMs = Date.now() - start;
  }
  check('opl: a scheduled note stays silent until its time',
        firstSoundMs === null || firstSoundMs >= SCHED_MS * 0.8, true);
  check('opl: and does sound afterwards', timeline.some((p) => p > 500), true);
  console.log(`      (first sound at ${firstSoundMs} ms, scheduled for ${SCHED_MS} ms)`);
}

console.log(failures ? `\n${failures} FAILED` : '\nRESULT devices ... PASS');
process.exit(failures ? 1 : 0);
