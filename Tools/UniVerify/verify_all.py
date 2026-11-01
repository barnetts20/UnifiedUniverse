#!/usr/bin/env python3
"""verify_all: build the offline harness, run every check, write Verification/REPORT.md.

Run from anywhere:  python3 Plugins/UnifiedUniverse/Tools/UniVerify/verify_all.py [--quick]

Steps
  1. Build univerify with g++ and with clang++ (-Wall -Wextra -Werror). Both must build;
     the clang binary also runs the Tier 1 test group so compiler-specific behaviour shows.
  2. univerify test: every registered Tier 1/2/3 check (JSON lines).
  3. univerify fuzzdump 10^6 + oracle/coord_oracle.py (Python big-int oracle).
  4. Static checks: shader-core subset lint, GPU struct layout asserts present, UE-facing
     rules (no FVector absolute positions, hooks in one file).
  5. REPORT.md: header, pass/fail tables with numbers, images, regressions vs REPORT.prev.md,
     and the unverified list, which must match REVIEW_TONIGHT.md.

The report is generated only. Never edit it by hand.
"""

import datetime
import json
import os
import re
import shutil
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
PLUGIN = os.path.abspath(os.path.join(HERE, "..", ".."))
VER = os.path.join(PLUGIN, "Verification")
WORK = os.path.join(VER, "work")
IMAGES = os.path.join(VER, "images")
REPORT = os.path.join(VER, "REPORT.md")
PREV = os.path.join(VER, "REPORT.prev.md")
UNVERIFIED = os.path.join(HERE, "unverified.json")
REVIEW = os.path.join(PLUGIN, "Docs", "REVIEW_TONIGHT.md")

QUICK = "--quick" in sys.argv


def run(cmd, cwd=PLUGIN, timeout=3600, env=None):
    t = time.time()
    p = subprocess.run(cmd, cwd=cwd, capture_output=True, text=True, timeout=timeout, env=env)
    return p.returncode, p.stdout + p.stderr, time.time() - t


def add(results, tier, cid, title, ok, measured, metrics=None):
    results.append({"tier": tier, "id": cid, "title": title, "pass": bool(ok), "measured": measured, "metrics": metrics or {}})


def build(results, compiler, tag):
    bdir = os.path.join(HERE, "build", "cmake-" + tag)
    code, out, secs = run(["cmake", "-S", HERE, "-B", bdir, "-DCMAKE_CXX_COMPILER=" + compiler, "-G", "Unix Makefiles"])
    if code == 0:
        code, out, secs2 = run(["cmake", "--build", bdir, "-j2"])
        secs += secs2
    exe = os.path.join(HERE, "build", "univerify")
    dst = os.path.join(HERE, "build", "univerify-" + tag)
    if code == 0 and os.path.exists(exe):
        shutil.copy2(exe, dst)
    errs = [l for l in out.splitlines() if "error" in l.lower() or "warning" in l.lower()][:8]
    add(results, 1, "T1.build." + tag, f"Harness and UniCore build with {compiler}, -Wall -Wextra -Werror, C++17",
        code == 0, f"built in {secs:.0f} s" if code == 0 else "BUILD FAILED: " + " | ".join(errs), {"seconds": secs})
    return dst if code == 0 else None


# ------------------------------------------------------------------------------------------
# Static checks
# ------------------------------------------------------------------------------------------

SWIZZLE = re.compile(r"\.(?:[xyzw]{2,4}|[rgba]{2,4})\b")


def strip_comments(src):
    src = re.sub(r"/\*.*?\*/", "", src, flags=re.S)
    return re.sub(r"//[^\n]*", "", src)


def lint_cores(results):
    core = os.path.join(PLUGIN, "Shaders", "Private", "Core")
    problems = []
    files = sorted(f for f in os.listdir(core) if f.endswith(".ush"))
    for f in files:
        src = strip_comments(open(os.path.join(core, f)).read())
        for m in SWIZZLE.finditer(src):
            problems.append(f"{f}: swizzle '{m.group(0)}'")
        # every function definition at file scope must carry UNI_FN
        for m in re.finditer(r"^(?!\s*(?:#|UNI_FN|static|struct|return|if|else|for|while|//))\s*(?:float[234]?|uint[234]?|int3?|bool|void)\s+(\w+)\s*\(", src, flags=re.M):
            problems.append(f"{f}: function '{m.group(1)}' without UNI_FN")
        for m in re.finditer(r"(?<![\w.])(\d+\.\d*(?:[eE][+-]?\d+)?|\d+[eE][+-]?\d+)([fF]?)", src):
            if not m.group(2):
                problems.append(f"{f}: unsuffixed float literal {m.group(1)}")
        for kw in ("Texture3D", "SamplerState", "RWStructuredBuffer", "groupshared"):
            if re.search(r"\b" + kw + r"\b", src):
                problems.append(f"{f}: resource/keyword '{kw}' not in the shared subset")
    add(results, 4, "T4.static.core_subset", "Shader cores stay in the shared C++/HLSL subset (no swizzles, UNI_FN, f-suffixed literals, no textures)",
        not problems, f"{len(files)} cores, {len(problems)} problems" + ("" if not problems else ": " + "; ".join(problems[:6])),
        {"files": len(files), "problems": len(problems)})


def lint_gpu_structs(results):
    """Every struct declared in a .usf/.ush that has a C++ mirror must have a static_assert."""
    src_root = os.path.join(PLUGIN, "Source")
    asserts = {}
    for d, _, fs in os.walk(src_root):
        for f in fs:
            if f.endswith((".h", ".cpp")):
                for m in re.finditer(r"static_assert\s*\(\s*sizeof\s*\(\s*(\w+)\s*\)\s*(?:==|%)\s*(\d+)", open(os.path.join(d, f)).read()):
                    asserts.setdefault(m.group(1), []).append(int(m.group(2)))
    gpu = []
    for d, _, fs in os.walk(src_root):
        for f in fs:
            if f.endswith(".h"):
                txt = open(os.path.join(d, f)).read()
                for m in re.finditer(r"//\s*GPU-MIRROR\s*\n\s*struct\s+(\w+)", txt):
                    gpu.append(m.group(1))
    missing = [g for g in gpu if g not in asserts]
    bad16 = [g for g in gpu if g in asserts and not any(v % 16 == 0 and v > 16 or v == 16 for v in asserts[g])]
    add(results, 4, "T4.static.gpu_structs", "GPU-mirrored structs (marked // GPU-MIRROR) have a static_assert, size a multiple of 16 (lesson #20)",
        not missing and not bad16,
        f"{len(gpu)} GPU structs; missing assert: {missing or 'none'}; not 16-multiple: {bad16 or 'none'}",
        {"structs": len(gpu), "missing": len(missing)})


def lint_ue_rules(results):
    src_root = os.path.join(PLUGIN, "Source")
    problems = []
    hook_files = set()
    for d, _, fs in os.walk(src_root):
        for f in fs:
            if not f.endswith((".h", ".cpp")):
                continue
            path = os.path.join(d, f)
            rel = os.path.relpath(path, PLUGIN)
            txt = strip_comments(open(path).read())
            if rel.startswith(os.path.join("Source", "UniCore")):
                for inc in re.findall(r'#include\s+[<"]([^>"]+)[>"]', txt):
                    if inc.startswith(("CoreMinimal", "Engine/", "UObject/", "Modules/", "RHI", "RenderGraph")) or inc.endswith(".generated.h"):
                        problems.append(f"{rel}: UE include {inc} in UniCore")
            if re.search(r"SubscribeToPostProcessingPass|PostRenderBasePass|PrePostProcessPass_RenderThread", txt) and not f.startswith("UniRenderHooks"):
                hook_files.add(rel)
            # absolute positions as FVector: flag UPROPERTY FVector named *Position/*Location outside the bubble types
            for m in re.finditer(r"FVector(?:3d|3f)?\s+(\w*(?:Universe|Absolute|Abs)\w*)\s*[;=]", txt):
                problems.append(f"{rel}: suspicious absolute FVector '{m.group(1)}'")
    if hook_files:
        problems.append("render hooks outside UniRenderHooks.cpp: " + ", ".join(sorted(hook_files)))
    add(results, 4, "T4.static.ue_rules", "UE-facing rules: no UE includes in UniCore, render hooks only in UniRenderHooks.cpp, no absolute FVector",
        not problems, "no problems" if not problems else "; ".join(problems[:6]), {"problems": len(problems)})


# ------------------------------------------------------------------------------------------
# Report
# ------------------------------------------------------------------------------------------

def git(*args):
    try:
        return subprocess.run(["git", *args], cwd=PLUGIN, capture_output=True, text=True).stdout.strip()
    except OSError:
        return ""


def prev_status():
    out = {}
    if os.path.exists(PREV):
        for line in open(PREV):
            m = re.match(r"\|\s*`([^`]+)`\s*\|\s*(PASS|FAIL)", line)
            if m:
                out[m.group(1)] = m.group(2)
    return out


TIER_NAMES = {1: "Tier 1 — core correctness", 2: "Tier 2 — generation behaviour", 3: "Tier 3 — rendering behaviour",
              4: "Tier 4 — Unreal integration (static only in this environment)"}


def write_report(results, unverified, total_secs):
    if os.path.exists(REPORT):
        shutil.copy2(REPORT, PREV)
    prev = prev_status()
    head = git("rev-parse", "--short", "HEAD") or "(no git)"
    dirty = bool(git("status", "--porcelain", "--untracked-files=no"))
    now = datetime.datetime.now(datetime.timezone(datetime.timedelta(hours=-5))).strftime("%Y-%m-%d %H:%M EST")
    tiers_green = [t for t in (1, 2, 3) if any(r["tier"] == t for r in results) and all(r["pass"] for r in results if r["tier"] == t)]
    reached = max(tiers_green) if tiers_green and tiers_green == list(range(1, max(tiers_green) + 1)) else 0
    failed = [r for r in results if not r["pass"]]

    L = []
    L.append("# Unified Universe — verification report")
    L.append("")
    L.append("*Generated by `Tools/UniVerify/verify_all.py`. Do not edit by hand.*")
    L.append("")
    L.append("| | |")
    L.append("| --- | --- |")
    L.append(f"| Commit | `{head}`{' + uncommitted changes' if dirty else ''} (branch `{git('rev-parse', '--abbrev-ref', 'HEAD')}`) |")
    L.append(f"| Run | {now}, {total_secs:.0f} s{' — QUICK MODE, not a gate run' if QUICK else ''} |")
    L.append(f"| Environment tier reached | Tier {reached} offline (Tier 4 static only: no Unreal, no DXC here — see ENVIRONMENT.md) |")
    L.append(f"| Checks | {len(results)} run, {len(results) - len(failed)} pass, **{len(failed)} fail** |")
    L.append("")
    for t in (1, 2, 3, 4):
        rows = [r for r in results if r["tier"] == t]
        if not rows:
            continue
        L.append(f"## {TIER_NAMES[t]}")
        L.append("")
        L.append("| Check | Result | Criterion | Measured |")
        L.append("| --- | --- | --- | --- |")
        for r in rows:
            L.append(f"| `{r['id']}` | {'PASS' if r['pass'] else 'FAIL'} | {r['title']} | {r['measured']} |")
        L.append("")

    imgs = sorted(f for f in os.listdir(IMAGES) if f.endswith(".png")) if os.path.isdir(IMAGES) else []
    obs_path = os.path.join(VER, "OBSERVATIONS.md")
    L.append("## Images")
    L.append("")
    if imgs:
        for f in imgs:
            L.append(f"- [{f}](images/{f})")
    else:
        L.append("None generated in this run.")
    L.append("")
    if os.path.exists(obs_path):
        L.append("## Visual inspection")
        L.append("")
        L.append(f"Written by the session after looking at the images: [OBSERVATIONS.md](OBSERVATIONS.md).")
        L.append("")

    L.append("## Regressions since the previous run")
    L.append("")
    reg = [r["id"] for r in results if not r["pass"] and prev.get(r["id"]) == "PASS"]
    fixed = [r["id"] for r in results if r["pass"] and prev.get(r["id"]) == "FAIL"]
    new = [r["id"] for r in results if r["id"] not in prev]
    if not prev:
        L.append("No previous report.")
    else:
        L.append(f"- Regressed (PASS → FAIL): {', '.join('`'+x+'`' for x in reg) if reg else 'none'}")
        L.append(f"- Fixed (FAIL → PASS): {', '.join('`'+x+'`' for x in fixed) if fixed else 'none'}")
        L.append(f"- New checks: {', '.join('`'+x+'`' for x in new) if new else 'none'}")
    L.append("")

    L.append("## Not verified here")
    L.append("")
    L.append("Each item below must also appear in `Docs/REVIEW_TONIGHT.md` §3 with its ID. The check `T0.report.unverified_listed` enforces that.")
    L.append("")
    for u in unverified:
        L.append(f"- **{u['id']}** — {u['what']}")
    L.append("")
    with open(REPORT, "w") as fh:
        fh.write("\n".join(L))


def main():
    t0 = time.time()
    os.makedirs(WORK, exist_ok=True)
    os.makedirs(IMAGES, exist_ok=True)
    results = []

    gcc = build(results, "g++", "gcc")
    for cxx in ("g++", "clang++"):
        if not shutil.which(cxx):
            continue
        code, out, secs = run([cxx, "-std=c++20", "-fsyntax-only", "-Wall", "-Wextra", "-Wshadow", "-Werror",
                               "-I" + os.path.join(PLUGIN, "Source", "UniCore"), "-I" + os.path.join(PLUGIN, "Shaders", "Private", "Core"),
                               os.path.join(HERE, "strict", "strict_headers.cpp")])
        errs = [l for l in out.splitlines() if "error" in l or "warning" in l][:4]
        add(results, 1, "T1.build.strict." + cxx.replace("+", "p"), f"Every UniCore header as C++20 with -Wall -Wextra -Wshadow -Werror ({cxx}), as the UE modules compile them",
            code == 0, "clean" if code == 0 else " | ".join(errs), {"seconds": secs})
    clang = build(results, "clang++", "clang") if shutil.which("clang++") else None

    jsonl = os.path.join(WORK, "results.jsonl")
    if os.path.exists(jsonl):
        os.remove(jsonl)
    if gcc:
        args = [gcc, "test", "--json", jsonl] + (["--quick"] if QUICK else [])
        code, out, secs = run(args, timeout=7200)
        open(os.path.join(WORK, "test_gcc.log"), "w").write(out)
        if os.path.exists(jsonl):
            results.extend(json.loads(l) for l in open(jsonl) if l.strip())
        if code not in (0, 1):
            add(results, 1, "T1.harness.run", "univerify test ran to completion", False, f"exit code {code}; see Verification/work/test_gcc.log")

    if clang:
        cj = os.path.join(WORK, "results_clang.jsonl")
        if os.path.exists(cj):
            os.remove(cj)
        logs = ""
        for grp in ("coord", "seed", "window", "level", "photometry"):
            code, out, secs = run([clang, "test", "--json", cj, "--quick", "--group", grp], timeout=3600)
            logs += out
        open(os.path.join(WORK, "test_clang.log"), "w").write(logs)
        rows = [json.loads(l) for l in open(cj) if l.strip()] if os.path.exists(cj) else []
        # The quick fuzz check fails by design (case count); judge the clang run on mismatches.
        bad = [r["id"] for r in rows if not r["pass"] and not (r["id"] == "T1.coord.fuzz" and r["metrics"].get("mismatches", 1) == 0)]
        add(results, 1, "T1.clang.coord_seed", "clang++ build passes every Tier 1 group (coord, seed, window, level, photometry; quick fuzz 10^5)",
            rows and not bad, f"{len(rows)} checks, failing: {bad or 'none'}", {"checks": len(rows)})

    if gcc:
        n = 100000 if QUICK else 1000000
        dump = os.path.join(WORK, "fuzz.bin")
        code, out, _ = run([gcc, "fuzzdump", str(n), dump])
        oj = os.path.join(WORK, "oracle.jsonl")
        if os.path.exists(oj):
            os.remove(oj)
        code, out, secs = run([sys.executable, "-I", os.path.join(HERE, "oracle", "coord_oracle.py"), dump, "--json", oj], timeout=3600)
        open(os.path.join(WORK, "oracle.log"), "w").write(out)
        if os.path.exists(oj):
            results.extend(json.loads(l) for l in open(oj) if l.strip())
        else:
            add(results, 1, "T1.coord.oracle", "Python oracle ran", False, out[-300:])
        for extra in ("seed_oracle.py",):
            p = os.path.join(HERE, "oracle", extra)
            if os.path.exists(p):
                ej = os.path.join(WORK, extra + ".jsonl")
                if os.path.exists(ej):
                    os.remove(ej)
                code, out, _ = run([sys.executable, "-I", p, gcc, "--json", ej], timeout=3600)
                open(os.path.join(WORK, extra + ".log"), "w").write(out)
                if os.path.exists(ej):
                    results.extend(json.loads(l) for l in open(ej) if l.strip())
                else:
                    add(results, 1, "T1." + extra, extra + " ran", False, out[-300:])
        try:
            os.remove(dump)
        except OSError:
            pass

    render_dir = os.path.join(WORK, "render")
    if os.path.isdir(render_dir):
        for f in os.listdir(IMAGES):
            if f.endswith(".png"):
                os.remove(os.path.join(IMAGES, f))
        code, out, _ = run([sys.executable, "-I", os.path.join(HERE, "scripts", "make_images.py"), render_dir, IMAGES])
        open(os.path.join(WORK, "images.log"), "w").write(out)
        n = len([f for f in os.listdir(IMAGES) if f.endswith(".png")])
        add(results, 3, "T3.images", "Reference frames and flythrough contact sheet converted to PNG with HUD (D-018)", code == 0 and n >= 4,
            f"{n} PNGs in Verification/images" if code == 0 else out[-300:], {"pngs": n})

    lint_cores(results)
    lint_gpu_structs(results)
    lint_ue_rules(results)

    unverified = json.load(open(UNVERIFIED)) if os.path.exists(UNVERIFIED) else []
    review = open(REVIEW).read() if os.path.exists(REVIEW) else ""
    missing = [u["id"] for u in unverified if u["id"] not in review]
    add(results, 4, "T0.report.unverified_listed", "Every unverified item is listed in REVIEW_TONIGHT.md",
        not missing, f"{len(unverified)} unverified items; missing from REVIEW_TONIGHT: {missing or 'none'}", {"items": len(unverified)})

    write_report(results, unverified, time.time() - t0)
    failed = [r["id"] for r in results if not r["pass"]]
    print(f"{len(results)} checks, {len(failed)} failed: {failed}")
    print("report:", REPORT)
    return 0 if not failed else 1


if __name__ == "__main__":
    sys.exit(main())
