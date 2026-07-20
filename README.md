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

## Status

| Version | What it does |
|---------|--------------|
| **0.1.0** | Scaffolding release — renders an animated **placeholder** test pattern. Verifies the Flow integration and Directory packaging. |
| 0.2.0 *(planned)* | Real WebAssembly SUBLEQ/DOOM engine, render-only (attract/demo loop). |
| later | Keyboard-playable DOOM. |

## Requirements

- Vaadin **25.2+**
- Java **21+**
- A modern browser with `OffscreenCanvas` and `WebAssembly` support.

## Usage

Add the dependency (from the [Vaadin Directory](https://vaadin.com/directory) /
`vaadin-addons` repository):

```xml
<dependency>
    <groupId>org.vaadin.addons.enverhaase</groupId>
    <artifactId>vaadoom</artifactId>
    <version>0.1.0</version>
</dependency>
```

```java
import org.vaadin.addons.enverhaase.vaadoom.Vaadoom;

Vaadoom doom = new Vaadoom();  // sized to the native 800x512 framebuffer
doom.setWidth("100%");         // or scale it — HasSize
add(doom);
```

## Building the add-on

```bash
mvn                       # run the demo view at http://localhost:8080/
mvn clean install         # build the add-on JAR
mvn clean install -Pdirectory   # build the Vaadin Directory .zip in target/
```

## Credits & license

MIT licensed. The SUBLEQ VM is derived from Adrian Cable's IOCCC 2025 *cable* entry and
the [`eternal`](https://github.com/adriancable/eternal) project (MIT). DOOM is
[fbdoom](https://github.com/ozkl/doomgeneric) / the id Software shareware `doom1.wad`.
See [LICENSE](LICENSE).
