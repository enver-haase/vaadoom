/*
 * opl_shim.c — Nuked-OPL3 backend for opl_shim.h. Vendored from lunatix.
 *
 * This is the ONLY translation unit that knows about Nuked-OPL3, keeping the LGPL core
 * (third_party/nuked/opl3.c) a separable, relinkable module: src/vm.c includes only
 * opl_shim.h. To swap cores (ymfm, Opal, …), replace this file and the vendored core;
 * the three-function contract in opl_shim.h stays put.
 *
 * Two write entry points: opl_write() applies immediately (OPL3_WriteReg) for same-thread
 * render (boot chime / --soundtest); opl_write_buffered() queues with sample-accurate timing
 * (OPL3_WriteRegBuffered) for the guest MMIO path, where writes and generation run on
 * different threads. Nuked's write buffer stamps each queued write against the generation
 * sample counter, so a guest note lands where it was issued in real time. In Vaadoom both run on the
 * worker thread, so no locking is needed; the sample-accurate stamping is what matters.
 */
#include "opl_shim.h"
#include "third_party/nuked/opl3.h"

static opl3_chip chip;

void opl_reset(uint32_t sample_rate) {
    OPL3_Reset(&chip, sample_rate);
}

void opl_write(uint16_t reg, uint8_t val) {
    OPL3_WriteReg(&chip, reg, val);
}

void opl_write_buffered(uint16_t reg, uint8_t val) {
    OPL3_WriteRegBuffered(&chip, reg, val);
}

void opl_generate(int16_t *buf, int frames) {
    if (frames > 0)
        OPL3_GenerateStream(&chip, buf, (uint32_t)frames);
}
