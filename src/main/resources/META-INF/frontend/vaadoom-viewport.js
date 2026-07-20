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
    return m;
  })();

  // Keys we swallow so the browser doesn't scroll / change focus during play.
  static SWALLOW = new Set([
    'ArrowUp', 'ArrowDown', 'ArrowLeft', 'ArrowRight', 'Space', 'Tab',
    'ControlLeft', 'ControlRight', 'AltLeft', 'AltRight',
  ]);

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
    this._phase = 'idle';
    this._error = null;
    this._focused = false;
    this._worker = null;
    this._started = false;
    this._down = new Set();          // currently-pressed codes (for repeat suppression)
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
      this.addEventListener('pointerdown', () => this.focus());
    }
    if (this.autostart) this.start();
  }

  disconnectedCallback() {
    super.disconnectedCallback();
    if (this._worker) { this._worker.terminate(); this._worker = null; }
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
      } else if (m.type === 'error') {
        this._error = m.message;
      }
    };
    worker.onerror = (ev) => {
      this._error = 'Engine worker error: ' + (ev.message || ev.type);
    };

    worker.postMessage({ type: 'start', canvas: offscreen }, [offscreen]);
  }
}

customElements.define('vaadoom-viewport', VaadoomViewport);
