#!/usr/bin/env python3
"""Numbers and pictures for `univerify galwarp` (D-220: the disc warp's aliasing and look).

    python3 -I Tools/UniVerify/galwarp_report.py DIR

Reads DIR/metrics.json and the float images, writes DIR/look.json and PNGs:
  look.json            per variant: the face-on structure's power by octave band and its contrast,
                       each against today's (1.0 = the same), and its per-pixel change from today;
  panel_xsec.png       labelled vertical cross-sections (the shared log scale of today's);
  panel_face.png       labelled face-on crops of the inner disc;
  panel_view.png       the top-down flight's last frame, marched and fine, per variant.
The face-on structure is the light over its azimuthal mean at each radius (annulus 0.1-0.7 of the
radius), minus 1: what the warp and the detail add to the smooth disc and arms. Its band powers
say whether a variant keeps today's mix of scales, which is what "perceptually consistent" asks.
"""
import json
import os
import sys

import numpy as np
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt


def load(p, w, h):
    return np.fromfile(p, dtype=np.float32).reshape(h, w)


def structure(face, ext):
    n = face.shape[0]
    c = (np.arange(n) + 0.5) / n * 2 * ext - ext
    x, y = np.meshgrid(c, -c)
    r = np.hypot(x, y)
    bins = np.linspace(0.0, ext * 1.42, 200)
    idx = np.digitize(r, bins)
    mean = np.zeros_like(face)
    for k in np.unique(idx):
        m = idx == k
        mean[m] = face[m].mean()
    s = np.where(mean > 0, face / np.maximum(mean, 1e-30) - 1.0, 0.0)
    ann = (r > 0.1) & (r < 0.7)
    s = np.where(ann, s, 0.0)
    # taper the annulus edges so they do not ring in the spectrum
    w = np.clip((r - 0.1) / 0.05, 0, 1) * np.clip((0.7 - r) / 0.05, 0, 1)
    return s * w, ann


def band_powers(s, ext):
    n = s.shape[0]
    f = np.fft.fftshift(np.abs(np.fft.fft2(s)) ** 2)
    k = np.fft.fftshift(np.fft.fftfreq(n, d=2 * ext / n))  # cycles per field unit
    kx, ky = np.meshgrid(k, k)
    kr = np.hypot(kx, ky)
    edges = [2, 4, 8, 16, 32, 64, 128]
    return [float(f[(kr >= a) & (kr < b)].sum()) for a, b in zip(edges[:-1], edges[1:])], edges


def main():
    d = sys.argv[1]
    m = json.load(open(os.path.join(d, "metrics.json")))
    vw, vh = m["view"]
    fn, fext = m["face"][0], m["face"][1]
    xw, xh = m["xsec"][0], m["xsec"][1]
    vars_ = m["variants"]
    base = None
    look = []
    for v in vars_:
        face = load(os.path.join(d, f"face_{v['tag']}.f32"), fn, fn)
        s, ann = structure(face, fext)
        bp, edges = band_powers(s, fext)
        contrast = float(np.sqrt((s[ann] ** 2).mean()))
        if base is None:
            base = (bp, contrast, face)
        change = float(np.abs(face - base[2])[ann].sum() / max(base[2][ann].sum(), 1e-30))
        look.append({"name": v["name"], "bands": [f"{a}-{b}" for a, b in zip(edges[:-1], edges[1:])],
                     "band_power_vs_today": [p / max(q, 1e-30) for p, q in zip(bp, base[0])],
                     "contrast": contrast, "contrast_vs_today": contrast / max(base[1], 1e-30), "face_change": change})
    json.dump(look, open(os.path.join(d, "look.json"), "w"), indent=2)
    for l in look:
        print(f"{l['name']:28s} contrast x{l['contrast_vs_today']:.2f}  face change {100*l['face_change']:5.1f}%  bands " +
              " ".join(f"{b}:{p:.2f}" for b, p in zip(l["bands"], l["band_power_vs_today"])))

    ref = load(os.path.join(d, f"xsec_{vars_[0]['tag']}.f32"), xw, xh)
    hi = float(np.log10(np.percentile(ref[ref > 0], 99.9)))
    lo = hi - 3.0
    fig, axes = plt.subplots(len(vars_), 1, figsize=(9.0, 1.5 * len(vars_)), dpi=110)
    for ax, v in zip(axes, vars_):
        x = load(os.path.join(d, f"xsec_{v['tag']}.f32"), xw, xh)
        ax.imshow(np.clip(np.log10(np.maximum(x, 10 ** lo)), lo, hi), cmap="magma", vmin=lo, vmax=hi, aspect="auto", interpolation="nearest")
        ax.set_title(v["name"], fontsize=9, loc="left", pad=3)
        ax.set_xticks([]); ax.set_yticks([])
    fig.tight_layout(h_pad=0.6)
    fig.savefig(os.path.join(d, "panel_xsec.png"))
    plt.close(fig)

    # face-on: a crop of the inner disc (x, y in [-0.45, 0.45]), log light, today's scale
    n0 = int(fn * (0.75 - 0.45) / 1.5)
    n1 = fn - n0
    f0 = base[2][n0:n1, n0:n1]
    fhi = float(np.log10(np.percentile(f0[f0 > 0], 99.5)))
    flo = fhi - 2.2
    cols = 4
    rows = (len(vars_) + cols - 1) // cols
    fig, axes = plt.subplots(rows, cols, figsize=(3.0 * cols, 3.15 * rows), dpi=110)
    for i, ax in enumerate(np.array(axes).reshape(-1)):
        ax.set_xticks([]); ax.set_yticks([])
        if i >= len(vars_):
            ax.axis("off")
            continue
        f = load(os.path.join(d, f"face_{vars_[i]['tag']}.f32"), fn, fn)[n0:n1, n0:n1]
        ax.imshow(np.clip(np.log10(np.maximum(f, 10 ** flo)), flo, fhi), cmap="magma", vmin=flo, vmax=fhi, interpolation="nearest")
        ax.set_title(vars_[i]["name"], fontsize=8, pad=3)
    fig.tight_layout()
    fig.savefig(os.path.join(d, "panel_face.png"))
    plt.close(fig)

    # top-down flight's last frame: marched (left) and fine reference (right)
    if not os.path.exists(os.path.join(d, f"view_{vars_[0]['tag']}_0_fine.f32")):
        print("wrote", d, "(no flights)")
        return
    r0 = load(os.path.join(d, f"view_{vars_[0]['tag']}_0_fine.f32"), vw, vh)
    vhi = float(np.log10(np.percentile(r0[r0 > 0], 99.8)))
    vlo = vhi - 2.0
    fig, axes = plt.subplots(len(vars_), 2, figsize=(7.0, 2.25 * len(vars_)), dpi=110)
    for row, v in zip(axes, vars_):
        for ax, kind in zip(row, ("march", "fine")):
            a = load(os.path.join(d, f"view_{v['tag']}_0_{kind}.f32"), vw, vh)
            ax.imshow(np.clip(np.log10(np.maximum(a, 10 ** vlo)), vlo, vhi), cmap="inferno", vmin=vlo, vmax=vhi, interpolation="nearest")
            ax.set_title(f"{v['name']}: {'march' if kind == 'march' else 'fine reference'}", fontsize=8, pad=3)
            ax.set_xticks([]); ax.set_yticks([])
    fig.tight_layout()
    fig.savefig(os.path.join(d, "panel_view.png"))
    plt.close(fig)
    print("wrote", d)


if __name__ == "__main__":
    main()
