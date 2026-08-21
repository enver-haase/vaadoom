#!/usr/bin/env python3
"""Compare a Vaadoom music render against the score it was playing, and against
a reference rendering of the same score by another DOOM port.

    python3 compare-music.py <ours.wav> <ours-trace.json> <IWAD> <lump> [reference.wav]

Reports the three things that have actually gone wrong at one time or another:
how many of the score's notes we play, whether we leave gaps the score does not
have, and how bright the result is next to the reference.
"""
import json, math, struct, sys

def wav(path):
    d = open(path, 'rb').read()
    rate = struct.unpack_from('<I', d, 24)[0]
    off = 12
    while off < len(d) - 8:
        cid = d[off:off+4]; sz = struct.unpack_from('<I', d, off+4)[0]
        if cid == b'data':
            break
        off += 8 + sz + (sz & 1)
    import numpy as np
    x = np.frombuffer(d, dtype='<i2', count=sz // 2, offset=off + 8)[0::2].astype(float)
    return rate, x

def genmidi_flags(wad_path):
    """Per-instrument GENMIDI flags, so we know which instruments are two voices."""
    d = open(wad_path, 'rb').read()
    n, ofs = struct.unpack_from('<ii', d, 4)
    for i in range(n):
        fp, sz = struct.unpack_from('<ii', d, ofs + i * 16)
        if d[ofs+i*16+8:ofs+i*16+16].split(b'\0')[0] == b'GENMIDI':
            g = d[fp:fp+sz]
            return [struct.unpack_from('<H', g, 8 + k * 36)[0] for k in range(175)]
    return []

def score_notes(wad_path, lump):
    d = open(wad_path, 'rb').read()
    n, ofs = struct.unpack_from('<ii', d, 4)
    ent = {}
    for i in range(n):
        fp, sz = struct.unpack_from('<ii', d, ofs + i * 16)
        nm = d[ofs+i*16+8:ofs+i*16+16].split(b'\0')[0].decode()
        ent[nm] = (fp, sz)
    fp, sz = ent[lump]; s = d[fp:fp+sz]
    slen, sstart = struct.unpack_from('<HH', s, 4)
    p, end, t = sstart, sstart + slen, 0
    ons = []; prog = {c: 0 for c in range(16)}
    while p < end:
        ev = s[p]; p += 1
        last, typ, ch = ev & 0x80, (ev >> 4) & 7, ev & 0xF
        if typ == 0: p += 1
        elif typ == 1:
            nb = s[p]; p += 1
            if nb & 0x80: p += 1
            ons.append((round(t / 140 * 1000), ch, nb & 0x7F, prog[ch]))
        elif typ in (2, 3): p += 1
        elif typ == 4:
            if s[p] == 0: prog[ch] = s[p+1]
            p += 2
        elif typ == 6: break
        if last:
            delay = 0
            while True:
                b = s[p]; p += 1
                delay = (delay << 7) | (b & 0x7F)
                if not (b & 0x80): break
            t += delay
    return ons

def trace_notes(path):
    """Note-on times, measured from the moment the song being compared started.

    A flush (register 0x1FF) is the guest telling the VM to drop what is queued,
    which it does exactly when a new song begins. Anything before the last flush
    belongs to a previous piece — the title music, typically — and counting it
    against this score inflates the note count (once seen: 107% of a score, which
    is not us inventing notes but us also playing D_INTRO).
    """
    log = json.load(open(path))
    flushes = [t for t, reg, val, dt in log if reg == 0x1FF]
    song_start = max(flushes) if flushes else min(t for t, _, _, _ in log)
    log = [e for e in log if e[0] >= song_start]
    ev = sorted((t + dt, reg, val) for t, reg, val, dt in log)
    t0 = ev[0][0]
    fn = {}; notes = []
    for ti, reg, val in ev:
        if 0xA0 <= reg <= 0xA8 or 0x1A0 <= reg <= 0x1A8:
            fn[reg & 0x1FF] = val
        elif (0xB0 <= reg <= 0xB8 or 0x1B0 <= reg <= 0x1B8) and (val & 0x20):
            notes.append(ti - t0)
    return notes

def brightness(path):
    import numpy as np
    rate, x = wav(path)
    seg = x[int(rate*10):int(rate*20)]
    seg = seg - seg.mean()
    N = 1 << 15
    acc = np.zeros(N // 2 + 1); hops = 0
    for i in range(0, max(len(seg) - N, 1), N // 2):
        acc += np.abs(np.fft.rfft(seg[i:i+N] * np.hanning(N))); hops += 1
    acc /= max(hops, 1)
    f = np.fft.rfftfreq(N, 1 / rate)
    return (acc * f).sum() / acc.sum(), acc[f > 3000].sum() / acc.sum()

ours_wav, ours_trace, iwad, lump = sys.argv[1:5]
ref = sys.argv[5] if len(sys.argv) > 5 else None

ons = score_notes(iwad, lump)
mine = trace_notes(ours_trace)
span = max(mine) / 1000
in_span = [o for o in ons if o[0] <= max(mine)]

# One score note is not always one OPL key-on: GENMIDI marks some instruments as
# two voices (flag bit 2) and those are keyed twice, by design. Counting raw
# key-ons against note events therefore reads over 100% — E1M1's snare alone
# accounts for it. Work out what the score should produce instead.
flags = genmidi_flags(iwad)
def instr_of(ch, note, prog):
    return 128 + note - 35 if ch == 15 else prog
expected = 0
for t_, ch, note, prog in in_span:
    idx = instr_of(ch, note, prog)
    expected += 2 if (0 <= idx < len(flags) and (flags[idx] & 0x04)) else 1

print(f"score {lump}: {len(in_span)} notes in the first {span:.0f}s = {len(in_span)/span:.1f}/s"
      f"  -> {expected} OPL key-ons expected ({expected-len(in_span)} of them second voices)")
print(f"           (compared from the song's own start, i.e. after the last flush)")
print(f"ours       : {len(mine)} key-ons             = {len(mine)/span:.1f}/s"
      f"   ({100*len(mine)/max(expected,1):.0f}% of what the score asks for)")

sg = sorted((in_span[i+1][0] - in_span[i][0] for i in range(len(in_span)-1)), reverse=True)
mg = sorted((mine[i+1] - mine[i] for i in range(len(mine)-1)), reverse=True)
print(f"largest gap between note-ons: score {sg[0]} ms, ours {mg[0]} ms"
      f"  {'OK' if mg[0] <= sg[0] * 1.5 else '<-- we leave a hole the score does not have'}")

c, hi = brightness(ours_wav)
print(f"ours       : centroid {c:.0f} Hz, energy >3kHz {100*hi:.1f}%")
if ref:
    c2, hi2 = brightness(ref)
    print(f"reference  : centroid {c2:.0f} Hz, energy >3kHz {100*hi2:.1f}%"
          f"   (we are {100*(hi-hi2)/hi2:+.0f}% relative)")
