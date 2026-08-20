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
 *
 * <h2>The IWAD</h2>
 * The game data comes from a WAD the browser fetches at start-up:
 * <pre>
 * add(new Vaadoom(Vaadoom.SHAREWARE_WAD));
 * </pre>
 * {@link #SHAREWARE_WAD} is id Software's freely redistributable shareware
 * {@code doom1.wad} (episode 1, "Knee-Deep in the Dead"). Any other IWAD works too:
 * the engine reads the lump directory and presents the file to DOOM under the name
 * that selects the matching game mode ({@code doom2.wad}, {@code doomu.wad},
 * {@code doom.wad}, {@code doom1.wad}), so the full games play as they should.
 * <p>
 * <b>Where those full IWADs come from is up to you.</b> The commercial WADs are not
 * freely redistributable and this add-on deliberately ships no links to copies of
 * them &mdash; if you own DOOM, The Ultimate DOOM, DOOM II or Final DOOM, put the
 * {@code .wad} from your own copy on your own server and pass that URL. Serving it
 * from your own Vaadin application needs no CORS setup at all; a cross-origin URL
 * must send {@code Access-Control-Allow-Origin}, since the worker uses {@code fetch()}.
 * <p>
 * The WAD is handed to the guest through a host-file device: the bytes stay in the
 * host's memory and are copied straight into guest RAM on demand, so the (slow)
 * emulated CPU never moves the file itself. If the fetch fails, DOOM falls back to
 * the shareware WAD contained in the boot image.
 *
 * <h2>Playing</h2>
 * The DOOM framebuffer is {@value #FB_WIDTH}&times;{@value #FB_HEIGHT}. By default the
 * component is sized to those pixels; use {@link #setWidth(String)} /
 * {@link #setHeight(String)} (from {@link HasSize}) to scale the viewport.
 * <p>
 * The engine boots a NOMMU Linux and launches fbdoom; first paint takes ~15 seconds (a
 * loading overlay is shown meanwhile). It is <b>playable</b>: when the component has
 * focus it forwards keyboard input to DOOM (arrows move, Ctrl fires, Space uses, Alt
 * strafes, 1&ndash;7 select weapons). Disable input with {@link #setPlayable(boolean)}.
 * Sound effects are played through the Web Audio API (see {@link #setSound(boolean)});
 * the browser only starts them once the user has interacted with the page.
 * <p>
 * Needs a browser with {@code OffscreenCanvas}, {@code WebAssembly} and
 * {@code DecompressionStream}; <b>no</b> cross-origin-isolation (COOP/COEP) headers are
 * required (input is delivered between VM slices, without SharedArrayBuffer).
 */
@Tag("vaadoom-viewport")
@JsModule("./vaadoom-viewport.js")
public class Vaadoom extends Component implements HasSize {

    /** Native width, in pixels, of the DOOM framebuffer. */
    public static final int FB_WIDTH = 800;
    /** Native height, in pixels, of the DOOM framebuffer. */
    public static final int FB_HEIGHT = 512;

    /**
     * id Software's shareware {@code doom1.wad} (v1.9, 4,196,020 bytes) on the Internet
     * Archive, served with CORS headers. This is the episode id Software released for
     * free distribution, so it is the one this add-on points at.
     */
    public static final String SHAREWARE_WAD =
            "https://archive.org/cors/doom-shareware_1996/DOOM1.WAD";

    /**
     * Creates a Vaadoom viewport that plays the WAD at the given URL, sized to the
     * native DOOM framebuffer ({@value #FB_WIDTH}&times;{@value #FB_HEIGHT}).
     *
     * @param wadUrl URL of the IWAD to play, e.g. {@link #SHAREWARE_WAD}, or a WAD you
     *               host yourself; {@code null} to use the shareware WAD bundled in the
     *               boot image. Cross-origin URLs must allow CORS.
     */
    public Vaadoom(String wadUrl) {
        setWidth(FB_WIDTH + "px");
        setHeight(FB_HEIGHT + "px");
        setAutostart(true);
        setWadUrl(wadUrl);
    }

    /**
     * Creates a Vaadoom viewport that plays the shareware WAD bundled in the boot image
     * (no download). Equivalent to {@code new Vaadoom(null)}.
     */
    public Vaadoom() {
        this(null);
    }

    /**
     * Sets the URL of the WAD to fetch and play. Takes effect on the next attach.
     *
     * @param wadUrl URL of the IWAD, or {@code null} for the bundled shareware WAD
     */
    public void setWadUrl(String wadUrl) {
        getElement().setProperty("wadUrl", wadUrl);
    }

    /**
     * @return the WAD URL, or {@code null} if the bundled shareware WAD is used
     */
    public String getWadUrl() {
        return getElement().getProperty("wadUrl", null);
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

    /**
     * Whether keyboard input is enabled (the viewport becomes focusable and
     * forwards key events to DOOM). Defaults to {@code true}. Input works without
     * any cross-origin-isolation headers.
     *
     * @param playable {@code true} to enable keyboard control
     */
    public void setPlayable(boolean playable) {
        getElement().setProperty("playable", playable);
    }

    /**
     * @return whether keyboard input is enabled
     */
    public boolean isPlayable() {
        return getElement().getProperty("playable", true);
    }

    /**
     * Whether DOOM's sound effects are played. Defaults to {@code true}. The guest
     * produces PCM frames which the VM's sound card hands to the page's
     * {@code AudioContext}; browsers keep that context suspended until the user
     * interacts with the page, so the first sounds arrive after the first click.
     *
     * @param sound {@code true} to play sound effects
     */
    public void setSound(boolean sound) {
        getElement().setProperty("sound", sound);
    }

    /**
     * @return whether sound effects are played
     */
    public boolean isSound() {
        return getElement().getProperty("sound", true);
    }

    /**
     * Playback gain applied to the OPL3 music stream, defaulting to {@code 0.4}.
     * <p>
     * Music plays continuously where sound effects are sparse, so at equal gain it
     * dominates: measured on the shareware WAD, the effects average
     * &minus;38&nbsp;dBFS against the music's &minus;28.5&nbsp;dBFS. The default
     * attenuates the music to sit under them. Both streams then pass a limiter just
     * below full scale, so raising this cannot make the mix clip &mdash; only louder.
     *
     * @param gain linear gain for the music stream ({@code 1} = as the chip emits it)
     */
    public void setMusicGain(double gain) {
        getElement().setProperty("musicGain", gain);
    }

    /**
     * @return the playback gain applied to the music stream
     */
    public double getMusicGain() {
        return getElement().getProperty("musicGain", 0.4d);
    }
}
