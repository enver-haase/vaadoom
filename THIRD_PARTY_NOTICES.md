# Third-party notices

Vaadoom's own code (the Flow component, `native/vm.c`, `em_backend.h`, the
`vaadoom-viewport.js` / `vaadoom-worker.js` glue and the compiled
`vaadoom.js` / `vaadoom.wasm`) is licensed under the MIT License — see
[LICENSE](LICENSE).

The add-on additionally **bundles and redistributes** third-party works: the
SUBLEQ virtual machine is derived from Adrian Cable's *cable* / *eternal*, and
the boot image `vmlinux.bootimage.gz` contains a complete Linux system (kernel,
C library, BusyBox) plus the DOOM engine and the DOOM shareware data. That image
is **built from modified sources** (a sound driver, a host-file driver and DOOM
sound backends were added — see the offer at the end), not taken from upstream. Their
licenses and the corresponding-source offer are below. Full GPL/LGPL texts are
shipped under `META-INF/licenses/` in the JAR (and `src/main/resources/META-INF/licenses/`
in the source tree).

## Relationship of the parts (why the GPL does not extend to Vaadoom's own code)

`native/vm.c` is a **CPU emulator**. The GPL/LGPL programs listed below run *as
guest software inside the emulated machine*; Vaadoom does not link against them
and forms no derivative work of them — they are combined only by **mere
aggregation** (running one program under/alongside another). Vaadoom's own code
therefore remains under the MIT License, while each bundled work keeps its own
license, whose terms (including the offer of corresponding source) are honored
below.

---

## 1. cable / Eternal — the SUBLEQ VM (MIT)

Two VM sources derive from Adrian Cable's IOCCC 2025 entry **cable** and the
**eternal** project, <https://github.com/adriancable/eternal>:

* `native/vm_nommu.c` — the **shipped WebAssembly engine** — is derived directly
  from eternal's minimal `vm/vm.c` (the fast NOMMU VM used by the bundled image).
* `native/vm.c` — an MMU-extended, de-obfuscated derivative (via
  <https://github.com/enver-haase/lunatix>), kept for the future MMU-DOOM path.

Both are used under the MIT License below.

```
MIT License

Copyright (c) 2025-2026 Adrian Cable / Eternal Software Initiative

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

## 2. DOOM engine — fbdoom (GPLv2; © id Software)

The boot image runs **fbdoom** (<https://github.com/stoffera/fbdoom>), a
framebuffer port of id Software's DOOM engine, as packaged in
`adriancable/eternal` under `doom/src` and **modified** there to add sound
output (`device/i_snd_sound.c` writing SFX to `/dev/dsp`, `device/i_snd_music.c`
driving `/dev/opl`). The engine source carries:

> Copyright (C) 1993-1996 by id Software, Inc.

id Software released the DOOM engine source code under the **GNU General Public
License, version 2** (1999). The bundled fbdoom binary is distributed under
GPLv2 — see `META-INF/licenses/GPL-2.0.txt`. Corresponding source: the
`doom/src` tree of <https://github.com/adriancable/eternal> and
<https://github.com/stoffera/fbdoom>.

## 3. DOOM shareware data — `doom1.wad` (© id Software, shareware)

The bundled `doom1.wad` is id Software's **DOOM shareware** game data
(Knee-Deep in the Dead). It is **not** covered by the GPL and remains the
property of id Software. It is redistributed unmodified under id Software's
long-standing permission to freely distribute the DOOM shareware. All DOOM
game content is © id Software. DOOM is a trademark of id Software LLC.

## 4. Linux kernel — `arch/subleq` port (GPLv2)

The bundled kernel is Linux with the SUBLEQ architecture port, GPLv2, **with
modifications**: `drivers/char/subleq_sound.c` (the OPL3 + PCM sound card),
`drivers/char/subleq_wad.c` (the host-file device backing `/dev/wad`) and the
matching MMIO helpers under `arch/subleq/kernel/`. Upstream source:
<https://github.com/adriancable/linux> (submodule of `adriancable/eternal`); the
modified source is offered below. See `META-INF/licenses/GPL-2.0.txt`.

## 5. BusyBox (GPLv2)

The image's userland shell/utilities are BusyBox, GPLv2. Source:
<https://github.com/adriancable/busybox> (submodule of `adriancable/eternal`).
See `META-INF/licenses/GPL-2.0.txt`.

## 6. uClibc-ng (LGPLv2.1)

The image's C library is uClibc-ng, LGPLv2.1. Source:
<https://github.com/adriancable/uclibc-ng> (submodule of `adriancable/eternal`).
See `META-INF/licenses/LGPL-2.1.txt`.

---

## Written offer for corresponding source

The shipped `vmlinux.bootimage.gz` is **not** an upstream build: it is produced
from *modified* GPLv2/LGPLv2.1 sources — the Linux kernel gained the
`subleq_sound` and `subleq_wad` drivers, and fbdoom gained its sound backends.

The complete corresponding source for everything in that image (the modified
Linux kernel, the modified DOOM/fbdoom engine, BusyBox and uClibc-ng) is
published at <https://github.com/enver-haase/eternal> and its submodules
(<https://github.com/enver-haase/linux>), which in turn track the upstream
`adriancable/*` repositories linked above. In addition, for three years from
distribution, the maintainer will provide the corresponding source on request —
contact **enver@vaadin.com**.

The image is built from those sources with `build-arch.sh cable-nommu`
(`WITH_SOUND=1`), which ends in `tools/make_boot_image.py`; see the `eternal`
project for the exact build.
