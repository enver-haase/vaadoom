/*
 * opl-replay.c — deterministic OPL replay harness.
 *
 *   ../build.sh replay                     # -> ./opl-replay
 *   ./opl-replay <stream.txt> <out.wav> <seconds> [buffered|direct]
 *
 * Reads a register stream ("<sample> <reg> <val>" per line, sample = absolute frame
 * index at 49716 Hz) and renders it through the SAME opl_shim.c + Nuked-OPL3 the
 * engine uses, writing a 16-bit stereo WAV. No wall clock and no emulator, so for
 * one input file the output depends on nothing but the compiler.
 *
 * Two things it is for:
 *
 *   • Replaying what the engine actually played. render-music.mjs writes a trace
 *     next to its WAV; trace2stream.py turns that into a stream file, and the
 *     replay of it reproduces the engine's audio sample for sample (verified over
 *     1988219 samples). A 115 s rendering takes 115 s of real time because the
 *     guest paces itself off the wall clock; replaying its trace takes a second,
 *     which is what makes measuring a fix practical.
 *
 *   • Comparing compilers. Built with emcc instead of cc, from the same sources,
 *     the output is byte-identical — so Emscripten is not a variable in anything
 *     the music does:
 *
 *       emcc -O3 -fwrapv -I.. opl-replay.c ../opl_shim.c ../third_party/nuked/opl3.c \
 *            -o opl-replay.js -sENVIRONMENT=node -sNODERAWFS=1 -sEXIT_RUNTIME=1
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "opl_shim.h"

#define RATE 49716
#define CHUNK 4096

static int16_t buf[CHUNK * 2];
static FILE *out;
static uint64_t written;

static void emit(uint64_t frames) {
    while (frames) {
        int n = frames > CHUNK ? CHUNK : (int) frames;
        opl_generate(buf, n);
        fwrite(buf, 4, (size_t) n, out);
        written += (uint64_t) n;
        frames -= (uint64_t) n;
    }
}

int main(int argc, char **argv) {
    if (argc < 4) { fprintf(stderr, "usage: opl-replay <stream.txt> <out.wav> <seconds> [buffered|direct]\n"); return 2; }
    FILE *in = fopen(argv[1], "r");
    if (!in) { perror(argv[1]); return 1; }
    out = fopen(argv[2], "wb");
    if (!out) { perror(argv[2]); return 1; }
    uint64_t total = (uint64_t) (atof(argv[3]) * RATE);
    int buffered = (argc < 5 || strcmp(argv[4], "direct") != 0);

    char hdr[44]; memset(hdr, 0, sizeof hdr);
    fwrite(hdr, 1, sizeof hdr, out);           /* patched at the end */

    opl_reset(RATE);
    uint64_t cur = 0, at; unsigned reg, val; long long line = 0;
    while (fscanf(in, "%llu %u %u", (unsigned long long *) &at, &reg, &val) == 3) {
        line++;
        if (at > total) break;
        if (at > cur) { emit(at - cur); cur = at; }
        if (buffered) opl_write_buffered((uint16_t) reg, (uint8_t) val);
        else          opl_write((uint16_t) reg, (uint8_t) val);
    }
    if (total > cur) emit(total - cur);
    fclose(in);

    uint32_t bytes = (uint32_t) (written * 4);
    memcpy(hdr, "RIFF", 4);
    uint32_t riff = 36 + bytes; memcpy(hdr + 4, &riff, 4);
    memcpy(hdr + 8, "WAVEfmt ", 8);
    uint32_t sz16 = 16; memcpy(hdr + 16, &sz16, 4);
    uint16_t pcm = 1, ch = 2, bits = 16, align = 4;
    memcpy(hdr + 20, &pcm, 2); memcpy(hdr + 22, &ch, 2);
    uint32_t rate = RATE, brate = RATE * 4;
    memcpy(hdr + 24, &rate, 4); memcpy(hdr + 28, &brate, 4);
    memcpy(hdr + 32, &align, 2); memcpy(hdr + 34, &bits, 2);
    memcpy(hdr + 36, "data", 4); memcpy(hdr + 40, &bytes, 4);
    fseek(out, 0, SEEK_SET); fwrite(hdr, 1, sizeof hdr, out); fclose(out);
    fprintf(stderr, "replayed %lld writes -> %s: %.2f s (%llu frames)\n",
            line, argv[2], (double) written / RATE, (unsigned long long) written);
    return 0;
}
