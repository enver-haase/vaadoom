/* vaadoom-audio.js — the audio clock follows the emulator, not the device.
 *
 * The problem this solves. The guest is a Linux machine emulated one SUBLEQ
 * instruction at a time, and on a fast desktop it produces sound at roughly 97%
 * of real time — measured, steady, and not the fault of the OPL emulation (which
 * costs about 2% of the total). A producer at 0.97x feeding a sound card at 1.00x
 * drains any fixed lead and then leaves a gap, forever, at a regular cadence: the
 * "tiny dropouts". Rendering the same music to a file has never had the artefact,
 * because a file is played back on the emulator's own timeline, where 97% of real
 * time simply means the file took a little longer to write.
 *
 * So this processor makes playback follow the emulator too. It keeps a ring buffer
 * per stream and resamples out of it at a ratio that is adjusted, slowly, to hold
 * the buffer near a target depth. If the guest runs slow the ratio settles a couple
 * of percent below nominal and the music plays that much slower — the same content,
 * the same relative tuning, just stretched, which is inaudible without a reference
 * to compare against. If the guest speeds up, the ratio comes back on its own. This
 * is what emulators generally call dynamic rate control, and it is the difference
 * between a machine-specific hack and something that also holds up on a visitor's
 * laptop that is 20% behind rather than 3%.
 *
 * Blocks arrive by postMessage, so no SharedArrayBuffer and therefore no
 * cross-origin isolation (COOP/COEP) is required of the embedding page — the same
 * property the VM worker is careful to keep.
 */

/* Ring capacity in frames, a power of two: about 2.6 s at the OPL's rate. Large
 * enough that the control loop never has to fight the buffer's edges. */
const CAPACITY = 1 << 17;

/* Control loop. GAIN converts a relative buffer-depth error into a relative rate
 * correction; SPAN is the largest correction allowed, i.e. how far behind real time
 * the guest may fall before dropouts return (25% covers machines far slower than
 * the one this was written on); SMOOTH is how quickly the ratio may move, and is
 * the reason a correction sounds like a tape machine settling rather than a warble. */
const GAIN = 0.3, SPAN = 0.25, SMOOTH = 0.02;

class VaadoomStream extends AudioWorkletProcessor {
  constructor(options) {
    super();
    const o = (options && options.processorOptions) || {};
    this.srcRate = o.srcRate || 49716;
    /* How much sound to keep in hand. It is latency, so it is as small as the
     * jitter in block arrival allows; the guest delivers in bursts as its timer
     * ticks, not smoothly. */
    this.target = Math.max(256, Math.round(((o.targetMs || 120) / 1000) * this.srcRate));

    this.left = new Float32Array(CAPACITY);
    this.right = new Float32Array(CAPACITY);
    this.write = 0;              // frames written, monotonic
    this.read = 0;               // fractional read position, same numbering
    this.nominal = this.srcRate / sampleRate;
    this.ratio = this.nominal;
    this.priming = true;         // wait for `target` frames before the first sample
    this.underruns = 0;
    this.overruns = 0;
    this.quanta = 0;
    this.holdL = 0;
    this.holdR = 0;

    this.port.onmessage = (e) => {
      const d = e.data;
      if (d.pcm) this.push(d.pcm);
      else if (d.type === 'reset') { this.read = this.write = 0; this.priming = true; }
    };
  }

  /* Interleaved signed-16 stereo in, floats into the ring. */
  push(pcm) {
    const frames = pcm.length >> 1;
    if (!frames) return;
    /* If the guest has run ahead far enough to lap us, drop the oldest sound rather
     * than the newest: what matters is staying close to the present. */
    const over = (this.write - this.read) + frames - CAPACITY;
    if (over > 0) { this.read += over; this.overruns++; }
    for (let i = 0; i < frames; i++) {
      const w = (this.write + i) & (CAPACITY - 1);
      this.left[w] = pcm[2 * i] / 32768;
      this.right[w] = pcm[2 * i + 1] / 32768;
    }
    this.write += frames;
  }

  /* Catmull-Rom, four points. Linear interpolation would cost a couple of dB at
   * the top of the band, which this engine's music can least afford — it is already
   * darker than other DOOM ports. */
  static tap(buf, i, t) {
    const a = buf[(i - 1) & (CAPACITY - 1)], b = buf[i & (CAPACITY - 1)];
    const c = buf[(i + 1) & (CAPACITY - 1)], d = buf[(i + 2) & (CAPACITY - 1)];
    return b + 0.5 * t * (c - a + t * (2 * a - 5 * b + 4 * c - d + t * (3 * (b - c) + d - a)));
  }

  process(_inputs, outputs) {
    const out = outputs[0];
    const L = out[0], R = out[1] || out[0];
    const n = L.length;
    let fill = this.write - this.read;

    /* Priming, and re-priming after an underrun: silence until there is a cushion
     * again, so one late block does not turn into a burst of micro-gaps. */
    if (this.priming) {
      if (fill < this.target) { L.fill(0); R.fill(0); return true; }
      this.priming = false;
    }

    /* Adjust the rate towards whatever keeps the buffer at its target depth. */
    const err = (fill - this.target) / this.target;         // >0: too full, consume faster
    const want = this.nominal * (1 + Math.max(-SPAN, Math.min(SPAN, GAIN * err)));
    this.ratio += (want - this.ratio) * SMOOTH;

    for (let i = 0; i < n; i++) {
      if (this.write - this.read < 3) {                     // ran dry mid-quantum
        this.underruns++;
        this.priming = true;
        for (; i < n; i++) { L[i] = this.holdL; R[i] = this.holdR; }   // hold, do not click
        break;
      }
      const base = Math.floor(this.read);
      const t = this.read - base;
      this.holdL = L[i] = VaadoomStream.tap(this.left, base, t);
      this.holdR = R[i] = VaadoomStream.tap(this.right, base, t);
      this.read += this.ratio;
    }

    /* A second's worth of counters, for whoever is debugging the sound next. */
    if ((this.quanta = (this.quanta + 1) % Math.round(sampleRate / 128)) === 0) {
      this.port.postMessage({
        fillMs: Math.round(((this.write - this.read) / this.srcRate) * 1000),
        speed: +(this.ratio / this.nominal).toFixed(4),   // 1.0 = exactly real time
        underruns: this.underruns,
        overruns: this.overruns,
      });
    }
    return true;
  }
}

registerProcessor('vaadoom-stream', VaadoomStream);
