# Vaadoom

**DOOM, as a Vaadin Flow component.**

Vaadoom renders a DOOM viewport inside an ordinary [Vaadin](https://vaadin.com) Flow
`Component`. Under the hood DOOM runs on a NOMMU Linux that itself runs on Adrian Cable's
single-instruction **SUBLEQ** virtual machine — compiled to WebAssembly and driven from a
Web Worker that paints the emulated framebuffer onto an `OffscreenCanvas`.

```java
Vaadoom doom = new Vaadoom();
add(doom);
```

That's it. The component is self-contained: the WebAssembly engine and the compressed
Linux+DOOM boot image ship inside the add-on JAR, so there is nothing to host or fetch.

![DOOM running inside a Vaadin Flow component](docs/img/doom-in-vaadin.png)

## How it works

```
<vaadoom-viewport> (Lit)
   │ transferControlToOffscreen()
   ▼
Web Worker ── vaadoom.js + vaadoom.wasm   (the SUBLEQ VM)
   • decompress the bundled image → MEMFS /boot.img   (DecompressionStream, gzip)
   • boot NOMMU "Eternal" Linux, auto-launch fbdoom (injected keystrokes)
   • run the VM in bounded slices; paint each frame to the OffscreenCanvas
```

Render-only (the DOOM attract/demo loop). It does **not** use `SharedArrayBuffer`, so the
consuming app needs **no** `COOP`/`COEP` cross-origin-isolation headers.

<p float="left">
  <img src="docs/img/eternal-linux-boot.png" width="380" alt="Eternal Linux booting"/>
  <img src="docs/img/doom-title.png" width="380" alt="DOOM title screen"/>
</p>

## Requirements

- Vaadin **25.2+**, Java **21+**
- A browser with `OffscreenCanvas`, `WebAssembly` and `DecompressionStream` (current
  Chrome/Edge/Firefox/Safari)
- First paint of DOOM takes a few tens of seconds (boot + launch) behind a loading
  overlay; the engine uses up to ~1.5 GB of WebAssembly memory.

## Usage

```xml
<dependency>
    <groupId>org.vaadin.addons.enverhaase</groupId>
    <artifactId>vaadoom</artifactId>
    <version>0.2.0</version>
</dependency>
```

```java
import org.vaadin.addons.enverhaase.vaadoom.Vaadoom;

Vaadoom doom = new Vaadoom();  // sized to the native 800x512 framebuffer
doom.setWidth("100%");          // or scale it — HasSize
add(doom);
```

## Building

```bash
./native/build.sh wasm     # compile the SUBLEQ VM to WebAssembly (needs emsdk)
./native/build.sh image    # fetch + repack the Linux+DOOM boot image (~38MB gz)
./native/build.sh native   # optional: the unchanged native SDL3 binary (needs SDL3)
mvn                        # run the demo view at http://localhost:8080/
mvn clean install -Pdirectory   # build the Vaadin Directory .zip in target/
```

The `native/` source is one file (`vm.c`) with a `#ifdef __EMSCRIPTEN__` seam, so the
same source builds both the browser engine and the original native SDL3 binary.

## Credits & license

MIT licensed. The SUBLEQ VM is derived from Adrian Cable's IOCCC 2025 *cable* entry and
the [`eternal`](https://github.com/adriancable/eternal) project (MIT). DOOM is **fbdoom**
with the id Software shareware `doom1.wad`. See [LICENSE](LICENSE).
