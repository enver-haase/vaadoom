/*
 * opl_shim.h — core-agnostic Yamaha OPL3 FM-synth interface for the lunatix VM.
 *
 * The VM's sound device (em_devices.h here, src/vm.c in lunatix) talks to the FM synthesizer ONLY through these three
 * functions, so the concrete OPL core is a one-file swap. The default backend is Nuked-OPL3
 * (LGPL-2.1), kept as a separable module in third_party/nuked/ and wired up in opl_shim.c;
 * a permissive core (ymfm / Opal) can be dropped in by replacing opl_shim.c alone.
 *
 * All synthesis runs on the HOST at native speed: the guest only writes OPL registers, like
 * DOS DOOM poking a physical SoundBlaster/AdLib. Output is interleaved 16-bit stereo.
 */
#ifndef LUNATIX_OPL_SHIM_H
#define LUNATIX_OPL_SHIM_H

#include <stdint.h>

/* (Re)initialize the chip to emit at `sample_rate` Hz (e.g. 49716, the native OPL3 rate). */
void opl_reset(uint32_t sample_rate);

/* Write one OPL3 register. `reg` is the 9-bit address (bit 8 selects the second bank),
 * `val` the data byte — exactly the (address, data) pair the AdLib/OPL3 hardware takes.
 * Applied immediately; use for same-thread render (boot chime / --soundtest). */
void opl_write(uint16_t reg, uint8_t val);

/* Like opl_write, but the write is queued with sample-accurate timing (Nuked's internal
 * write buffer) and applied at the correct sample position during opl_generate(). Use this
 * for the guest MMIO path, where register writes and audio generation run on different
 * threads: the write lands where the guest issued it in real time (which the guest paces via
 * the RTC), rather than being smeared across whichever generation block happens to run next.
 * Caller must serialize opl_write_buffered() and opl_generate() (they share chip state). */
void opl_write_buffered(uint16_t reg, uint8_t val);

/* Render `frames` stereo frames into `buf` (2*frames int16, L,R interleaved). */
void opl_generate(int16_t *buf, int frames);

#endif /* LUNATIX_OPL_SHIM_H */
