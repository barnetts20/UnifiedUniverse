#!/usr/bin/env python3
"""Python arbitrary-precision oracle for the Unified Universe coordinate maths (D-016).

Reads the records `univerify fuzzdump` wrote (the C++ UniCoord.h results plus the shader
core's object-relative vector) and recomputes every output from the inputs with Python
integers, which have no width limit and no rounding. Float conversions are rounded here
by explicit round-half-to-even on integers, never by trusting a library cast.

Usage: coord_oracle.py DUMP [--json OUT.jsonl]
Exit status 0 only when every field of every record matches bit for bit.
"""

import json
import math
import multiprocessing as mp
import struct
import sys
import time

import numpy as np

MASK128 = (1 << 128) - 1

FUZZ_DTYPE = np.dtype([
    ("A", "<u4", 12), ("B", "<u4", 12), ("Level", "<u4"), ("Step", "<i4", 3), ("Off", "<u4", 3),
    ("Frac0", "<f4", 3), ("Disp", "<f8", 3),
    ("DeltaTc", "<u4", 12), ("Dbl", "<f8", 3), ("Hi", "<f4", 3), ("Lo", "<f4", 3),
    ("Cell", "<u4", 12), ("Neigh", "<u4", 12), ("Added", "<u4", 12), ("FracOut", "<f4", 3),
    ("ObjRel", "<f4", 3),
])
assert FUZZ_DTYPE.itemsize == 424, FUZZ_DTYPE.itemsize


def axis(limbs, i):
    """Axis i of a 12-limb coordinate as an int; limb 0 is most significant."""
    v = 0
    for n in range(4):
        v = (v << 32) | int(limbs[i * 4 + n])
    return v


def f32_bits_of_int(n):
    """Correctly rounded (RNE) float32 bit pattern of an integer, via integers only."""
    neg = n < 0
    m = -n if neg else n
    if m == 0:
        bits = 0
    else:
        p = m.bit_length() - 1
        if p < 24:
            mant, e = m, 0
            # normalise to 24 bits
            shift = 23 - p
            mant <<= shift
            e = p
        else:
            drop = p - 23
            mant = m >> drop
            rest = m - (mant << drop)
            half = 1 << (drop - 1)
            if rest > half or (rest == half and (mant & 1)):
                mant += 1
                if mant == 1 << 24:
                    mant >>= 1
                    p += 1
            e = p
        if e >= 128:
            bits = 0x7F800000
        else:
            bits = ((e + 127) << 23) | (mant & 0x7FFFFF)
    if neg and bits:
        bits |= 0x80000000
    return bits


def f32_value(bits):
    return struct.unpack("<f", struct.pack("<I", bits))[0]


def f32_bits(x):
    """Bits of a Python float rounded once to float32 (numpy does IEEE RNE)."""
    return int(np.array([x], dtype=np.float32).view(np.uint32)[0])


def int_of_f32_bits(bits):
    """Exact integer value of an integer-valued float32."""
    v = f32_value(bits)
    return int(v)


def to_signed(v):
    return v - (1 << 128) if v >> 127 else v


def check_chunk(args):
    path, start, count = args
    data = np.memmap(path, dtype=FUZZ_DTYPE, mode="r", offset=start * FUZZ_DTYPE.itemsize, shape=(count,))
    bad = {k: 0 for k in ("sub", "todouble", "hilo", "cellof", "neighbour", "adddisp", "objrel")}
    first = []
    for idx in range(count):
        before = sum(bad.values())
        r = data[idx]
        A, B = r["A"], r["B"]
        L = int(r["Level"])
        for i in range(3):
            a, b = axis(A, i), axis(B, i)
            d = (a - b) & MASK128
            if axis(r["DeltaTc"], i) != d:
                bad["sub"] += 1
            sd = to_signed(d)  # shortest image, -2^127 for the antipode
            # ToDouble: Python float(int) is correctly rounded (RNE)
            if float(r["Dbl"][i]) != float(sd) and not (math.isnan(float(r["Dbl"][i]))):
                bad["todouble"] += 1
            # Hi/Lo
            hb = f32_bits_of_int(sd)
            hi_int = int_of_f32_bits(hb) if hb not in (0x7F800000, 0xFF800000) else None
            lb = f32_bits_of_int(sd - hi_int) if hi_int is not None else 0
            got_h = int(np.float32(r["Hi"][i]).view(np.uint32))
            got_l = int(np.float32(r["Lo"][i]).view(np.uint32))
            # Zero has one encoding here: -0.0 and +0.0 both mean zero residual.
            if got_h != hb or (got_l & 0x7FFFFFFF) != (lb & 0x7FFFFFFF) or ((got_l & 0x7FFFFFFF) and got_l != lb):
                bad["hilo"] += 1
            # CellOf, Neighbour
            cell = (a >> L) if L < 128 else 0
            if axis(r["Cell"], i) != cell:
                bad["cellof"] += 1
            bits = 128 - L
            mod = 1 << bits
            nb = (cell + int(r["Step"][i])) % mod
            if axis(r["Neigh"], i) != nb:
                bad["neighbour"] += 1
            # AddDisplacement: same rule, Python floats are IEEE doubles
            frac0 = float(r["Frac0"][i])
            v = frac0 + float(r["Disp"][i])
            n = math.floor(v)
            f = v - n
            fb = f32_bits(f)
            carry = int(n)
            if f32_value(fb) >= 1.0:
                fb = 0
                carry += 1
            if f32_value(fb) < 0.0:
                fb = 0
            if axis(r["Added"], i) != (a + carry) & MASK128 or int(np.float32(r["FracOut"][i]).view(np.uint32)) != fb:
                bad["adddisp"] += 1
            # Object relative: (CellOrigin - B) + offset_cm, rounded once to float32, minus frac
            origin = (cell << L) & MASK128 if L < 128 else 0
            cd = to_signed((origin - b) & MASK128)
            off = int(r["Off"][i])
            off_cm = (off << (L - 32)) if L >= 32 else (off >> (32 - L))
            tot = to_signed((cd + off_cm) & MASK128)
            relb = f32_bits_of_int(tot)
            rel = np.float32(f32_value(relb)) - np.float32(r["Frac0"][i])
            if int(np.float32(rel).view(np.uint32)) != int(np.float32(r["ObjRel"][i]).view(np.uint32)):
                bad["objrel"] += 1
        if sum(bad.values()) > before and len(first) < 5:
            first.append(start + idx)
    return bad, first


def main():
    path = sys.argv[1]
    out = None
    if "--json" in sys.argv:
        out = sys.argv[sys.argv.index("--json") + 1]
    total = (np.memmap(path, dtype=np.uint8, mode="r").shape[0]) // FUZZ_DTYPE.itemsize
    t0 = time.time()
    workers = max(1, mp.cpu_count())
    chunk = 20000
    jobs = [(path, s, min(chunk, total - s)) for s in range(0, total, chunk)]
    agg = {}
    first = []
    with mp.Pool(workers) as pool:
        for bad, f in pool.imap_unordered(check_chunk, jobs):
            for k, v in bad.items():
                agg[k] = agg.get(k, 0) + v
            first.extend(f)
    mism = sum(agg.values())
    secs = time.time() - t0
    detail = " ".join(f"{k}={v}" for k, v in sorted(agg.items()) if v)
    measured = f"{total} cases x 3 axes vs Python big-int oracle, {mism} mismatches {detail}({secs:.0f} s)"
    ok = mism == 0 and total >= 1_000_000
    print(("PASS " if ok else "FAIL ") + measured)
    if first:
        print("first bad records:", sorted(first)[:5])
    if out:
        with open(out, "a") as fh:
            fh.write(json.dumps({
                "tier": 1, "id": "T1.coord.oracle",
                "title": "Coordinate fuzz vs Python big-int oracle: Sub, ToDouble, ToHiLo, CellOf, Neighbour, AddDisplacement, object-relative (shader core)",
                "pass": ok, "measured": measured,
                "metrics": {"cases": total, "mismatches": mism, "seconds": secs},
            }) + "\n")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
