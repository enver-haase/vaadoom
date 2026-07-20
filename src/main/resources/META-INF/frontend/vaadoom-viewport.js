import { LitElement, html, css } from 'lit';

/**
 * <vaadoom-viewport> — the Vaadoom DOOM viewport.
 *
 * Boots the bundled NOMMU-Linux + SUBLEQ-VM WebAssembly engine in a Web Worker,
 * which auto-launches fbdoom and paints its 800x512 framebuffer onto an
 * OffscreenCanvas transferred to that worker. Render-only (attract/demo loop);
 * no SharedArrayBuffer, so no cross-origin-isolation headers are required.
 *
 * The engine assets (vaadoom.js / vaadoom.wasm / vmlinux.bootimage.gz /
 * vaadoom-worker.js) are served raw from META-INF/resources/vaadoom/, i.e. at
 * `<context>/vaadoom/...`, resolved here against document.baseURI.
 */
class VaadoomViewport extends LitElement {
  static get properties() {
    return {
      autostart: { type: Boolean },
      _phase: { state: true },
      _error: { state: true },
    };
  }

  static FB_W = 800;
  static FB_H = 512;

  static get styles() {
    return css`
      :host { display: block; position: relative; background: #000; overflow: hidden; }
      canvas {
        display: block; width: 100%; height: 100%;
        image-rendering: pixelated; image-rendering: crisp-edges;
      }
      .overlay {
        position: absolute; inset: 0; display: flex; flex-direction: column;
        align-items: center; justify-content: center; gap: 14px;
        color: #d33; font: 600 15px/1.4 monospace; letter-spacing: .05em;
        background: radial-gradient(circle at 50% 40%, #1a0000 0%, #000 70%);
        text-align: center; padding: 16px;
      }
      .overlay[hidden] { display: none; }
      .title { color: #e33; font-size: 34px; font-weight: 800;
        text-shadow: 0 0 18px #900; font-family: Georgia, serif; }
      .msg { color: #caa; font-weight: 400; }
      .spin { width: 26px; height: 26px; border: 3px solid #400;
        border-top-color: #e33; border-radius: 50%; animation: s 1s linear infinite; }
      @keyframes s { to { transform: rotate(360deg); } }
      .err { color: #f66; max-width: 90%; white-space: pre-wrap; font-weight: 400; }
    `;
  }

  constructor() {
    super();
    this.autostart = true;
    this._phase = 'idle';
    this._error = null;
    this._worker = null;
    this._started = false;
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
    if (this.autostart) this.start();
  }

  disconnectedCallback() {
    super.disconnectedCallback();
    if (this._worker) { this._worker.terminate(); this._worker = null; }
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
      if (m.type === 'status') this._phase = m.phase;
      else if (m.type === 'error') this._error = m.message;
    };
    worker.onerror = (ev) => {
      this._error = 'Engine worker error: ' + (ev.message || ev.type);
    };

    worker.postMessage({ type: 'start', canvas: offscreen }, [offscreen]);
  }
}

customElements.define('vaadoom-viewport', VaadoomViewport);
