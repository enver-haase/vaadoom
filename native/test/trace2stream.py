#!/usr/bin/env python3
"""Turn a Vaadoom OPL trace into the exact stream of applied register writes.

    trace2stream.py <render-trace.json> <stream.txt>

Trace entry: [t_ms, reg, val, dt_ms, at] — `at` is the sample the device applied the
write at (opl.generated for an immediate one, opl_sched[].at for a scheduled one).
Feed the result to opl-replay.c and a native build renders exactly what the engine
played, sample for sample.

What needs modelling is the queue in em_devices.h, because two of its properties are
not visible in a per-write timestamp:

  * A FLUSH (reg 0x1FF) throws away every queued write that has not been applied
    yet. The key-offs the device issues in its place are traced like any other write
    and need no special handling.

  * The queue is a FIFO that is drained from the head, so a write is applied at
    max(its own `at`, the position where the entry ahead of it was applied) — an
    entry whose turn has not come holds up entries behind it even if their sample
    has passed. Sorting by `at` instead gets a song change wrong by a few hundred
    milliseconds, which is exactly where the interesting bugs live.

Immediate writes (dt 0) bypass the queue, so they also tell us how far the stream had
been generated: everything queued for an earlier sample must have gone in before them.
"""
import json, sys

FLUSH = 0x1FF

log = json.load(open(sys.argv[1]))
applied, queue = [], []
seq = 0
qtime = 0            # where the last queue entry was applied


def drain(upto):
    """Apply every head of the queue whose sample has been reached."""
    global qtime
    while queue and queue[0][0] <= upto:
        at, s, reg, val = queue.pop(0)
        qtime = max(at, qtime)
        applied.append((qtime, s, reg, val))


for t, reg, val, dt, at in log:
    at = int(at)
    if reg == FLUSH:
        drain(at)
        queue.clear()
        continue
    if dt == 0:                       # immediate: bypasses the queue
        drain(at)
        qtime = max(qtime, at)
        applied.append((at, seq, reg, val))
    else:
        queue.append((at, seq, reg, val))
    seq += 1
drain(float('inf'))

applied.sort(key=lambda p: (p[0], p[1]))
with open(sys.argv[2], 'w') as f:
    for at, _, reg, val in applied:
        f.write('%d %d %d\n' % (at, reg, val))

print('%d trace entries -> %d applied writes, span %.2f s'
      % (len(log), len(applied), applied[-1][0] / 49716))
