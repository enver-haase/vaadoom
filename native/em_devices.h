/* -----------------------------------------------------------------------------
 * em_devices.h — the two optional zero-page devices of the Vaadoom engine:
 * a PCM sound card and a host-file (WAD) channel.
 *
 * Both follow the lunatix device convention (see lunatix `src/vm.h`, kernel
 * `arch/subleq/include/asm/subleq-regs.h`): the registers are ORDINARY zero-page
 * words in the free gap between the RTC (words 64-66) and the kernel text (which
 * starts at word 1024). That is what makes them *optional* — a VM without these
 * devices just performs a harmless `mem[R] -= mem[A]` into a reserved cell nobody
 * reads, so one and the same boot image runs silently on a stock CableVM and with
 * sound + host WAD here. No ABI fork, no negative-operand magic.
 *
 * Two access shapes, both taken from devices this VM already has:
 *   - WRITE register (like the sound card / control registers): the guest issues
 *     `mem[R] -= mem[A]`; we intercept R, drop the store, and take V = mem[A] as
 *     the payload.
 *   - READ register (like the RTC at word 64): when an instruction *sources* the
 *     register we refresh that memory cell first and then let the ordinary subleq
 *     run, so the guest reads it with a plain load.
 *
 * Everything is inert until JS calls em_dev_enable() — with no WAD and no sound
 * the engine behaves exactly like the stock upstream VM, bit for bit.
 * ---------------------------------------------------------------------------*/
#ifndef VAADOOM_EM_DEVICES_H
#define VAADOOM_EM_DEVICES_H

#include <emscripten.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "opl_shim.h"

/* ------------------------------------------------------------------ registers */
/* Sound card (identical to lunatix / the guest's subleq_sound driver). */
#define MMIO_OPL        67   /* W: packed (reg<<8)|val -> OPL3 (music; not synthesized yet) */
#define MMIO_PCM_BASE   68   /* W: word index of the guest PCM ring in M (0 = disabled)     */
#define MMIO_PCM_FRAMES 69   /* W: ring capacity in stereo frames                           */
#define MMIO_PCM_WRITE  70   /* W: producer counter — total stereo frames enqueued          */
#define MMIO_PCM_RATE   71   /* W: PCM sample rate in Hz                                    */

/* Host-file channel — new in Vaadoom, used to hand the fetched WAD to the guest.
 * The host owns the bytes; a transfer is a host-side memcpy straight into guest
 * RAM, so the slow SUBLEQ CPU never copies the file itself. */
#define MMIO_HF_SEL     72   /* W: stream select: 0 = file bytes, 1 = suggested file name   */
#define MMIO_HF_DEST    73   /* W: destination WORD index in guest RAM                      */
#define MMIO_HF_OFF     74   /* W: byte offset within the selected stream                   */
#define MMIO_HF_LEN     75   /* W: byte count — writing this performs the transfer          */
#define MMIO_HF_SIZE    76   /* R: size of the selected stream in bytes (0 = no file)       */
#define MMIO_HF_DONE    77   /* R: bytes copied by the last transfer, or -1 on a bad request*/

#define MMIO_DEV_LO     MMIO_OPL        /* write-register window: 67..75 */
#define MMIO_DEV_HI     MMIO_HF_LEN
#define MMIO_RD_LO      MMIO_HF_SIZE    /* read-register window:  76..77 */
#define MMIO_RD_HI      MMIO_HF_DONE

/* em_dev_enable() flags. */
#define DEV_SOUND       1
#define DEV_HOSTFILE    2

/* ---------------------------------------------------------------------- state */
#define PCM_RATE_DEFAULT 11025          /* DOOM's SFX rate; the guest overrides it */
#define AUDIO_PCM_MAX    8192           /* max frames handed to JS per pump        */
#define OPL_RATE         49716          /* OPL3's native emit rate (Hz)            */
#define OPL_MAX_CATCHUP  4096           /* cap one generate() burst (~82 ms)       */
#define OPL_BLOCK_FRAMES 2048           /* hand music to JS in blocks of this size */

static int      dev_flags;              /* DEV_* bits; 0 = every device inert */

static struct {
    int32_t  base;                      /* word index of the guest ring (0 = off) */
    int32_t  frames;                    /* ring capacity in stereo frames         */
    uint32_t write;                     /* guest producer counter                 */
    uint32_t read;                      /* our consumer counter                   */
    int32_t  rate;                      /* sample rate in Hz                      */
    uint32_t opl_writes;                /* OPL register writes seen (music: TODO) */
} pcm = { 0, 0, 0, 0, PCM_RATE_DEFAULT, 0 };

/* Music. The guest writes OPL3 registers (its music player translates MUS/MIDI to
 * register pokes, paced off the RTC) and we run the chip here, exactly like a DOS
 * DOOM driving an AdLib. Generation is WALL-CLOCK proportional and happens often —
 * that is what keeps the timing right: Nuked stamps each buffered register write
 * against the generation sample counter, so a write lands where the guest issued it
 * only if generation tracks real time. Generating one big block per VM slice would
 * quantize every note onto the slice boundary, which is what a 140 Hz score sounds
 * like when it is sampled at 13 Hz: mush. */
static struct {
    int      touched;                   /* the guest has driven the chip at least once */
    double   last_ms;                   /* wall clock at the last generate()           */
    int16_t  buf[OPL_BLOCK_FRAMES * 2]; /* accumulates frames until a block is full    */
    int      have;                      /* frames currently in buf                     */
    uint64_t generated;                 /* total frames generated since the chip reset */
} opl;

/* Writes the guest scheduled for a moment that has not been rendered yet. The
 * guest runs its sequencer ahead of real time and stamps each write with how far
 * in the future it belongs (the top 15 bits of the MMIO word, in milliseconds);
 * we hold it here and apply it at exactly that sample. Without this the guest's
 * render loop — about 7 iterations a second on this machine — would decide when
 * every note starts, smearing a score written on a 250 ms grid by +-100 ms. */
#define OPL_SCHED_MAX 1024
static struct { uint64_t at; uint16_t reg; uint8_t val; } opl_sched[OPL_SCHED_MAX];
static int opl_sched_head, opl_sched_count;

static struct {
    uint8_t *data;  uint32_t size;      /* stream 0: the WAD itself     */
    uint8_t *name;  uint32_t name_len;  /* stream 1: its file name      */
    int32_t  sel, dest, off, done;
    uint32_t served;                    /* bytes handed to the guest so far */
} hf;

/* ------------------------------------------------------------------- JS calls */
/* Hand one block of stereo frames (interleaved S16) to the worker, which forwards
 * it to the page's AudioContext. Copied out of the heap here because the heap can
 * move (ALLOW_MEMORY_GROWTH) and the block travels to another thread. */
EM_JS(void, em_audio_push, (const int16_t *ptr, int frames, int rate), {
  if (!globalThis.__vdAudio) return;                 /* audio not wired up */
  var pcm = HEAP16.slice(ptr >> 1, (ptr >> 1) + frames * 2);
  globalThis.__vdAudio(pcm, rate);
});

/* Music goes to the page as its own stream at the OPL's native rate; the page mixes
 * the two by simply playing both (SDL did the same with two audio streams). Keeping
 * them separate avoids resampling 49716 Hz down to the SFX rate in here. */
EM_JS(void, em_audio_push_opl, (const int16_t *ptr, int frames, int rate), {
  if (!globalThis.__vdAudioOpl) return;
  var pcm = HEAP16.slice(ptr >> 1, (ptr >> 1) + frames * 2);
  globalThis.__vdAudioOpl(pcm, rate);
});

/* Diagnostic: mirror every OPL register write to JS with its wall-clock time, so the
 * guest's music stream can be inspected (is it keying notes at all? how often? on
 * which channels?). Off by default and free when off. */
EM_JS(void, em_opl_trace, (int reg, int val, double t, int dt), {
  if (globalThis.__vdOplLog) globalThis.__vdOplLog.push([Math.round(t), reg, val, dt]);
});

static int opl_tracing;

/* Booting a machine must leave the devices as quiet as a cold one: no note still
 * keyed from a previous run, no half-finished transfer, no ring armed at an
 * address that means nothing to the new image. Called from main(). */
static void dev_reset(void) {
    pcm.base = pcm.frames = 0;
    pcm.write = pcm.read = 0;
    pcm.rate = PCM_RATE_DEFAULT;
    opl.touched = 0;
    opl.have = 0;
    opl.generated = 0;
    opl_sched_head = opl_sched_count = 0;
    hf.sel = hf.dest = hf.off = hf.done = 0;
}

/* ------------------------------------------------------------------ interface */
/* JS: Module._em_dev_enable(flags) — turns the optional devices on. */
EMSCRIPTEN_KEEPALIVE void em_dev_enable(int flags) { dev_flags = flags; }

/* JS: Module._em_opl_trace_enable(1) — start mirroring OPL writes (diagnostic). */
EMSCRIPTEN_KEEPALIVE void em_opl_trace_enable(int on) { opl_tracing = on; }

/* JS: Module._em_hf_set(dataPtr, dataLen, namePtr, nameLen) — publishes the WAD.
 * The buffers are malloc'd on the JS side and owned by the engine afterwards. */
EMSCRIPTEN_KEEPALIVE void em_hf_set(uint8_t *data, int data_len, uint8_t *name, int name_len) {
    hf.data = data;  hf.size     = (uint32_t) (data_len > 0 ? data_len : 0);
    hf.name = name;  hf.name_len = (uint32_t) (name_len > 0 ? name_len : 0);
    hf.sel = hf.dest = hf.off = 0;
    hf.done = 0;
    hf.served = 0;
}

/* JS: Module._em_hf_served() — bytes the guest has pulled through the host-file
 * device. Proof that DOOM is really reading the fetched WAD, and a useful thing to
 * show while a big IWAD is being paged in. */
EMSCRIPTEN_KEEPALIVE int em_hf_served(void) { return (int) hf.served; }

/* JS: Module._em_opl_writes() — how many OPL register writes the guest has made.
 * Lets the worker report "music is being driven" while FM synthesis is still TODO. */
EMSCRIPTEN_KEEPALIVE int em_opl_writes(void) { return (int) pcm.opl_writes; }

/* -------------------------------------------------------------- host-file I/O */
static const uint8_t *hf_stream(uint32_t *len_out) {
    if (hf.sel == 1) { *len_out = hf.name_len; return hf.name; }
    *len_out = hf.size; return hf.data;
}

/* The MMIO_HF_LEN doorbell: copy [off, off+len) of the selected stream into guest
 * RAM at word index dest. Clamped to both the stream and the guest's memory; the
 * number of bytes actually written lands in MMIO_HF_DONE. */
static void hf_transfer(int32_t *mem, int32_t mem_words, int32_t len) {
    uint32_t avail = 0;
    const uint8_t *src = hf_stream(&avail);
    if (!src || len <= 0 || hf.off < 0 || hf.dest <= 0 || (uint32_t) hf.off >= avail) {
        hf.done = (len == 0) ? 0 : -1;
        return;
    }
    uint32_t n = (uint32_t) len;
    if (n > avail - (uint32_t) hf.off) n = avail - (uint32_t) hf.off;
    /* Destination must fit in guest RAM (word granularity, rounded up). */
    int64_t need_words = ((int64_t) n + 3) / 4;
    if ((int64_t) hf.dest + need_words > mem_words) { hf.done = -1; return; }
    memcpy((uint8_t *) &mem[hf.dest], src + hf.off, n);
    hf.done = (int32_t) n;
    hf.served += n;
}

/* ------------------------------------------------------------------ dispatch */
/* A guest write to a device register (payload = mem[A]). Called only for
 * b in [MMIO_DEV_LO, MMIO_DEV_HI]; returns 0 if the device is disabled, in which
 * case the caller must fall through to the ordinary subleq store. */
static inline int dev_write(int32_t *mem, int32_t mem_words, int32_t reg, int32_t v) {
    switch (reg) {
        case MMIO_OPL:
        case MMIO_PCM_BASE:
        case MMIO_PCM_FRAMES:
        case MMIO_PCM_WRITE:
        case MMIO_PCM_RATE:
            if (!(dev_flags & DEV_SOUND)) return 0;
            break;
        default:
            if (!(dev_flags & DEV_HOSTFILE)) return 0;
            break;
    }
    switch (reg) {
        case MMIO_OPL:
            pcm.opl_writes++;
            if (!opl.touched) {                      /* first write: start the chip */
                opl.touched = 1;
                opl.last_ms = emscripten_get_now();
                opl.generated = 0;
                opl_sched_head = opl_sched_count = 0;
                opl_reset(OPL_RATE);
            }
            {
                uint16_t reg = (uint16_t) ((v >> 8) & 0x1FF);
                uint8_t  val = (uint8_t) (v & 0xFF);
                uint32_t dt_ms = ((uint32_t) v >> 17) & 0x7FFF;   /* 0 = right now */
                if (opl_tracing) em_opl_trace(reg, val, emscripten_get_now(), (int) dt_ms);
                if (dt_ms == 0 || opl_sched_count >= OPL_SCHED_MAX) {
                    opl_write_buffered(reg, val);
                } else {
                    int i = (opl_sched_head + opl_sched_count) % OPL_SCHED_MAX;
                    opl_sched[i].at  = opl.generated + (uint64_t) dt_ms * OPL_RATE / 1000;
                    opl_sched[i].reg = reg;
                    opl_sched[i].val = val;
                    opl_sched_count++;
                }
            }
            break;
        case MMIO_PCM_BASE:   pcm.base   = v;                 break;
        case MMIO_PCM_FRAMES: pcm.frames = v;                 break;
        case MMIO_PCM_WRITE:  pcm.write  = (uint32_t) v;      break;
        case MMIO_PCM_RATE:   if (v >= 4000 && v <= 96000) pcm.rate = v; break;
        case MMIO_HF_SEL:     hf.sel  = v;                    break;
        case MMIO_HF_DEST:    hf.dest = v;                    break;
        case MMIO_HF_OFF:     hf.off  = v;                    break;
        case MMIO_HF_LEN:     hf_transfer(mem, mem_words, v); break;
    }
    return 1;
}

/* Refresh a read register in place, so the guest's ordinary load sees it.
 * Called only for a in [MMIO_RD_LO, MMIO_RD_HI]. */
static inline void dev_read_refresh(int32_t *mem, int32_t reg) {
    if (!(dev_flags & DEV_HOSTFILE)) return;
    if (reg == MMIO_HF_SIZE) { uint32_t n; hf_stream(&n); mem[reg] = (int32_t) n; }
    else                     { mem[reg] = hf.done; }
}

/* Generate as much music as real time has advanced since the last call, and hand it
 * to JS in fixed blocks. Called often (see vm_nommu.c) so that Nuked's write stamping
 * has fine-grained positions to land on; the catch-up is capped so that a stalled tab
 * cannot synthesize a huge burst on its next tick. Silent — and free — until the guest
 * first drives the chip. */
static void opl_pump(int flush) {
    if (!(dev_flags & DEV_SOUND) || !opl.touched) return;

    double now = emscripten_get_now();
    double dms = now - opl.last_ms;
    if (dms < 0) dms = 0;
    opl.last_ms = now;

    uint32_t want = (uint32_t) (dms * OPL_RATE / 1000.0);
    if (want > OPL_MAX_CATCHUP) want = OPL_MAX_CATCHUP;

    while (want) {
        /* Apply everything the guest scheduled for a sample we have reached. */
        while (opl_sched_count && opl_sched[opl_sched_head].at <= opl.generated) {
            opl_write_buffered(opl_sched[opl_sched_head].reg, opl_sched[opl_sched_head].val);
            opl_sched_head = (opl_sched_head + 1) % OPL_SCHED_MAX;
            opl_sched_count--;
        }
        /* Render no further than the next scheduled write, so it lands on time. */
        uint32_t seg = want;
        if (opl_sched_count) {
            uint64_t until = opl_sched[opl_sched_head].at - opl.generated;
            if (until < seg) seg = (uint32_t) until;
        }
        if (!seg) continue;                     /* a write is due right now */
        int room = OPL_BLOCK_FRAMES - opl.have;
        int n = (int) (seg < (uint32_t) room ? seg : (uint32_t) room);
        opl_generate(&opl.buf[opl.have * 2], n);
        opl.have += n;
        opl.generated += (uint64_t) n;
        want -= (uint32_t) n;
        if (opl.have == OPL_BLOCK_FRAMES) {
            em_audio_push_opl(opl.buf, opl.have, OPL_RATE);
            opl.have = 0;
        }
    }
    if (flush && opl.have) {                 /* end of slice: do not sit on a part block */
        em_audio_push_opl(opl.buf, opl.have, OPL_RATE);
        opl.have = 0;
    }
}

/* Drain the guest's PCM ring (one stereo frame per word: lo16 = L, hi16 = R) and
 * hand the frames to JS. Called from the run loop every AUDIO_PUMP_STEPS steps. */
static void audio_pump(const int32_t *mem, int32_t mem_words) {
    if (!(dev_flags & DEV_SOUND)) return;
    if (pcm.base <= 0 || pcm.frames <= 0 || (int64_t) pcm.base + pcm.frames > mem_words) return;

    uint32_t cap   = (uint32_t) pcm.frames;
    uint32_t avail = pcm.write - pcm.read;       /* wrap-safe unsigned difference */
    if (avail > cap) { pcm.read = pcm.write - cap; avail = cap; }  /* guest lapped us */
    if (avail > AUDIO_PCM_MAX) avail = AUDIO_PCM_MAX;
    if (!avail) return;

    static int16_t block[AUDIO_PCM_MAX * 2];
    for (uint32_t i = 0; i < avail; i++) {
        uint32_t w = (uint32_t) mem[pcm.base + (int32_t) ((pcm.read + i) % cap)];
        block[2 * i]     = (int16_t) (w & 0xFFFF);
        block[2 * i + 1] = (int16_t) (w >> 16);
    }
    pcm.read += avail;
    em_audio_push(block, (int) avail, pcm.rate);
}

#endif /* VAADOOM_EM_DEVICES_H */
