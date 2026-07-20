/*
 * lunatix vm.c — Phase 1 core: subneg VM + user/supervisor mode + base+bound MMU.
 *
 * Phase 0 was a faithful, de-obfuscated reimplementation of the IOCCC 2025 "cable"
 * subleq VM (adriancable/eternal). Phase 1 adds the machinery that makes lunatix
 * lunatix: a CPU mode bit, control registers, and a base+bound MMU with PRECISE,
 * RESTARTABLE faults — the protection cable's NOMMU Linux explicitly lacks.
 *
 * COMPATIBILITY: the machine boots in SUPERVISOR mode, where translation is the
 * identity. cable's kernel runs entirely in supervisor, so its vmlinux.bootimage
 * still boots bit-identically — the MMU is dormant until someone drops to user mode.
 *
 * THE INSTRUCTION (unchanged from cable — subleq):
 *      mem[B] -= mem[A];  if (mem[B] <= 0) PC = C;  else PC += 3 words
 *   32-bit little-endian words; operand words hold BYTE addresses; bit 0 = indirect;
 *   /4 converts a byte address to a word index into M.
 *   (lunatix's own subneg / branch-on-<0 ISA divergence is still deferred; the
 *    branch condition is orthogonal to protection, so Phase 1 keeps subleq to
 *    preserve the cable baseline.)
 *
 * MEMORY PROTECTION (the point):
 *   - MODE_SUPER: translate() is identity. Kernel sees physical memory. MMIO active.
 *   - MODE_USER:  translate() is base+bound. paddr = vaddr + BASE; a vaddr outside
 *                 [0, LIMIT) faults. MMIO is disabled (a user cannot reach devices).
 *   - PRECISE, RESTARTABLE FAULTS FOR FREE: step() resolves and bounds-checks EVERY
 *     operand access before it commits the mem[B] write or the PC update. A fault
 *     therefore mutates nothing; "restart" is just "put PC back to the instruction
 *     start and don't commit." No setjmp, no 68000-style unrestartable-frame problem.
 *   - On a fault the CPU records cause + faulting vaddr + the (restartable) user PC,
 *     switches to supervisor, and vectors to the trap handler — exactly the
 *     abort-and-trap a supervisor needs to kill or page-in the offending task.
 */

#ifdef __EMSCRIPTEN__
#  include "em_backend.h"        /* WebAssembly seam: canvas video + input queue */
#else
#  include <SDL3/SDL.h>          /* native build: unchanged SDL3 backend */
#endif
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* ------------------------------------------------------------------ config */

#ifndef MEM_WORDS
#define MEM_WORDS   (3 << 27)          /* 402,653,184 words = 1.5 GiB (cable's size) */
#endif                                 /* overridable at compile time (e.g. -DMEM_WORDS=...) for the wasm build */
#define FB_W        800
#define FB_H        512
#define FB_BYTES    (FB_W * FB_H * 4)
#define TIMER_STEPS 800000

/* Low guard page: user virtual addresses below this ALWAYS fault, so a NULL or
 * small-pointer dereference is trapped instead of hitting real memory. User tasks
 * are linked to start here — the same trick x86 uses by leaving the bottom of the
 * address space unmapped below the load address. 0x1000 bytes == 0x400 words. */
#define USER_VA_MIN 0x400               /* 0x1000 bytes, in words */

/* Phase 2 paging: word-based 4 KiB pages, two-level page table. */
#define PAGE_SHIFT_W 10                 /* 1024 words = 4 KiB per page */
#define PAGE_WORDS   (1 << PAGE_SHIFT_W)

/* Kernel vmalloc window (Option B, real-world kernel-space paging): in supervisor
 * mode, word indices at/above this are translated through CR_KPTB; everything below
 * stays identity (the direct-mapped physical region). It sits exactly at the top of
 * physical RAM (MEM_WORDS), so identity covers all of RAM and vmalloc lives just above.
 * Must stay below 2 GiB in BYTES (word index < 0x20000000) because operand word indices
 * are signed (v/4); a 3 GiB window would wrap negative and mis-route. cable never sets
 * CR_KPTB, so its supervisor stays fully identity and it boots unchanged. */
#define KVMALLOC_MIN_W MEM_WORDS

enum {                                  /* fixed low-memory "registers" (word indices) */
    REG_INT_HANDLER  = 0,
    REG_INT_SAVED_PC = 1,
    REG_FB_OFFSET    = 6,
    REG_RTC          = 64,
};

#define MMIO_KEYBOARD (-1)              /* effective operand index -1: keyboard  */
#define MMIO_CLOCK    (64)              /* effective operand index 64: RTC       */

/* Privileged control registers — supervisor-only MMIO, addressed like cable's
 * device magic. A subneg operand naming one of these effective indices reads or
 * writes a CPU control register instead of memory. NO NEW INSTRUCTION: the one
 * subneg op drives the MMU by touching these addresses, exactly as it drives the
 * keyboard/clock/framebuffer. In user mode these indices are negative -> below
 * USER_VA_MIN -> they fault, so the interface is privileged by construction.
 * Writing CR_RTE is return-from-trap: enter user mode at CR_SAVED_PC. */
#define CR_BASE       (-16)
#define CR_LIMIT      (-17)
#define CR_VECTOR     (-18)
#define CR_SAVED_PC   (-19)
#define CR_CAUSE      (-20)
#define CR_FAULT_ADDR (-21)
#define CR_RTE        (-22)
#define CR_PTB        (-23)   /* page-table base (physical word idx); 0 = paging off */
#define CR_FAULT_ACC  (-24)   /* access type of the last fault (R/W/X)               */
#define CR_KPTB       (-25)   /* kernel page-table base for the vmalloc window (real- *
                               * world kernel-space paging); 0 = super stays identity */
#define CR_SYSGATE    (-26)   /* user syscall-gate vaddr (word idx); user pc==this raises *
                               * CAUSE_SYSCALL. 0 = disabled (cable/NOMMU never set it)    */

typedef enum { ACCESS_READ, ACCESS_WRITE, ACCESS_EXEC } access_t;
typedef enum { MODE_SUPER, MODE_USER }                  cpu_mode_t;
typedef enum { STEP_CONT, STEP_HALT, STEP_FAULT }       step_result_t;

enum { FAULT_NONE = 0, FAULT_BOUNDS = 1, FAULT_PAGE = 2, CAUSE_TIMER = 0,
       CAUSE_SYSCALL = 3 };  /* trap causes */

/* --------------------------------------------------------------- machine state */

static int32_t     *M;              /* flat physical memory */
static int32_t      pc;             /* program counter (word index; vaddr in user mode) */
static uint64_t     ticks;

/* control registers — the CPU's protection state */
static struct {
    cpu_mode_t mode;
    int32_t    base;                /* MMU base, in words (added to a user vaddr)     */
    int32_t    limit;               /* MMU bound, in words (user vaddr must be < this) */
    int32_t    vector;              /* trap handler, physical word index               */
    int32_t    saved_pc;            /* PC saved on trap (restartable: the faulting instr) */
    int32_t    cause;               /* last fault cause                                 */
    int32_t    fault_addr;          /* faulting vaddr (word index)                      */
    int32_t    ptb;                 /* page-table base (physical word idx); 0 = paging off */
    int32_t    fault_access;        /* access type of the last fault                    */
    int32_t    kptb;                /* kernel page-table base (vmalloc window); 0 = off  */
    int32_t    sysgate;             /* user syscall-gate vaddr (word idx); 0 = disabled  */
} cpu;

static bool fault_pending;          /* set by translate() on a violation mid-step */
static uint32_t user_quantum = 0;   /* >0: preempt a user task after this many steps (0 = off) */

#ifndef __EMSCRIPTEN__
static SDL_Window  *window;
static SDL_Surface *surface;
#endif

/* ------------------------------------------------------------------- MMU seam */

static void raise_fault(int32_t cause, int32_t vaddr) {
    if (!fault_pending) {           /* first fault in an instruction wins */
        fault_pending  = true;
        cpu.cause      = cause;
        cpu.fault_addr = vaddr;
    }
}

/* vaddr (word index) -> paddr (word index). Sets fault_pending on a violation and
 * returns a harmless 0 so the caller can bail without a wild access. */
static void page_fault(int32_t wi, access_t access) {
    cpu.fault_access = access;
    raise_fault(FAULT_PAGE, wi);
}

/* Two-level page-table walk shared by user paging and kernel (vmalloc) paging.
 * base is a physical word index of the L1 table. Sets fault_pending + returns 0 on a
 * missing/protected page. */
static inline int32_t walk_pt(int32_t base, int32_t wi, access_t access) {
    int32_t vpn = wi >> PAGE_SHIFT_W;
    int32_t off = wi & (PAGE_WORDS - 1);

    /* Every table/page index must be a valid physical word index. A malformed page
     * table must raise a fault, not index M[] out of bounds and crash the VM. */
    int64_t l1i = (int64_t)base + ((vpn >> 10) & 0x3FF);
    if (l1i < 0 || l1i >= MEM_WORDS) {
        fprintf(stderr, "[walk] L1 index %lld OOB: base=%d wi=0x%08x\n",
                (long long)l1i, base, (uint32_t)wi);
        page_fault(wi, access); return 0;
    }
    int32_t l2 = M[l1i];                                /* L1[vpn hi] -> L2 table, 0=absent */
    if (!l2) { page_fault(wi, access); return 0; }

    int64_t l2i = (int64_t)l2 + (vpn & 0x3FF);
    if (l2i < 0 || l2i >= MEM_WORDS) {
        fprintf(stderr, "[walk] L2 index %lld OOB: l2=%d wi=0x%08x\n",
                (long long)l2i, l2, (uint32_t)wi);
        page_fault(wi, access); return 0;
    }
    int32_t pte = M[l2i];                               /* PTE: bit0=present, bit1=writable, frame=pte>>4 */
    if (!(pte & 1)) { page_fault(wi, access); return 0; }              /* not present -> demand fault */
    if (access == ACCESS_WRITE && !(pte & 2)) { page_fault(wi, access); return 0; } /* write-protected */

    int64_t pa = (int64_t)(pte >> 4) * PAGE_WORDS + off;
    if (pa < 0 || pa >= MEM_WORDS) {
        fprintf(stderr, "[walk] PA %lld OOB: pte=0x%08x frame=%d wi=0x%08x\n",
                (long long)pa, (uint32_t)pte, pte >> 4, (uint32_t)wi);
        page_fault(wi, access); return 0;
    }
    return (int32_t)pa;
}

/* Hot-path shortcut: true when translation is pure identity + bounds check, i.e.
 * supervisor mode with no kernel vmalloc window (cable/NOMMU is always here). Kept
 * in sync by refresh_xlat() at the few mode/kptb transition points. */
static bool xlat_fast;
static inline void refresh_xlat(void) {
    xlat_fast = (cpu.mode == MODE_SUPER) && (cpu.kptb == 0);
}

static inline int32_t translate(int32_t wi, access_t access) {
    if (__builtin_expect(xlat_fast, 1)) {          /* supervisor identity: just bounds-check */
        if (__builtin_expect((uint32_t)wi < (uint32_t)MEM_WORDS, 1)) return wi;
        page_fault(wi, access); return 0;
    }
    if (cpu.mode == MODE_SUPER) {
        /* Real-world kernel-space paging (Option B): the direct-mapped physical region
         * is identity, but a high vmalloc window is translated through CR_KPTB when the
         * kernel has set it. cable never sets CR_KPTB -> supervisor stays fully identity. */
        if (cpu.kptb && wi >= (int32_t)KVMALLOC_MIN_W) {
            static int kdbg = 0;               /* DEBUG: first N kernel-window walks */
            if (kdbg < 24) {
                int32_t vpn = wi >> PAGE_SHIFT_W;
                int32_t l1i = cpu.kptb + ((vpn >> 10) & 0x3FF);
                int32_t l1  = (l1i >= 0 && l1i < MEM_WORDS) ? M[l1i] : -1;
                int32_t pte = (l1 > 0 && (l1 + (vpn & 0x3FF)) >= 0 &&
                               (l1 + (vpn & 0x3FF)) < MEM_WORDS) ? M[l1 + (vpn & 0x3FF)] : -1;
                fprintf(stderr, "[kwalk] wi=0x%08x byte=0x%08x kptb=0x%x l1i=0x%x l1=0x%x pte=0x%x acc=%d\n",
                        (uint32_t)wi, (uint32_t)(wi << 2), cpu.kptb, l1i, l1, pte, access);
                kdbg++;
            }
            return walk_pt(cpu.kptb, wi, access);
        }
        /* identity — kernel sees physical memory. Bound-check so an access beyond
         * physical RAM faults instead of crashing the VM. */
        if ((uint32_t)wi >= MEM_WORDS) {
            fprintf(stderr, "[super-id] OOB wi=%d (0x%08x) access=%d\n",
                    wi, (uint32_t)wi, access);
            page_fault(wi, access); return 0;
        }
        return wi;
    }

    if (cpu.ptb) {                  /* --- Phase 2: paging (two-level page table) --- */
        if (wi < 0) { page_fault(wi, access); return 0; }
        return walk_pt(cpu.ptb, wi, access);
    }

    /* --- Phase 1: base+bound --- */
    if (wi < USER_VA_MIN || wi >= cpu.limit) {
        raise_fault(FAULT_BOUNDS, wi);  /* below the guard page, or past the bound */
        return 0;
    }
    return wi + cpu.base;           /* base+bound relocation */
}

static inline int32_t phys_read(int32_t wi, access_t access) {
    int32_t p = translate(wi, access);
    return fault_pending ? 0 : M[p];
}

/* ------------------------------------------------------ privileged control regs */

static inline bool is_cr(int32_t idx) { return idx <= CR_BASE && idx >= CR_SYSGATE; }

static int32_t cr_read(int32_t idx) {
    switch (idx) {
        case CR_BASE:       return cpu.base;
        case CR_LIMIT:      return cpu.limit;
        case CR_VECTOR:     return cpu.vector;
        case CR_SAVED_PC:   return cpu.saved_pc;
        case CR_CAUSE:      return cpu.cause;
        case CR_FAULT_ADDR: return cpu.fault_addr;
        case CR_PTB:        return cpu.ptb;
        case CR_FAULT_ACC:  return cpu.fault_access;
        case CR_KPTB:       return cpu.kptb;
        case CR_SYSGATE:    return cpu.sysgate;
        default:            return 0;                /* CR_RTE reads as 0 */
    }
}

/* CR[idx] := val. CR_RTE performs return-from-trap: drop to user at CR_SAVED_PC. */
static step_result_t cr_write(int32_t idx, int32_t val) {
    switch (idx) {
        case CR_BASE:       cpu.base       = val; break;
        case CR_LIMIT:      cpu.limit      = val; break;
        case CR_VECTOR:     cpu.vector     = val; break;
        case CR_SAVED_PC:   cpu.saved_pc   = val; break;
        case CR_CAUSE:      cpu.cause      = val; break;
        case CR_FAULT_ADDR: cpu.fault_addr = val; break;
        case CR_PTB:        cpu.ptb = val; break;
        case CR_FAULT_ACC:  cpu.fault_access = val; break;
        case CR_KPTB:       cpu.kptb = val; refresh_xlat(); break;
        case CR_SYSGATE:    cpu.sysgate = val; break;
        case CR_RTE:        cpu.mode = MODE_USER; pc = cpu.saved_pc; refresh_xlat(); break;
    }
    return STEP_CONT;
}

/* value of an operand used as a source: a control register in supervisor mode,
 * otherwise an ordinary (translated) memory read. */
static inline int32_t read_src(int32_t idx) {
    if (cpu.mode == MODE_SUPER && is_cr(idx)) return cr_read(idx);
    int32_t p = translate(idx, ACCESS_READ);
    return fault_pending ? 0 : M[p];
}

/* ---------------------------------------------------------------- operand fetch
 *
 * Read one operand word at the PC and return the effective WORD index it names.
 * Operand words are byte addresses; bit 0 selects indirection. All code/pointer
 * reads go through phys_read, so a bad PC or pointer faults precisely.
 */
static inline int32_t operand(void) {
    int32_t slot = pc++;
    int32_t w    = phys_read(slot, ACCESS_EXEC);
    if (fault_pending) return 0;
    if (w & 1) {                          /* indirect: dereference the pointer */
        int32_t v = phys_read(w / 4, ACCESS_READ);
        return v / 4;
    }
    return w / 4;                         /* direct: value == w, skip the redundant re-read */
}

/* --------------------------------------------------------------------- devices */

static void fb_present(void) {
#ifdef __EMSCRIPTEN__
    em_fb_present((const uint8_t *)&M[M[REG_FB_OFFSET]], FB_BYTES);
#else
    if (!surface) return;                   /* headless (e.g. --bench): no SDL surface */
    memcpy(surface->pixels, &M[M[REG_FB_OFFSET]], FB_BYTES);
    SDL_UpdateWindowSurface(window);
#ifdef CAPTURE
    { const char *path = getenv("LUNATIX_CAPTURE"); if (path) SDL_SaveBMP(surface, path); }
#endif
#endif
}

static void kbd_poll_into(int32_t dst) {    /* supervisor-only; dst is physical */
#ifdef __EMSCRIPTEN__
    /* Same encoding as SDL: keydown -> +scancode, keyup -> -scancode. The JS side
     * pushes those signed codes into em_kbd; 0 means "no event this poll". */
    int32_t code = em_kbd_poll();
    if (code) M[dst] = code;
#else
    if (!surface) return;                   /* headless (e.g. --bench): SDL not initialised */
    int32_t V[32];
    if (SDL_PollEvent((SDL_Event *)V))
        M[dst] = V[6] * (1537 - 2 * V[0]);
#endif
}

static void rtc_read(void) {                /* supervisor-only; physical */
    timespec_get((struct timespec *)&M[REG_RTC], TIME_UTC);
}

/* ------------------------------------------------------------------ one step */

static step_result_t step(void) {
    fault_pending = false;

    /* Syscall gate: a user program requests the kernel by jumping to the gate vaddr.
     * We trap BEFORE executing anything there and deliver CAUSE_SYSCALL via the same
     * restartable path as a fault (run loop saves PC -> CR_SAVED_PC, vectors to
     * CR_VECTOR). cable/NOMMU never set CR_SYSGATE, so this is inert for them. */
    if (cpu.mode == MODE_USER && cpu.sysgate && pc == cpu.sysgate) {
        raise_fault(CAUSE_SYSCALL, pc);
        return STEP_FAULT;
    }

    int32_t A = operand();
    int32_t B = operand();
    int32_t C = operand();
    if (fault_pending) return STEP_FAULT;   /* bad instruction fetch */
    if (C == 0)        return STEP_HALT;

    /* MMIO is privileged: only honored in supervisor mode. A user program can't
     * reach devices — and its addresses are relocated by base+bound anyway. */
    if (cpu.mode == MODE_SUPER) {           /* devices + control registers: privileged */
        if (A == MMIO_CLOCK)      rtc_read();
        if (A == MMIO_KEYBOARD) { kbd_poll_into(translate(B, ACCESS_WRITE)); return STEP_CONT; }
        if (B == MMIO_KEYBOARD)   return STEP_CONT;
        if (is_cr(B)) {                     /* CR[B] := mem[A];  CR_RTE = return-from-trap */
            int32_t val = read_src(A);
            if (fault_pending) return STEP_FAULT;
            return cr_write(B, val);
        }
    }

    /* Resolve + bounds-check BOTH data accesses BEFORE committing anything. */
    int32_t av = read_src(A);               /* control register in supervisor, else memory */
    int32_t pb = translate(B, ACCESS_WRITE);
    if (fault_pending) return STEP_FAULT;   /* nothing mutated, PC not yet committed */

    int32_t result = M[pb] - av;            /* commit point */
    M[pb] = result;

    if (result <= 0) {
        pc = C;                             /* subleq branch */
    } else if (cpu.mode == MODE_SUPER && M[REG_INT_HANDLER] && ++ticks > TIMER_STEPS) {
        fb_present();
        M[REG_INT_SAVED_PC] = 4 * pc;
        pc = M[REG_INT_HANDLER] / 4;
        ticks = 0;
    }
    return STEP_CONT;
}

/* ---------------------------------------------------------------------- run loop
 *
 * max_steps == 0 means "run forever" (cable). A nonzero cap is used by the
 * self-test so a buggy image can't hang. Returns the number of steps executed.
 */
static uint64_t run(uint64_t max_steps) {
    uint64_t n = 0;
    uint32_t q = 0;                         /* steps this user quantum */
    refresh_xlat();                         /* sync fast-path flag with the caller's mode */
    for (;; n++) {
        if (max_steps && n >= max_steps) break;
        int32_t start_pc = pc;              /* for restart on fault */
        step_result_t r = step();
        if (r == STEP_HALT) break;
        if (r == STEP_FAULT) {
            /* precise + restartable: nothing was committed */
            pc            = start_pc;
            cpu.saved_pc  = start_pc;
            cpu.mode      = MODE_SUPER;
            refresh_xlat();
            fprintf(stderr,
                    "[trap] cause=%d faulting vaddr=%d (word) at user pc=%d -> vector %d\n",
                    cpu.cause, cpu.fault_addr, start_pc, cpu.vector);
            pc = cpu.vector;                /* enter the supervisor trap handler */
            q = 0;
            continue;
        }
        /* STEP_CONT: preempt a running user task when its quantum expires */
        if (user_quantum && cpu.mode == MODE_USER && ++q >= user_quantum) {
            q = 0;
            cpu.saved_pc = pc;              /* resume at the NEXT instruction (not a restart) */
            cpu.cause    = CAUSE_TIMER;
            cpu.mode     = MODE_SUPER;
            refresh_xlat();
            pc           = cpu.vector;      /* preempt -> scheduler */
        }
    }
    return n;
}

#ifdef __EMSCRIPTEN__
/* JS drives execution in bounded slices so the worker event loop can present
 * frames (fb_present -> OffscreenCanvas) and drain input between slices, without
 * Asyncify. State (pc, cpu, ticks) lives in globals, so run() resumes across
 * calls. Returns steps actually executed; a short count means the VM halted. */
EMSCRIPTEN_KEEPALIVE int em_run_slice(int max_steps) {
    return (int) run((uint64_t) max_steps);
}
#endif

/* ---------------------------------------------------------------- image load */

static void load_image(void) {
    char  *dst   = (char *)M;
    size_t off   = 0;
    size_t limit = (size_t)MEM_WORDS * 4;
#ifdef __EMSCRIPTEN__
    /* The worker decompresses the bundled image into MEMFS at /boot.img before
     * calling main(); stdin isn't available in a Web Worker. */
    FILE *f = fopen("/boot.img", "rb");
    if (!f) { fprintf(stderr, "lunatix: cannot open /boot.img\n"); exit(1); }
    off = fread(dst, 1, limit, f);
    fclose(f);
#else
    for (;;) {
        ssize_t k = read(0, dst + off, limit - off);
        if (k <= 0) break;
        off += (size_t)k;
        if (off >= limit) break;
    }
#endif
    if (off == 0) { fprintf(stderr, "lunatix: empty boot image\n"); exit(1); }
}

/* ------------------------------------------------------------------ self-test
 *
 * Phase 1 verification (the plan's key milestone): a user task that scribbles out
 * of bounds is caught by a fault and handled by the supervisor, while the rest of
 * memory is provably untouched — i.e. memory protection actually works.
 *
 * We act as firmware: configure the MMU, install a subneg trap handler, drop to
 * user mode. The user task does one legal in-bounds write, then references an
 * out-of-bounds address; the trap vectors to the handler, which sets a "killed"
 * sentinel and halts. (A subneg-programmable privileged control-register interface
 * — so a real supervisor can set BASE/LIMIT and do return-from-trap itself — is the
 * next increment toward the tiny OS.)
 */

/* emit a subneg instruction at word `at`; operands a,b,c given as WORD indices in
 * whatever address space `at` lives in, stored (like cable) as byte addresses. */
static void emit(int32_t at, int32_t a, int32_t b, int32_t c) {
    M[at] = a * 4;  M[at + 1] = b * 4;  M[at + 2] = c * 4;
}

/* self-test fixture (physical word indices, in the supervisor region) */
#define ST_NEG_ONE 100
#define ST_KILLED  101
#define ST_VICTIM  102
#define ST_HANDLER 200
#define ST_MAGIC   0xBEEF
#define ST_UBASE   4096                 /* physical base where the user task maps */
#define ST_LIMIT   (USER_VA_MIN + 0x40) /* top user vaddr (exclusive) */

/* Build a user task linked at vaddr USER_VA_MIN, mapped at physical ST_UBASE, whose
 * second instruction touches `bad_vaddr`. Run it; verify the supervisor caught the
 * fault at `expect_addr` and nothing outside the task was disturbed. */
static int scenario(const char *name, int32_t bad_vaddr, int32_t expect_addr) {
    const int32_t VB = USER_VA_MIN;

    M[ST_NEG_ONE] = -1;
    M[ST_KILLED]  = 0;
    M[ST_VICTIM]  = ST_MAGIC;
    M[REG_INT_HANDLER] = 0;                         /* no timer handler -> timer inert */

    emit(ST_HANDLER,     ST_NEG_ONE, ST_KILLED, ST_HANDLER + 3); /* KILLED -= -1 => 1 */
    emit(ST_HANDLER + 3, 0, 0, 0);                              /* C == 0 -> halt      */

    int32_t base = ST_UBASE;                        /* paddr = vaddr + base */
    emit(base + (VB + 0), VB + 12, VB + 11, VB + 3);   /* Dc -= ONEc       [legal, 5-1=4] */
    emit(base + (VB + 3), VB + 10, bad_vaddr, VB + 6); /* mem[bad] -= Zc   [faults]       */
    M[base + (VB + 10)] = 0;   /* Zc   */
    M[base + (VB + 11)] = 5;   /* Dc   */
    M[base + (VB + 12)] = 1;   /* ONEc */

    cpu.base = base;  cpu.limit = ST_LIMIT;  cpu.vector = ST_HANDLER;
    cpu.cause = FAULT_NONE;  cpu.fault_addr = -1;  cpu.saved_pc = -1;
    cpu.mode  = MODE_USER;   pc = VB;

    run(1000);

    int ok = 1;
    printf("  scenario '%s' (illegal vaddr = 0x%x):\n", name, (unsigned)bad_vaddr);
    #define CHECK(label, cond) do { \
        printf("    %-26s %s\n", label, (cond) ? "ok" : "FAIL"); \
        ok = ok && (cond); } while (0)
    CHECK("legal in-bounds write",   M[base + (VB + 11)] == 4);
    CHECK("fault raised (bounds)",   cpu.cause == FAULT_BOUNDS);
    CHECK("faulting vaddr",          cpu.fault_addr == expect_addr);
    CHECK("PC restartable",          cpu.saved_pc == VB + 3);
    CHECK("supervisor handler ran",  M[ST_KILLED] == 1);
    CHECK("kernel victim untouched", M[ST_VICTIM] == ST_MAGIC);
    #undef CHECK
    return ok;
}

/* A supervisor written IN SUBNEG programs the MMU through the privileged control
 * registers and RTEs into a user task — no C firmware sets mode/base/limit/vector.
 * Proves the control-register interface + return-from-trap with NO new instruction:
 * the single subneg op drives it all by touching magic addresses. */
static int scenario_rte(void) {
    const int32_t VB = USER_VA_MIN, UB = ST_UBASE, TRAP = 300;

    M[100] = -1;            /* scratch (value operand for RTE; ignored) */
    M[101] = 0;             /* KILLED  */
    M[102] = ST_MAGIC;      /* VICTIM  */
    M[110] = UB;            /* value: task base        */
    M[111] = ST_LIMIT;      /* value: task limit       */
    M[112] = TRAP;          /* value: trap vector      */
    M[113] = VB;            /* value: task entry vaddr */
    M[REG_INT_HANDLER] = 0;

    /* supervisor setup: program the CRs, then RTE into the task */
    emit(200, 110, CR_BASE,     203);   /* CR_BASE     := M[110] = task base   */
    emit(203, 111, CR_LIMIT,    206);   /* CR_LIMIT    := M[111]               */
    emit(206, 112, CR_VECTOR,   209);   /* CR_VECTOR   := M[112] = handler     */
    emit(209, 113, CR_SAVED_PC, 212);   /* CR_SAVED_PC := M[113] = entry vaddr */
    emit(212, 100, CR_RTE,      215);   /* RTE -> mode = USER, pc = CR_SAVED_PC */

    /* supervisor trap handler: KILLED := 0 - CR_CAUSE (reads a CR), then halt */
    emit(TRAP,     CR_CAUSE, 101, TRAP + 3);
    emit(TRAP + 3, 0, 0, 0);

    /* user task (physical UB, linked at VB); second op runs past its bound */
    emit(UB + VB + 0, VB + 12, VB + 11, VB + 3);    /* Dc -= ONEc        [legal] */
    emit(UB + VB + 3, VB + 10, ST_LIMIT, VB + 6);   /* mem[LIMIT] -= Zc  [fault] */
    M[UB + VB + 10] = 0;    /* Zc   */
    M[UB + VB + 11] = 5;    /* Dc   */
    M[UB + VB + 12] = 1;    /* ONEc */

    /* boot: supervisor, MMU regs cleared so the SUBNEG code must set them */
    cpu.base = 0; cpu.limit = 0; cpu.vector = 0; cpu.saved_pc = 0;
    cpu.cause = FAULT_NONE; cpu.fault_addr = -1;
    cpu.mode = MODE_SUPER; pc = 200;

    run(1000);

    int ok = 1;
    printf("  scenario 'subneg supervisor: program MMU + RTE + trap-return':\n");
    #define CHECK(label, cond) do { \
        printf("    %-30s %s\n", label, (cond) ? "ok" : "FAIL"); \
        ok = ok && (cond); } while (0)
    CHECK("base set by subneg CR write",  cpu.base == UB);
    CHECK("limit set by subneg CR write", cpu.limit == ST_LIMIT);
    CHECK("RTE entered user (legal run)", M[UB + VB + 11] == 4);
    CHECK("user fault trapped back",      cpu.cause == FAULT_BOUNDS);
    CHECK("faulting vaddr",               cpu.fault_addr == ST_LIMIT);
    CHECK("handler read CR_CAUSE",        M[101] == -1);
    CHECK("victim untouched",             M[102] == ST_MAGIC);
    #undef CHECK
    return ok;
}

/* Phase 2 paging: a user task runs in mapped pages, touches an UNMAPPED page
 * (demand fault -> C pager maps it -> restart -> succeeds), then writes a
 * READ-ONLY page (permission fault -> killed). Exercises the page-table walk,
 * demand paging, and per-page write protection. */
static int paging_selftest(void) {
    const int32_t PTB = 0x1000, L2 = 0x1400;   /* L1 table, one L2 table (physical) */
    const int32_t CODE_FRAME = 8, DATA_FRAME = 9, RO_FRAME = 10;
    const int32_t CODE = CODE_FRAME * PAGE_WORDS;   /* 0x2000 */
    const int32_t DATA = DATA_FRAME * PAGE_WORDS;   /* 0x2400 */

    for (int i = 0; i < PAGE_WORDS; i++) { M[PTB + i] = 0; M[L2 + i] = 0; }
    M[PTB + 0] = L2;                          /* L1[0] -> L2 table */
    M[L2 + 1]  = (CODE_FRAME << 4) | 3;        /* vpn1 (code+data): present + writable */
    /* vpn2 left absent -> demand fault. vpn3 present + read-only: */
    M[L2 + 3]  = (RO_FRAME << 4) | 1;          /* present, NOT writable */

    /* task code lives in vpn1 (vaddr 0x400 -> phys CODE); cells in vpn1 too */
    M[CODE + 0x10] = 0;  M[CODE + 0x11] = -1;  M[CODE + 0x12] = 0;   /* CTR, NEG1, Z */
    emit(CODE + 0x00, 0x411, 0x410, 0x403);   /* CTR -= NEG1 => CTR++            [mapped]   */
    emit(CODE + 0x03, 0x411, 0x800, 0x406);   /* mem[0x800](vpn2) -= NEG1 => 1   [demand]   */
    emit(CODE + 0x06, 0x411, 0xC00, 0x409);   /* mem[0xC00](vpn3) -= NEG1        [RO fault] */
    emit(CODE + 0x09, 0x412, 0x412, 0);        /* halt (unreached: killed on RO write)      */

    cpu.ptb = PTB; cpu.base = 0; cpu.limit = 0;
    cpu.mode = MODE_USER; pc = USER_VA_MIN;
    cpu.cause = FAULT_NONE; cpu.fault_addr = -1;
    refresh_xlat();                          /* this scenario drives step() directly */

    int demand = 0, perm = 0, killed = 0;
    for (int guard = 0; guard < 1000; guard++) {
        int32_t start = pc;
        step_result_t r = step();
        if (r == STEP_HALT) break;
        if (r == STEP_FAULT) {
            if (cpu.cause != FAULT_PAGE) { killed = 1; break; }
            int32_t vpn = cpu.fault_addr >> PAGE_SHIFT_W;
            if (M[L2 + vpn] & 1) {            /* present -> permission violation -> kill */
                perm = 1; killed = 1; break;
            }
            M[L2 + vpn] = (DATA_FRAME << 4) | 3;   /* demand-map the page, then restart */
            demand++;
            pc = start;                       /* restartable fault: re-run the instruction */
            continue;
        }
    }

    cpu.ptb = 0;                              /* leave paging off for other tests */
    int ok = 1;
    printf("  scenario 'paging: demand page + write-protect':\n");
    #define CHECK(l, c) do { printf("    %-30s %s\n", l, (c) ? "ok" : "FAIL"); ok = ok && (c); } while (0)
    CHECK("mapped page executed",   M[CODE + 0x10] == 1);   /* CTR incremented */
    CHECK("demand fault serviced",  demand == 1);
    CHECK("demand page written",    M[DATA + 0] == 1);      /* the once-unmapped page now holds 1 */
    CHECK("write-protect fault",    perm == 1);
    CHECK("task killed on RO write", killed == 1);
    #undef CHECK
    return ok;
}

/* Step 9: a user program requests the kernel by jumping to the syscall-gate vaddr
 * (CR_SYSGATE). The VM must trap with CAUSE_SYSCALL, restartably (saved PC = gate), and
 * enter the supervisor at CR_VECTOR — the mechanism userspace will use instead of the
 * NOMMU direct call to __subleq_syscall. */
static int scenario_syscall(void) {
    const int32_t VB = USER_VA_MIN;
    const int32_t base = ST_UBASE;
    const int32_t GATE = VB + 3;                    /* user vaddr of the syscall gate */

    M[REG_INT_HANDLER] = 0;                          /* timer inert */
    emit(base + (VB + 0), VB + 10, VB + 11, VB + 3); /* Dc -= ONEc => 5-1=4>0, fall through to GATE */
    M[base + (VB + 10)] = 1;                         /* ONEc */
    M[base + (VB + 11)] = 5;                         /* Dc   */
    emit(ST_HANDLER, 0, 0, 0);                       /* supervisor: halt on entry (inspect after) */

    cpu.base = base;  cpu.limit = ST_LIMIT;  cpu.vector = ST_HANDLER;  cpu.sysgate = GATE;
    cpu.cause = FAULT_NONE;  cpu.fault_addr = -1;  cpu.saved_pc = -1;
    cpu.mode  = MODE_USER;   pc = VB;

    run(1000);
    cpu.sysgate = 0;                                 /* disable for other tests */

    int ok = 1;
    printf("  scenario 'user syscall gate -> CAUSE_SYSCALL trap':\n");
    #define CHECK(l, c) do { printf("    %-30s %s\n", l, (c) ? "ok" : "FAIL"); ok = ok && (c); } while (0)
    CHECK("legal instruction ran",   M[base + (VB + 11)] == 4);
    CHECK("syscall cause",           cpu.cause == CAUSE_SYSCALL);
    CHECK("trapped at gate (restart)", cpu.saved_pc == GATE);
    CHECK("entered supervisor",      cpu.mode == MODE_SUPER);
    #undef CHECK
    return ok;
}

static int selftest(void) {
    printf("lunatix self-test — MMU (user tasks linked at 0x%x bytes)\n", USER_VA_MIN * 4);
    int ok = 1;
    ok &= scenario("NULL / low guard", 0,        0);        /* base+bound: vaddr < USER_VA_MIN */
    ok &= scenario("above LIMIT",      ST_LIMIT,  ST_LIMIT); /* base+bound: vaddr >= limit      */
    ok &= scenario_rte();                                    /* subneg-driven MMU + RTE          */
    ok &= paging_selftest();                                 /* Phase 2: paging + demand faults  */
    ok &= scenario_syscall();                                /* Step 9: user syscall-gate trap   */
    printf("  RESULT ...................... %s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}

/* ===================== tiny subneg assembler (labels + backpatch) ===========
 * Lets demos (a scheduler!) be written in readable macro form instead of
 * hand-computed addresses. A first seed of the real Phase 0 assembler. */
enum { AS_LBLS = 32, AS_FIX = 512 };
static int32_t as_pc;
static int32_t as_lbl[AS_LBLS];
static struct { int32_t at; int lbl; } as_fixups[AS_FIX];
static int      as_nfix;
static int32_t  ZC, ONEc, Tc, T2c;          /* well-known cells, set by the demo */

static void as_org(int32_t org) {
    as_pc = org; as_nfix = 0;
    for (int i = 0; i < AS_LBLS; i++) as_lbl[i] = -1;
}
static void as_here(int id) { as_lbl[id] = as_pc; }
static void as_raw(int32_t a, int32_t b, int32_t c) {
    M[as_pc] = a * 4; M[as_pc + 1] = b * 4; M[as_pc + 2] = c * 4; as_pc += 3;
}
static void as_jraw(int32_t a, int32_t b, int lbl) {        /* c is a label, backpatched */
    M[as_pc] = a * 4; M[as_pc + 1] = b * 4; M[as_pc + 2] = 0;
    as_fixups[as_nfix].at = as_pc + 2; as_fixups[as_nfix].lbl = lbl; as_nfix++;
    as_pc += 3;
}
static void as_link(void) {
    for (int i = 0; i < as_nfix; i++) M[as_fixups[i].at] = as_lbl[as_fixups[i].lbl] * 4;
}
#define AS_NEXT (as_pc + 3)
static void as_sub(int32_t src, int32_t dst) { as_raw(src, dst, AS_NEXT); }   /* dst -= src  */
static void as_clr(int32_t x)                { as_raw(x, x, AS_NEXT); }        /* x = 0       */
static void as_mov(int32_t dst, int32_t src) { as_clr(dst); as_clr(Tc); as_sub(src, Tc); as_sub(Tc, dst); }
static void as_jmp(int lbl)                  { as_jraw(ZC, ZC, lbl); }         /* goto lbl    */
static void as_jlez(int32_t x, int lbl)      { as_jraw(ZC, x, lbl); }          /* if x<=0 goto*/
static void as_setcr(int32_t cr, int32_t v)  { as_raw(v, cr, AS_NEXT); }       /* CR := mem[v]*/
static void as_rte(void)                     { as_raw(ZC, CR_RTE, AS_NEXT); }  /* -> user     */
static void as_toggle(int32_t v)             { as_mov(T2c, ONEc); as_sub(v, T2c); as_mov(v, T2c); } /* v = 1-v */

/* ---------------------------------------------------------- two-task demo
 * A subneg supervisor round-robins two isolated user tasks on the timer.
 * Task A is well-behaved (counts forever). Task B counts a few times then
 * scribbles out of bounds -> trap -> the supervisor KILLS B and keeps running
 * A. Demonstrates preemptive multitasking + memory protection + isolation:
 * exactly the thing cable's NOMMU Linux (and the Amiga) could not do. */
static int demo_twotask(void) {
    /* supervisor cells (physical, identity-mapped) */
    ZC = 100; ONEc = 101; Tc = 102; T2c = 103;
    const int32_t causeT = 104, aliveT = 105, CUR = 106;
    const int32_t A_BASE = 110, A_LIMIT = 111, A_SPC = 112, A_ALIVE = 113;
    const int32_t B_BASE = 114, B_LIMIT = 115, B_SPC = 116, B_ALIVE = 117;
    const int32_t GUARD = 120, GUARD_MAGIC = 0x1234;
    const int32_t AUB = 0x2000, BUB = 0x3000, LIM = USER_VA_MIN + 0x40, ENTRY = USER_VA_MIN;

    M[ZC] = 0; M[ONEc] = 1; M[Tc] = 0; M[T2c] = 0; M[causeT] = 0; M[aliveT] = 0; M[CUR] = 0;
    M[A_BASE] = AUB; M[A_LIMIT] = LIM; M[A_SPC] = ENTRY; M[A_ALIVE] = 1;
    M[B_BASE] = BUB; M[B_LIMIT] = LIM; M[B_SPC] = ENTRY; M[B_ALIVE] = 1;
    M[GUARD] = GUARD_MAGIC; M[REG_INT_HANDLER] = 0;

    /* Task A (physical AUB, linked at vaddr ENTRY; base=AUB so paddr=vaddr+AUB):
     * CTR@0x410, Z@0x420, NEG1@0x421 — increment CTR forever. */
    M[AUB + 0x410] = 0; M[AUB + 0x420] = 0; M[AUB + 0x421] = -1;
    emit(AUB + 0x400, 0x421, 0x410, 0x403);   /* CTR -= (-1) => CTR++ ; ->0x403 */
    emit(AUB + 0x403, 0x420, 0x420, 0x400);   /* jmp 0x400                      */

    /* Task B (physical BUB): count down K, incrementing CTR, then scribble OOB.
     * CTR@0x410, Z@0x420, NEG1@0x421, ONE@0x422, K@0x423. */
    M[BUB + 0x410] = 0; M[BUB + 0x420] = 0; M[BUB + 0x421] = -1; M[BUB + 0x422] = 1; M[BUB + 0x423] = 4;
    emit(BUB + 0x400, 0x422, 0x423, 0x409);   /* K -= 1 ; if K<=0 goto WILD(0x409) */
    emit(BUB + 0x403, 0x421, 0x410, 0x406);   /* CTR++ ; ->0x406                   */
    emit(BUB + 0x406, 0x420, 0x420, 0x400);   /* jmp 0x400                         */
    emit(BUB + 0x409, 0x420, 0x600, 0x40C);   /* WILD: mem[0x600] -= 0 ; 0x600>=LIMIT -> FAULT */

    /* Scheduler (subneg supervisor) */
    enum { L_START, L_SCHED, L_SCA, L_PICK, L_PKA, L_BACK, L_LOAD, L_LDA, L_RTE };
    as_org(300);
    as_here(L_START);
      as_jmp(L_LOAD);                          /* boot: launch the current task */
    as_here(L_SCHED);                          /* trap entry (CR_VECTOR) */
      as_jlez(CUR, L_SCA);                     /* CUR==0 -> task A, else task B */
      as_mov(B_SPC, CR_SAVED_PC);              /* save B's resume pc */
      as_mov(causeT, CR_CAUSE);
      as_jlez(causeT, L_PICK);                 /* cause 0 = timer -> don't kill */
      as_clr(B_ALIVE);                         /* fault -> kill B */
      as_jmp(L_PICK);
    as_here(L_SCA);
      as_mov(A_SPC, CR_SAVED_PC);
      as_mov(causeT, CR_CAUSE);
      as_jlez(causeT, L_PICK);
      as_clr(A_ALIVE);
    as_here(L_PICK);
      as_toggle(CUR);                          /* CUR = 1 - CUR */
      as_jlez(CUR, L_PKA);                      /* chosen A? */
      as_mov(aliveT, B_ALIVE); as_jlez(aliveT, L_BACK);  /* chosen B dead -> revert */
      as_jmp(L_LOAD);
    as_here(L_PKA);
      as_mov(aliveT, A_ALIVE); as_jlez(aliveT, L_BACK);
      as_jmp(L_LOAD);
    as_here(L_BACK);
      as_toggle(CUR);                          /* revert to the surviving task */
    as_here(L_LOAD);
      as_jlez(CUR, L_LDA);
      as_setcr(CR_BASE, B_BASE); as_setcr(CR_LIMIT, B_LIMIT); as_setcr(CR_SAVED_PC, B_SPC);
      as_jmp(L_RTE);
    as_here(L_LDA);
      as_setcr(CR_BASE, A_BASE); as_setcr(CR_LIMIT, A_LIMIT); as_setcr(CR_SAVED_PC, A_SPC);
    as_here(L_RTE);
      as_rte();
    as_link();

    cpu.vector = as_lbl[L_SCHED];
    cpu.base = 0; cpu.limit = 0; cpu.saved_pc = 0; cpu.cause = FAULT_NONE; cpu.fault_addr = -1;
    cpu.mode = MODE_SUPER; pc = as_lbl[L_START];
    user_quantum = 25;

    uint64_t steps = run(3000);
    user_quantum = 0;

    int32_t actr = M[AUB + 0x410], bctr = M[BUB + 0x410];
    int ok = 1;
    printf("lunatix two-task preemptive demo (timer quantum = 25 steps, cap = 3000)\n");
    printf("  steps run ................... %llu\n", (unsigned long long)steps);
    printf("  task A counter .............. %d\n", actr);
    printf("  task B counter .............. %d\n", bctr);
    #define CHECK(l, c) do { printf("  %-28s %s\n", l, (c) ? "ok" : "FAIL"); ok = ok && (c); } while (0)
    CHECK("task A kept running",     actr > 50);
    CHECK("task B ran then stopped", bctr >= 1 && bctr < 10);
    CHECK("task B killed on fault",  M[B_ALIVE] == 0);
    CHECK("task A still alive",      M[A_ALIVE] == 1);
    CHECK("A ran far more than B",   actr > bctr * 5);
    CHECK("supervisor guard intact", M[GUARD] == GUARD_MAGIC);
    #undef CHECK
    printf("  RESULT ...................... %s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}

/* ---------------------------------------------------------------------- main */

int main(int argc, char **argv) {
    M = calloc(MEM_WORDS, sizeof *M);
    if (!M) { fprintf(stderr, "lunatix: cannot allocate %d words\n", MEM_WORDS); return 1; }

    cpu.mode = MODE_SUPER;

    if (argc > 1 && strcmp(argv[1], "--selftest") == 0)
        return selftest();
    if (argc > 1 && strcmp(argv[1], "--demo") == 0)
        return demo_twotask();

    /* Headless benchmark: `--bench [steps]` loads the image and runs a bounded
     * number of steps, printing the achieved rate. Same on native and wasm. */
    if (argc > 1 && strcmp(argv[1], "--bench") == 0) {
        uint64_t cap = (argc > 2) ? strtoull(argv[2], NULL, 10) : 1000000000ULL;
        load_image();
        struct timespec t0, t1;
        timespec_get(&t0, TIME_UTC);
        uint64_t did = run(cap);
        timespec_get(&t1, TIME_UTC);
        double secs = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;
        fprintf(stderr, "bench: %llu steps in %.3fs = %.1f Msteps/s%s\n",
                (unsigned long long) did, secs, did / secs / 1e6,
                did < cap ? " (HALTED early)" : "");
        return 0;
    }

    load_image();

#ifdef __EMSCRIPTEN__
    /* Set up the canvas presenter, then return: the worker drives execution via
     * em_run_slice() so the event loop can composite frames and read input.
     * M is intentionally not freed (the module stays resident for the slices). */
    em_video_init(FB_W, FB_H);
    pc = 0;
    return 0;
#else
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        fprintf(stderr, "lunatix: SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }
    window  = SDL_CreateWindow("lunatix", FB_W, FB_H, 0);
    surface = SDL_GetWindowSurface(window);
    if (!window || !surface) {
        fprintf(stderr, "lunatix: SDL window/surface failed: %s\n", SDL_GetError());
        return 1;
    }

    pc = 0;
    run(0);

    SDL_DestroyWindow(window);
    SDL_Quit();
    free(M);
    return 0;
#endif
}
