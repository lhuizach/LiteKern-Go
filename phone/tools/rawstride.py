#!/usr/bin/env python3
"""LiteKern Go, phone: find the true row length of a raw frame.

    python3 phone/tools/rawstride.py IN.raw [MIN MAX]

A sheared picture means the rows aren't as long as we said. Neighbouring rows look
alike, so the byte offset where the data best matches itself is the row length.
"""
import sys

import numpy as np

d = np.fromfile(sys.argv[1], dtype=np.uint8).astype(np.float32)
lo = int(sys.argv[2]) if len(sys.argv) > 2 else 800
hi = int(sys.argv[3]) if len(sys.argv) > 3 else 4000
d -= d.mean()
scores = []
for s in range(lo, hi):
    a, b = d[:-s], d[s:]
    scores.append((float((a * b).mean()), s))
scores.sort(reverse=True)
for score, s in scores[:10]:
    print("%6d bytes  %.1f" % (s, score))
