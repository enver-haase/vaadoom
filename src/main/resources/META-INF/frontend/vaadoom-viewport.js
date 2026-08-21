import { LitElement, html, css } from 'lit';

/**
 * <vaadoom-viewport> — the Vaadoom DOOM viewport.
 *
 * Boots the bundled NOMMU-Linux + SUBLEQ-VM WebAssembly engine in a Web Worker,
 * which auto-launches fbdoom and paints its 800x512 framebuffer onto an
 * OffscreenCanvas transferred to that worker.
 *
 * Keyboard input: when the component has focus, key events are mapped to USB HID
 * scancodes (which the subleq keyboard driver understands: +code = down, -code =
 * up) and posted to the worker. Because the worker runs the VM in bounded slices
 * and yields between them, it processes these messages without any SharedArrayBuffer
 * — so no cross-origin-isolation (COOP/COEP) headers are required.
 *
 * Assets are served raw from META-INF/resources/vaadoom/ (i.e. `<context>/vaadoom/…`).
 */
class VaadoomViewport extends LitElement {
  static get properties() {
    return {
      autostart: { type: Boolean },
      playable: { type: Boolean },
      wadUrl: { type: String },
      sound: { type: Boolean },
      musicGain: { type: Number },
      _phase: { state: true },
      _error: { state: true },
      _focused: { state: true },
    };
  }

  static FB_W = 800;
  static FB_H = 512;

  // KeyboardEvent.code -> USB HID usage id (page 0x07) == SDL scancode.
  static KEYMAP = (() => {
    const m = {
      ArrowRight: 79, ArrowLeft: 80, ArrowDown: 81, ArrowUp: 82,
      ControlLeft: 224, ControlRight: 228,
      AltLeft: 226, AltRight: 230,
      ShiftLeft: 225, ShiftRight: 229,
      Space: 44, Enter: 40, Escape: 41, Tab: 43, Backspace: 42,
      Minus: 45, Equal: 46, Comma: 54, Period: 55, Slash: 56, Backquote: 53,
    };
    for (let i = 0; i < 26; i++) m['Key' + String.fromCharCode(65 + i)] = 4 + i; // KeyA..KeyZ
    m.Digit0 = 39;
    for (let i = 1; i <= 9; i++) m['Digit' + i] = 29 + i;                        // Digit1..9 -> 30..38
    for (let i = 1; i <= 12; i++) m['F' + i] = 57 + i;                           // F1..F12 -> HID 58..69
    return m;
  })();

  // Keys we swallow so the browser doesn't scroll / reload / go fullscreen /
  // open devtools while the game has focus.
  static SWALLOW = (() => {
    const s = new Set([
      'ArrowUp', 'ArrowDown', 'ArrowLeft', 'ArrowRight', 'Space', 'Tab',
      'ControlLeft', 'ControlRight', 'AltLeft', 'AltRight',
    ]);
    for (let i = 1; i <= 12; i++) s.add('F' + i);                                // F1..F12 are DOOM functions
    return s;
  })();

  static get styles() {
    return css`
      :host { display: block; position: relative; background: #000; overflow: hidden; outline: none; }
      :host(:focus-visible) { box-shadow: 0 0 0 2px #e33 inset; }
      canvas { display: block; width: 100%; height: 100%; image-rendering: pixelated; image-rendering: crisp-edges; }
      .overlay {
        position: absolute; inset: 0; display: flex; flex-direction: column;
        align-items: center; justify-content: center; gap: 14px;
        color: #d33; font: 600 15px/1.4 monospace; letter-spacing: .05em;
        background: radial-gradient(circle at 50% 40%, #1a0000 0%, #000 70%);
        text-align: center; padding: 16px;
      }
      .overlay[hidden] { display: none; }
      .title { color: #e33; font-size: 34px; font-weight: 800; text-shadow: 0 0 18px #900; font-family: Georgia, serif; }
      .msg { color: #caa; font-weight: 400; }
      .spin { width: 26px; height: 26px; border: 3px solid #400; border-top-color: #e33; border-radius: 50%; animation: s 1s linear infinite; }
      @keyframes s { to { transform: rotate(360deg); } }
      .err { color: #f66; max-width: 90%; white-space: pre-wrap; font-weight: 400; }
      /* click-to-play hint, shown while running but not focused */
      .hint {
        position: absolute; left: 0; right: 0; bottom: 0; padding: 8px 10px;
        font: 12px/1.3 monospace; color: #eee; text-align: center;
        background: linear-gradient(transparent, rgba(0,0,0,.75));
        pointer-events: none; user-select: none;
      }
      .hint[hidden] { display: none; }
      .hint b { color: #e77; }
    `;
  }

  constructor() {
    super();
    this.autostart = true;
    this.playable = true;
    this.wadUrl = null;
    this.sound = true;
    this.musicGain = 0.6;
    this._phase = 'idle';
    this._error = null;
    this._focused = false;
    this._worker = null;
    this._started = false;
    this._down = new Set();          // currently-pressed codes (for repeat suppression)
    this._audioCtx = null;           // created on the first block (see _playBlock)
    this._musicNode = null;          // gain stage for the music stream
    this._masterNode = null;         // master bus (limiter) both streams meet at
    this._audioAt = { sfx: 0, music: 0 };   // per-stream cursors, in AudioContext time
    this._onKeyDown = this._onKeyDown.bind(this);
    this._onKeyUp = this._onKeyUp.bind(this);
  }

  render() {
    const running = this._phase === 'running' && !this._error;
    return html`
      <canvas width="${VaadoomViewport.FB_W}" height="${VaadoomViewport.FB_H}"></canvas>
      <div class="overlay" ?hidden=${running}>
        <div class="title">VAADOOM</div>
        ${this._error
          ? html`<div class="err">⚠ ${this._error}</div>`
          : html`<div class="spin"></div><div class="msg">${this._phaseLabel()}</div>`}
      </div>
      <div class="hint" ?hidden=${!running || !this.playable || this._focused}>
        Click to play — <b>arrows</b> move · <b>Ctrl</b> fire · <b>Space</b> use · <b>Alt</b> strafe · <b>1–7</b> weapons
      </div>
    `;
  }

  _phaseLabel() {
    switch (this._phase) {
      case 'idle': return 'ready';
      case 'loading-engine': return 'loading engine…';
      case 'loading-wad': return 'loading WAD…';
      case 'loading-image': return 'loading Linux + DOOM image…';
      case 'booting': return 'booting Eternal Linux…';
      case 'running': return 'running';
      case 'halted': return 'engine halted';
      default: return this._phase;
    }
  }

  firstUpdated() {
    if (this.playable) {
      this.tabIndex = 0;                                  // focusable
      this.addEventListener('keydown', this._onKeyDown);
      this.addEventListener('keyup', this._onKeyUp);
      this.addEventListener('focus', () => (this._focused = true));
      this.addEventListener('blur', () => { this._focused = false; this._releaseAll(); });
      this.addEventListener('pointerdown', () => { this.focus(); this._ensureAudio(); });
    }
    if (this.autostart) this.start();
  }

  disconnectedCallback() {
    super.disconnectedCallback();
    if (this._worker) { this._worker.terminate(); this._worker = null; }
    if (this._audioCtx) { this._audioCtx.close().catch(() => {}); this._audioCtx = null; }
    this._musicNode = null;
    this._masterNode = null;
  }

  _onKeyDown(e) {
    const hid = VaadoomViewport.KEYMAP[e.code];
    if (!hid) return;
    if (VaadoomViewport.SWALLOW.has(e.code)) e.preventDefault();
    if (this._down.has(e.code)) return;                  // ignore auto-repeat
    this._down.add(e.code);
    this._sendKey(hid);                                  // +code = keydown
  }

  _onKeyUp(e) {
    const hid = VaadoomViewport.KEYMAP[e.code];
    if (!hid) return;
    if (VaadoomViewport.SWALLOW.has(e.code)) e.preventDefault();
    this._down.delete(e.code);
    this._sendKey(-hid);                                 // -code = keyup
  }

  _releaseAll() {
    for (const code of this._down) {
      const hid = VaadoomViewport.KEYMAP[code];
      if (hid) this._sendKey(-hid);
    }
    this._down.clear();
  }

  _sendKey(code) {
    if (this._worker) this._worker.postMessage({ type: 'key', code });
  }

  /** Diagnostic: start/stop mirroring the guest's OPL register writes, and pull the
   * captured log into `_oplLog`. Handy when music sounds wrong — it shows whether the
   * guest is keying notes at all, and how often. */
  traceOpl(on = true, dump = 0) {
    if (this._worker) this._worker.postMessage({ type: 'opltrace', on, dump });
  }

  /* Play one block of interleaved S16 stereo frames on one of the two audio
   * streams: 'sfx' (the guest's PCM ring) or 'music' (the OPL3 chip, rendered
   * host-side at its native rate). Each stream keeps its own cursor and they are
   * simply played together — the same arrangement the native build gets from two
   * SDL audio streams. Blocks are queued back-to-back on the AudioContext clock;
   * if the emulator falls behind and a queue runs dry we resync with a small lead,
   * so a hiccup costs one gap instead of permanent drift.
   *
   * Blocks that arrive before the context is running are DROPPED, not queued: a
   * suspended context's currentTime does not advance, so scheduling into it would
   * pile up every block produced during the ~15s boot and play them all — minutes
   * behind — once the first click resumes it. Sound therefore starts at the moment
   * the user interacts with the page, which is also the only moment browsers allow. */
  _playBlock(stream, pcm, rate) {
    if (!this.sound) return;
    const ctx = this._ensureAudio();          // also retries resume() after a gesture
    if (!ctx || ctx.state !== 'running') return;
    const frames = pcm.length >> 1;
    if (!frames) return;
    const buf = ctx.createBuffer(2, frames, rate);
    const left = buf.getChannelData(0), right = buf.getChannelData(1);
    for (let i = 0; i < frames; i++) {
      left[i] = pcm[2 * i] / 32768;
      right[i] = pcm[2 * i + 1] / 32768;
    }
    const src = ctx.createBufferSource();
    src.buffer = buf;
    /* Music goes through a gain stage. An OPL voice is quiet by nature — one 2-op
     * voice at zero attenuation peaks around -18 dBFS in the chip's own output —
     * and DOOM's music player attenuates it further, so the raw stream sits around
     * -50 dBFS: audible only with the speakers wide open. The boost is applied here,
     * in the float domain, where only the final device clamps. */
    src.connect(stream === 'music' ? this._musicGainNode(ctx) : this._master(ctx));
    const now = ctx.currentTime;
    // Resync when the queue has run dry (emulator fell behind) or drifted too far
    // ahead (it ran faster than real time); otherwise keep blocks seamless.
    if (this._audioAt[stream] < now + 0.02 || this._audioAt[stream] > now + 0.5)
      this._audioAt[stream] = now + 0.08;
    src.start(this._audioAt[stream]);
    this._audioAt[stream] += buf.duration;
  }

  /* Creates the AudioContext and keeps trying to resume it. Browsers start it
   * suspended until the page has seen a user gesture, which is why this is called
   * both from the click that starts play and from every arriving SFX block —
   * whichever comes first wins, and sound recovers on its own afterwards. */
  /* Both streams meet at a master bus with a limiter on it. They are mixed by
   * simply being played together, so their peaks can coincide — and how loud
   * either one is depends on the WAD, on DOOM's own volume settings and on
   * musicGain. The limiter is a safety net for that, not an effect: it sits just
   * below full scale and only acts when a sum would otherwise clip. */
  _master(ctx) {
    if (!this._masterNode) {
      const limiter = ctx.createDynamicsCompressor();
      limiter.threshold.value = -3;      // dBFS
      limiter.knee.value = 0;
      limiter.ratio.value = 20;
      limiter.attack.value = 0.003;
      limiter.release.value = 0.25;
      limiter.connect(ctx.destination);
      this._masterNode = limiter;
    }
    return this._masterNode;
  }

  /* Music is measurably louder than the sound effects at equal gain — it plays
   * continuously where effects are sparse — so it is attenuated to sit under
   * them. Measured on the shareware WAD: effects average -38 dBFS, music -28.5
   * before this stage. */
  _musicGainNode(ctx) {
    if (!this._musicNode) {
      this._musicNode = ctx.createGain();
      this._musicNode.connect(this._master(ctx));
    }
    this._musicNode.gain.value = this.musicGain;
    return this._musicNode;
  }

  _ensureAudio() {
    if (!this.sound) return null;
    if (!this._audioCtx) {
      const Ctx = window.AudioContext || window.webkitAudioContext;
      if (!Ctx) return null;
      this._audioCtx = new Ctx();
      this._audioAt = { sfx: 0, music: 0 };
    }
    if (this._audioCtx.state === 'suspended') this._audioCtx.resume().catch(() => {});
    return this._audioCtx;
  }

  /** Boots the engine. Safe to call once; ignored if already started. */
  start() {
    if (this._started) return;
    this._started = true;
    this._error = null;
    this._phase = 'loading-engine';

    const canvas = this.renderRoot.querySelector('canvas');
    let offscreen;
    try {
      offscreen = canvas.transferControlToOffscreen();
    } catch (e) {
      this._error = 'OffscreenCanvas not supported by this browser.';
      return;
    }

    const workerUrl = new URL('vaadoom/vaadoom-worker.js', document.baseURI);
    let worker;
    try {
      worker = new Worker(workerUrl, { type: 'module' });
    } catch (e) {
      this._error = 'Could not start engine worker: ' + e;
      return;
    }
    this._worker = worker;

    worker.onmessage = (ev) => {
      const m = ev.data;
      if (m.type === 'status') {
        this._phase = m.phase;
        if (m.phase === 'running' && this.playable) this.focus();  // grab keyboard when DOOM starts
      } else if (m.type === 'pcm') {
        this._playBlock('sfx', m.pcm, m.rate);
      } else if (m.type === 'opl') {
        this._playBlock('music', m.pcm, m.rate);
      } else if (m.type === 'opltrace') {
        this._oplLog = m.log;      // diagnostic, populated on request (see traceOpl())
      } else if (m.type === 'stats') {
        this._stats = m;   // engine counters; handy when debugging a WAD or sound
      } else if (m.type === 'wad') {
        // Non-fatal: without a usable WAD the guest falls back to the bundled one.
        this._wad = m;
        if (m.ok) console.info(`vaadoom: WAD loaded (${m.bytes} bytes, served to DOOM as ${m.name})`);
        else console.warn('vaadoom: ' + m.message);
      } else if (m.type === 'error') {
        this._error = m.message;
      }
    };
    worker.onerror = (ev) => {
      this._error = 'Engine worker error: ' + (ev.message || ev.type);
    };

    worker.postMessage(
      { type: 'start', canvas: offscreen, config: { wadUrl: this.wadUrl, sound: this.sound } },
      [offscreen]);
  }
}

customElements.define('vaadoom-viewport', VaadoomViewport);
