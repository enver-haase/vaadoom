/* -----------------------------------------------------------------------------
 * vm_nommu.c — minimal NOMMU SUBLEQ VM for the bundled Eternal-Linux + DOOM image.
 *
 * Derived from Adrian Cable's eternal `vm/vm.c` (MIT). This is the *fast* engine:
 * the NOMMU image never uses user/supervisor mode or the MMU, so there is no
 * address translation, no mode checks and no fault machinery — just the bare
 * subleq loop. ~2.4x faster than the MMU-capable vm.c on the same image.
 *
 * The WebAssembly seam (#ifdef __EMSCRIPTEN__) mirrors vm.c: canvas present +
 * input ring (em_backend.h), image from MEMFS, and JS-driven em_run_slice().
 * The native SDL3 path is preserved for parity/benchmarking.
 * ---------------------------------------------------------------------------*/
#ifdef __EMSCRIPTEN__
#  include "em_backend.h"
#  include "em_devices.h"
#else
#  include <SDL3/SDL.h>
#endif
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define FB_WIDTH  800
#define FB_HEIGHT 512
#define FB_SIZE   (FB_WIDTH * FB_HEIGHT * 4)
#define FB_ADDR   (0x60000000 - FB_SIZE)      /* fixed framebuffer address (eternal ABI) */
#ifndef MEM_WORDS
#define MEM_WORDS (3 << 27)                   /* 1.5 GiB, overridable for the wasm heap */
#endif
#define PRESENT_MIN_MS 15.0                   /* throttle framebuffer blits to ~66 fps */

static int32_t *mem;
static int32_t  pc, timer;
static double   last_present_ms;

#ifndef __EMSCRIPTEN__
static SDL_Window  *window;
static SDL_Surface *screen;
#endif

static inline double now_ms(void) {
#ifdef __EMSCRIPTEN__
    return emscripten_get_now();
#else
    return (double) SDL_GetTicks();
#endif
}

static inline int32_t kb_poll(void) {
#ifdef __EMSCRIPTEN__
    return em_kbd_poll();                      /* ring filled by JS: +down / -up / 0 */
#else
    if (!screen) return 0;                     /* headless (--bench) */
    SDL_Event e;
    if (SDL_PollEvent(&e) && (e.type == SDL_EVENT_KEY_DOWN || e.type == SDL_EVENT_KEY_UP))
        return (e.type == SDL_EVENT_KEY_DOWN) ? e.key.scancode : -e.key.scancode;
    return 0;
#endif
}

static inline void present(void) {
    double t = now_ms();
    if (t - last_present_ms < PRESENT_MIN_MS) return;
    last_present_ms = t;
#ifdef __EMSCRIPTEN__
    em_fb_present((const uint8_t *) &mem[FB_ADDR / 4], FB_SIZE);
#else
    if (!screen) return;
    memcpy(screen->pixels, &mem[FB_ADDR / 4], FB_SIZE);
    SDL_UpdateWindowSurface(window);
#endif
}

/* An operand word is a byte address; bit 0 selects one level of indirection. */
static inline int32_t fetch(void) {
    int32_t raw = mem[pc++];
    return (raw & 1) ? mem[raw / 4] / 4 : raw / 4;
}

/* Run up to max_steps (0 = forever). Returns steps executed; stops on halt (c==0). */
static uint64_t run(uint64_t max_steps) {
    int32_t a, b, c;
    uint64_t n = 0;
    for (;;) {
        if (max_steps && n >= max_steps) break;
        a = fetch(); b = fetch(); c = fetch();
        n++;
        if (a == -1) {                         /* keyboard MMIO: mem[b] = next key event */
            mem[b] = kb_poll();
        } else if (b == -1) {                  /* serial console byte */
#ifndef __EMSCRIPTEN__
            unsigned char ch = (unsigned char) mem[a];
            write(1, &ch, 1);
#endif                                          /* (dropped in the browser; fbcon shows it) */
#ifdef __EMSCRIPTEN__
        } else if (dev_flags                   /* zero-page device write (sound / host file) */
                   && (uint32_t) (b - MMIO_DEV_LO) <= (uint32_t) (MMIO_DEV_HI - MMIO_DEV_LO)
                   && dev_write(mem, MEM_WORDS, b, mem[a])) {
            /* register written; the store is dropped and no branch is taken, exactly like
             * the control-register and sound paths in lunatix's cpu.c. */
#endif
        } else {                               /* subleq */
            if (a == 64) timespec_get((struct timespec *) &mem[64], TIME_UTC);  /* RTC */
#ifdef __EMSCRIPTEN__
            /* Read registers behave like the RTC: refresh the cell, then load normally. */
            if (dev_flags && (uint32_t) (a - MMIO_RD_LO) <= (uint32_t) (MMIO_RD_HI - MMIO_RD_LO))
                dev_read_refresh(mem, a);
#endif
            mem[b] -= mem[a];
            if (mem[b] <= 0) pc = c;
            if (mem[0] && ++timer > 800000) {  /* timer interrupt + display refresh */
                present();
#ifdef __EMSCRIPTEN__
                /* Drain the sound ring on the guest's own tick (~every 800k steps)
                 * rather than once per slice: a slice can accumulate most of a
                 * second of SFX, and handing that over as one block would put the
                 * same delay between an action and the sound it makes. */
                audio_pump(mem, MEM_WORDS);
#endif
                mem[1] = pc * 4;
                pc = mem[0] / 4;
                timer = 0;
            }
        }
        if (!c) break;                         /* halt */
    }
    return n;
}

#ifdef __EMSCRIPTEN__
/* Base of the emulated RAM inside the wasm heap. Test/debug hook: it lets JS read
 * guest memory (used by the device self-test in native/test/). */
EMSCRIPTEN_KEEPALIVE int em_mem_base(void) { return (int) (intptr_t) mem; }

EMSCRIPTEN_KEEPALIVE int em_run_slice(int max_steps) {
    int n = (int) run((uint64_t) max_steps);
    audio_pump(mem, MEM_WORDS);   /* catch whatever the last tick left in the ring */
    return n;
}
#endif

static void load_image(void) {
    size_t limit = (size_t) MEM_WORDS * 4;
#ifdef __EMSCRIPTEN__
    FILE *f = fopen("/boot.img", "rb");
    if (!f) { fprintf(stderr, "vaadoom: cannot open /boot.img\n"); exit(1); }
    fread(mem, 1, limit, f);
    fclose(f);
#else
    size_t off = 0;
    for (;;) {
        ssize_t k = read(0, (char *) mem + off, limit - off);
        if (k <= 0) break;
        off += (size_t) k;
        if (off >= limit) break;
    }
    if (off == 0) { fprintf(stderr, "vaadoom: empty boot image on stdin\n"); exit(1); }
#endif
}

int main(int argc, char **argv) {
    mem = calloc(MEM_WORDS, sizeof *mem);
    if (!mem) { fprintf(stderr, "vaadoom: cannot allocate %d words\n", MEM_WORDS); return 1; }

    if (argc > 1 && strcmp(argv[1], "--bench") == 0) {
        uint64_t cap = (argc > 2) ? strtoull(argv[2], NULL, 10) : 1000000000ULL;
        load_image();
        struct timespec t0, t1;
        timespec_get(&t0, TIME_UTC);
        uint64_t did = run(cap);
        timespec_get(&t1, TIME_UTC);
        double s = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;
        fprintf(stderr, "nommu bench: %llu steps in %.3fs = %.1f Msteps/s%s\n",
                (unsigned long long) did, s, did / s / 1e6, did < cap ? " (HALTED)" : "");
        return 0;
    }

    load_image();

#ifdef __EMSCRIPTEN__
    em_video_init(FB_WIDTH, FB_HEIGHT);
    pc = 0;
    return 0;                                  /* JS drives execution via em_run_slice() */
#else
    if (!SDL_Init(SDL_INIT_VIDEO)) { fprintf(stderr, "vaadoom: SDL init failed\n"); return 1; }
    window = SDL_CreateWindow("vaadoom", FB_WIDTH, FB_HEIGHT, 0);
    screen = SDL_GetWindowSurface(window);
    pc = 0;
    run(0);
    SDL_DestroyWindow(window);
    SDL_Quit();
    free(mem);
    return 0;
#endif
}
