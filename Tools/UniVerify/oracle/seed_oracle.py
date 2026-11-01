#!/usr/bin/env python3
"""Python oracle for UniSeed: Avalanche, HashCombine (both forms), MixSeed, FRandomStream,
CellSeed, ObjectSeed, counter draws and ChannelId, recomputed from the inputs that
`univerify seeddump` prints. Usage: seed_oracle.py UNIVERIFY_BINARY [--json OUT]
"""

import json
import struct
import subprocess
import sys

M = 0xFFFFFFFF


def av(x):
    x &= M
    x ^= x >> 16
    x = (x * 0x85EBCA6B) & M
    x ^= x >> 13
    x = (x * 0xC2B2AE35) & M
    x ^= x >> 16
    return x


def hc_jenkins(a, c):
    a &= M; c &= M; b = 0x9E3779B9
    a = (a + b) & M
    a = (a - b - c) & M; a ^= c >> 13
    b = (b - c - a) & M; b ^= (a << 8) & M
    c = (c - a - b) & M; c ^= b >> 13
    a = (a - b - c) & M; a ^= c >> 12
    b = (b - c - a) & M; b ^= (a << 16) & M
    c = (c - a - b) & M; c ^= b >> 5
    a = (a - b - c) & M; a ^= c >> 3
    b = (b - c - a) & M; b ^= (a << 10) & M
    c = (c - a - b) & M; c ^= b >> 15
    return c


def hc_fast(a, c):
    return (c ^ ((a + 0x9E3779B9 + ((c << 6) & M) + (c >> 2)) & M)) & M


def fnv_upper(s):
    h = 2166136261
    for ch in s.upper().encode():
        h ^= ch
        h = (h * 16777619) & M
    return h


def seedmix(h, v):
    return av(h ^ av(v ^ 0x9E3779B9))


def main():
    exe = sys.argv[1]
    out = sys.argv[sys.argv.index("--json") + 1] if "--json" in sys.argv else None
    text = subprocess.run([exe, "seeddump"], capture_output=True, text=True).stdout
    lines = [json.loads(l) for l in text.splitlines() if l.strip()]
    fast = lines[0]["variant_fast"]
    hc = hc_fast if fast else hc_jenkins
    bad = 0
    n = 0
    for r in lines[1:]:
        if "name" in r:
            n += 1
            bad += fnv_upper(r["name"]) != r["channel"]
            continue
        n += 1
        a, c, idx = r["a"], r["c"], r["idx"]
        mix = av(hc(a, c)) & 0x7FFFFFFF
        ok = av(a) == r["av"] and hc(a, c) == r["hc"] and mix == r["mix"]
        ok &= (av(hc(a, hc(c, idx))) & 0x7FFFFFFF) == r["mixi"]
        s = mix
        for k in range(4):
            s = (s * 196314165 + 907633515) & M
            f = struct.unpack("<f", struct.pack("<I", 0x3F800000 | (s >> 9)))[0] - 1.0
            fb = struct.unpack("<I", struct.pack("<f", f))[0]
            ok &= fb == r["fr"][k]
        h = seedmix(a, 0x55C0FFEE)
        for v in [r["L"]] + r["X"] + r["Y"] + r["Z"] + [c & 0xFFFF, idx, (n - 1) & 7]:
            h = seedmix(h, v)
        cell = av(h)
        ok &= cell == r["cell"]
        ok &= seedmix(seedmix(cell, 0x1B2E0F7A), idx) == r["obj"]
        ok &= seedmix(seedmix(cell, c), idx) == r["draw"]
        bad += 0 if ok else 1
    measured = f"{n} cases vs Python oracle ({'fast' if fast else 'Jenkins'} HashCombine), {bad} mismatches"
    ok = bad == 0 and n > 1000
    print(("PASS " if ok else "FAIL ") + measured)
    if out:
        with open(out, "a") as fh:
            fh.write(json.dumps({"tier": 1, "id": "T1.seed.oracle",
                                 "title": "UniSeed vs Python oracle: Avalanche, HashCombine, MixSeed, FRandomStream, CellSeed, ObjectSeed, draws, ChannelId",
                                 "pass": ok, "measured": measured, "metrics": {"cases": n, "mismatches": bad}}) + "\n")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
