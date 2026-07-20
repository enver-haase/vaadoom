import { LitElement, html, css } from 'lit';

/**
 * <vaadoom-viewport> — Vaadoom DOOM viewport.
 *
 * 0.1.0 PLACEHOLDER: draws an animated test pattern at the native DOOM
 * framebuffer resolution (800x512) so the Flow component, packaging and
 * Directory publishing pipeline can be verified before the WebAssembly
 * SUBLEQ/DOOM engine is wired in (Stage 4).
 */
class VaadoomViewport extends LitElement {
  static get properties() {
    return {
      autostart: { type: Boolean },
    };
  }

  static get styles() {
    return css`
      :host {
        display: block;
        position: relative;
        background: #000;
        overflow: hidden;
      }
      canvas {
        display: block;
        width: 100%;
        height: 100%;
        image-rendering: pixelated;
      }
      .badge {
        position: absolute;
        left: 8px;
        bottom: 6px;
        font: 11px/1.2 monospace;
        color: #9c9;
        opacity: 0.7;
        pointer-events: none;
        user-select: none;
      }
    `;
  }

  // DOOM native framebuffer size (matches Vaadoom.FB_WIDTH/FB_HEIGHT).
  static FB_W = 800;
  static FB_H = 512;

  constructor() {
    super();
    this.autostart = true;
    this._raf = 0;
    this._t = 0;
  }

  render() {
    return html`
      <canvas
        width="${VaadoomViewport.FB_W}"
        height="${VaadoomViewport.FB_H}"
      ></canvas>
      <span class="badge">vaadoom 0.1.0 · placeholder</span>
    `;
  }

  firstUpdated() {
    this._canvas = this.renderRoot.querySelector('canvas');
    this._ctx = this._canvas.getContext('2d');
    if (this.autostart) {
      this.start();
    } else {
      this._drawFrame(0); // draw one static frame
    }
  }

  disconnectedCallback() {
    super.disconnectedCallback();
    this.stop();
  }

  start() {
    if (this._raf) return;
    const loop = () => {
      this._t += 1;
      this._drawFrame(this._t);
      this._raf = requestAnimationFrame(loop);
    };
    this._raf = requestAnimationFrame(loop);
  }

  stop() {
    if (this._raf) {
      cancelAnimationFrame(this._raf);
      this._raf = 0;
    }
  }

  _drawFrame(t) {
    const ctx = this._ctx;
    if (!ctx) return;
    const w = VaadoomViewport.FB_W;
    const h = VaadoomViewport.FB_H;

    // Animated plasma-ish background as a stand-in for the DOOM framebuffer.
    const g = ctx.createLinearGradient(0, 0, w, h);
    const p = (t % 360) / 360;
    g.addColorStop(0, `hsl(${(p * 360) | 0}, 60%, 12%)`);
    g.addColorStop(0.5, `hsl(${((p * 360) + 40) % 360 | 0}, 70%, 22%)`);
    g.addColorStop(1, `hsl(${((p * 360) + 80) % 360 | 0}, 60%, 10%)`);
    ctx.fillStyle = g;
    ctx.fillRect(0, 0, w, h);

    // Scanlines for a CRT-ish feel.
    ctx.fillStyle = 'rgba(0,0,0,0.18)';
    for (let y = 0; y < h; y += 3) {
      ctx.fillRect(0, y, w, 1);
    }

    // Title.
    ctx.save();
    ctx.textAlign = 'center';
    ctx.textBaseline = 'middle';
    ctx.fillStyle = '#e33';
    ctx.font = 'bold 96px Georgia, serif';
    ctx.shadowColor = '#600';
    ctx.shadowBlur = 24;
    ctx.fillText('VAADOOM', w / 2, h / 2 - 20);
    ctx.shadowBlur = 0;
    ctx.fillStyle = '#cbb';
    ctx.font = '20px monospace';
    ctx.fillText('SUBLEQ + WASM viewport — engine wiring in progress', w / 2, h / 2 + 60);
    ctx.restore();
  }
}

customElements.define('vaadoom-viewport', VaadoomViewport);
