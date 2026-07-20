package org.vaadin.addons.enverhaase.vaadoom;

import com.vaadin.flow.component.Component;
import com.vaadin.flow.component.HasSize;
import com.vaadin.flow.component.Tag;
import com.vaadin.flow.component.dependency.JsModule;

/**
 * Vaadoom &mdash; a Flow component that renders a DOOM viewport.
 * <p>
 * DOOM runs inside a NOMMU Linux which itself runs on Adrian Cable's single-instruction
 * SUBLEQ virtual machine, compiled to WebAssembly and driven from a Web Worker. The
 * worker paints the emulated framebuffer onto an {@code OffscreenCanvas} inside the
 * {@code <vaadoom-viewport>} custom element.
 * <p>
 * The DOOM framebuffer is {@value #FB_WIDTH}&times;{@value #FB_HEIGHT}. By default the
 * component is sized to those pixels; use {@link #setWidth(String)} /
 * {@link #setHeight(String)} (from {@link HasSize}) to scale the viewport.
 *
 * <p><b>Note:</b> this 0.1.0 release ships a placeholder renderer (an animated test
 * pattern) so the Directory packaging and Flow integration can be verified before the
 * WebAssembly DOOM engine is wired in.
 */
@Tag("vaadoom-viewport")
@JsModule("./vaadoom-viewport.js")
public class Vaadoom extends Component implements HasSize {

    /** Native width, in pixels, of the DOOM framebuffer. */
    public static final int FB_WIDTH = 800;
    /** Native height, in pixels, of the DOOM framebuffer. */
    public static final int FB_HEIGHT = 512;

    /**
     * Creates a Vaadoom viewport sized to the native DOOM framebuffer
     * ({@value #FB_WIDTH}&times;{@value #FB_HEIGHT}).
     */
    public Vaadoom() {
        setWidth(FB_WIDTH + "px");
        setHeight(FB_HEIGHT + "px");
        setAutostart(true);
    }

    /**
     * Whether the emulator starts automatically when the component is attached.
     * Defaults to {@code true}.
     *
     * @param autostart {@code true} to start on attach
     */
    public void setAutostart(boolean autostart) {
        getElement().setProperty("autostart", autostart);
    }

    /**
     * @return whether the emulator starts automatically on attach
     */
    public boolean isAutostart() {
        return getElement().getProperty("autostart", true);
    }
}
