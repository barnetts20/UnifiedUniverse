#!/usr/bin/env python3
"""Images for `univerify galslices` (the sliced galaxy-map prototype).

    python3 -I Tools/UniVerify/galslices_report.py DIR

Reads DIR/metrics.json and the float images the command wrote, and writes PNGs next to them:
  xsec_<tag>.png          vertical cross-sections (x along the disc, height about its centre, log light),
  view_<name>_<tag>.png   the two renders inside / just above the home disc (log light),
  diff_<name>_<tag>.png   each render's relative error against the field's (blue low, red high),
  panel_home.png          labelled cross-sections: the field, today's map, 1 / 4 / 8 / 16 slices,
  panel_changed.png       labelled cross-sections of each changed warp's field and its 4 / 8 slices.
Every image of a kind shares the full-warp field's colour scale, so they compare directly.
"""
import json
import os
import sys

import numpy as np
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt


def load(path, w, h):
    return np.fromfile(path, dtype=np.float32).reshape(h, w)


def logimg(img, lo, hi):
    return np.clip(np.log10(np.maximum(img, 10.0 ** lo)), lo, hi)


def save_log(img, path, lo, hi, cmap):
    plt.imsave(path, logimg(img, lo, hi), vmin=lo, vmax=hi, cmap=cmap)


def panel(rows, path, lo, hi, w, h):
    """rows: [(title, f32 path)]; one labelled cross-section per row."""
    rows = [r for r in rows if os.path.exists(r[1])]
    if not rows:
        return
    fig, axes = plt.subplots(len(rows), 1, figsize=(9.0, 1.55 * len(rows)), dpi=110)
    if len(rows) == 1:
        axes = [axes]
    for ax, (title, p) in zip(axes, rows):
        ax.imshow(logimg(load(p, w, h), lo, hi), cmap="magma", vmin=lo, vmax=hi, aspect="auto", interpolation="nearest")
        ax.set_title(title, fontsize=9, loc="left", pad=3)
        ax.set_xticks([])
        ax.set_yticks([])
    fig.tight_layout(h_pad=0.6)
    fig.savefig(path)
    plt.close(fig)


def main():
    d = sys.argv[1]
    m = json.load(open(os.path.join(d, "metrics.json")))
    xw, xh = m["images"]["xsec"][0], m["images"]["xsec"][1]
    vw, vh = m["images"]["view"]
    home = m["galaxies"][0]
    tags = [(v["tag"], v["name"], v) for v in home["variants"] if "tag" in v]

    ref = load(os.path.join(d, "xsec_field.f32"), xw, xh)
    hi = float(np.log10(np.percentile(ref[ref > 0], 99.9)))
    lo = hi - 3.0
    save_log(ref, os.path.join(d, "xsec_field.png"), lo, hi, "magma")
    for tag, _, _ in tags:
        p = os.path.join(d, f"xsec_{tag}.f32")
        if os.path.exists(p):
            save_log(load(p, xw, xh), os.path.join(d, f"xsec_{tag}.png"), lo, hi, "magma")

    def pct(v):
        return f"{100.0 * v:.1f}%"

    rows = [("Field (reference)", os.path.join(d, "xsec_field.f32"))]
    for tag, name, v in tags:
        if v["size"] == 1024 and v["relative"] and v["jitter"] == 0 and v["slices"] in (0, 1, 4, 8, 16) and "4H" not in name:
            rows.append((f"{name}: per-point error {pct(v['l1'])}", os.path.join(d, f"xsec_{tag}.f32")))
    panel(rows, os.path.join(d, "panel_home.png"), lo, hi, xw, xh)

    rows = []
    for gi, g in enumerate(m["galaxies"]):
        fp = os.path.join(d, f"xsec_g{gi}_field.f32")
        if not os.path.exists(fp):
            continue
        rows.append((f"{g['name']}: field (changes the look {pct(g['look_l1'])})", fp))
        for v in g["variants"]:
            if "tag" in v:
                rows.append((f"{g['name']}: {v['name']}, per-point error {pct(v['l1'])}", os.path.join(d, f"xsec_{v['tag']}.f32")))
    panel(rows, os.path.join(d, "panel_changed.png"), lo, hi, xw, xh)

    for view in ("in_disc", "low_over"):
        rp = os.path.join(d, f"view_{view}_field.f32")
        if not os.path.exists(rp):
            continue
        r = load(rp, vw, vh)
        rhi = float(np.log10(np.percentile(r[r > 0], 99.8)))
        rlo = rhi - 2.5
        save_log(r, os.path.join(d, f"view_{view}_field.png"), rlo, rhi, "inferno")
        for tag, _, _ in tags:
            p = os.path.join(d, f"view_{view}_{tag}.f32")
            if not os.path.exists(p):
                continue
            a = load(p, vw, vh)
            save_log(a, os.path.join(d, f"view_{view}_{tag}.png"), rlo, rhi, "inferno")
            e = (a - r) / np.maximum(r, 1e-30)
            plt.imsave(os.path.join(d, f"diff_{view}_{tag}.png"), np.clip(e, -0.25, 0.25), vmin=-0.25, vmax=0.25, cmap="RdBu_r")
    print("wrote PNGs to", d)


if __name__ == "__main__":
    main()
