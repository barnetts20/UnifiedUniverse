#!/usr/bin/env python3
"""Cross-check the engine's shader parameter structs against the shaders (review aid; no engine needed).

    python3 -I Tools/UniVerify/shader_params.py [--verbose]

Unreal binds a shader's parameters by name. A resource or uniform the HLSL references that the
C++ parameter struct lacks fails only at runtime in the editor ("unbound parameter"), and the
offline gate never compiles the engine side, so this script checks it mechanically. For every
IMPLEMENT_GLOBAL_SHADER in UniShaders.cpp it:
  1. collects the class's FParameters from UniShaders.h (SHADER_PARAMETER_STRUCT_INCLUDE expanded,
     `using FParameters = ...` followed) and the defines its ModifyCompilationEnvironment sets;
  2. preprocesses the .usf with those defines (gcc's preprocessor; engine includes stubbed, the
     plugin's mapped to Shaders/Private), follows the call graph from the entry point and collects
     the file-scope resources and uniforms the reachable functions reference (what the compiler
     keeps);
  3. reports each referenced one the struct lacks, and each whose type differs.
Exit status 1 if anything is missing or mismatched. Parameters the struct has and the shader does not
use are listed with --verbose (harmless: the compiler strips them and RDG leaves them unbound).
"""
import os
import re
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SH = os.path.join(ROOT, "Source/UnifiedUniverseRender/Private/UniShaders.h")
CPP = os.path.join(ROOT, "Source/UnifiedUniverseRender/Private/UniShaders.cpp")
SHADERS = os.path.join(ROOT, "Shaders/Private")

CPP_TYPES = {
    "float": "float", "uint32": "uint", "int32": "int", "FVector2f": "float2", "FVector3f": "float3",
    "FVector4f": "float4", "FUintVector2": "uint2", "FUintVector3": "uint3", "FUintVector4": "uint4",
    "FIntPoint": "int2", "FIntVector": "int3", "FIntVector4": "int4", "FMatrix44f": "float4x4", "FLinearColor": "float4",
}


def strip_comments(t):
    t = re.sub(r"/\*.*?\*/", "", t, flags=re.S)
    return re.sub(r"//[^\n]*", "", t)


def parse_structs(h):
    """name -> list of (kind, type, name) members; includes kept as ('include', struct, '')."""
    out = {}
    for m in re.finditer(r"BEGIN_SHADER_PARAMETER_STRUCT\((\w+)\s*,[^)]*\)(.*?)END_SHADER_PARAMETER_STRUCT\(\)", h, re.S):
        out.setdefault(m.group(1), [])
    return out


def members_of(body):
    mem = []
    for m in re.finditer(r"(SHADER_PARAMETER\w*)\s*\(([^;]*?)\)\s*(?=\n|SHADER_|RENDER_TARGET|END_)", body):
        macro, args = m.group(1), m.group(2)
        if macro == "SHADER_PARAMETER_STRUCT_INCLUDE":
            a = [x.strip() for x in args.split(",")]
            mem.append(("include", a[0], a[1]))
            continue
        # the last comma-separated token is the name; the rest is the type (templates can hold commas)
        i = args.rfind(",")
        mem.append((macro, args[:i].strip(), args[i + 1:].strip()))
    return mem


def classes(h):
    """class -> (members or a struct name, defines)."""
    out = {}
    structs = {}
    # top-level structs
    for m in re.finditer(r"^BEGIN_SHADER_PARAMETER_STRUCT\((\w+)\s*,[^)]*\)(.*?)^END_SHADER_PARAMETER_STRUCT\(\)", h, re.S | re.M):
        structs[m.group(1)] = members_of(m.group(2))
    # classes declared by a macro (#define M(Name) class Name ... using FParameters = S; ... ; M(X)):
    # the macro's text runs over its backslash-continued lines
    for d in re.finditer(r"#define\s+(\w+)\(Name\)((?:[^\n]*\\\n)+[^\n]*)", h):
        macro, body = d.group(1), d.group(2)
        if not re.search(r"class\s+Name\b", body):
            continue
        u = re.search(r"using\s+FParameters\s*=\s*(\w+)", body)
        for inv in re.finditer(r"^%s\((\w+)\)" % macro, h, re.M):
            out[inv.group(1)] = ([("include", u.group(1), "")] if u else [], {})
    for m in re.finditer(r"class\s+(\w+)\s*:\s*public\s+FGlobalShader\s*\{(.*?)\n\};", h, re.S):
        name, body = m.group(1), m.group(2)
        defines = dict((d.group(1), d.group(2)) for d in re.finditer(r'SetDefine\(TEXT\("(\w+)"\),\s*([^)]+)\)', body))
        inner = re.search(r"BEGIN_SHADER_PARAMETER_STRUCT\(FParameters\s*,[^)]*\)(.*?)END_SHADER_PARAMETER_STRUCT\(\)", body, re.S)
        if inner:
            mem = members_of(inner.group(1))
        else:
            u = re.search(r"using\s+FParameters\s*=\s*(\w+)\s*;", body)
            mem = [("include", u.group(1), "")] if u else []
        out[name] = (mem, defines)
    return out, structs


def flatten(mem, structs, seen=()):
    flat = []
    for kind, typ, name in mem:
        if kind == "include":
            if typ in seen:
                continue
            flat += flatten(structs.get(typ, []), structs, seen + (typ,))
        else:
            flat.append((kind, typ, name))
    return flat


def hlsl_type(kind, typ):
    if kind in ("SHADER_PARAMETER",):
        return CPP_TYPES.get(typ, typ)
    if kind == "SHADER_PARAMETER_ARRAY":
        t = typ.split(",")[0].strip()
        return CPP_TYPES.get(t, t) + "[]"
    if kind == "SHADER_PARAMETER_SAMPLER":
        return "SamplerState"
    return typ.replace(" ", "")  # RDG_TEXTURE(Texture2D<float4>) etc.: the HLSL type itself


def preprocess(usf, defines):
    stubs = tempfile.mkdtemp(prefix="uniparams_")
    # map /Engine/... to empty stubs and /Plugin/UnifiedUniverse/Private/... to the shaders
    src = open(os.path.join(SHADERS, usf)).read()

    def fix_includes(text, base):
        def repl(m):
            path = m.group(1)
            if path.startswith("/Engine/"):
                return ""
            if path.startswith("/Plugin/UnifiedUniverse/Private/"):
                path = path[len("/Plugin/UnifiedUniverse/Private/"):]
                full = os.path.join(SHADERS, path)
            else:
                full = os.path.join(base, path)
            return '#include "%s"' % full
        return re.sub(r'#include\s+"([^"]+)"', repl, text)

    # rewrite every shader file reachable (includes resolved to rewritten copies)
    done = {}

    def rewrite(path):
        if path in done:
            return done[path]
        text = open(path).read()
        out = os.path.join(stubs, os.path.relpath(path, SHADERS).replace("/", "__"))
        done[path] = out

        def repl(m):
            p = m.group(1)
            if p.startswith("/Engine/"):
                return ""
            if p.startswith("/Plugin/UnifiedUniverse/Private/"):
                full = os.path.join(SHADERS, p[len("/Plugin/UnifiedUniverse/Private/"):])
            else:
                full = os.path.normpath(os.path.join(os.path.dirname(path), p))
            return '#include "%s"' % rewrite(full)
        open(out, "w").write(re.sub(r'#include\s+"([^"]+)"', repl, text))
        return out

    top = rewrite(os.path.join(SHADERS, usf))
    args = ["cpp", "-P", "-undef", "-nostdinc", "-w"] + ["-D%s=%s" % (k, v) for k, v in defines.items()] + [top]
    r = subprocess.run(args, capture_output=True, text=True)
    if r.returncode != 0:
        raise RuntimeError(r.stderr[:2000])
    return r.stdout


DECL = re.compile(r"^\s*((?:RW)?(?:Texture\w+|StructuredBuffer|ByteAddressBuffer|Buffer)\s*(?:<[^>;]*>)?|SamplerState|(?:float|uint|int|bool)(?:[234](?:x[234])?)?)\s+(\w+)\s*(\[[^\]]*\])?\s*;", re.M)


def shader_globals(text):
    """File-scope declarations (brace depth 0) of resources and uniforms, and the text outside them."""
    depth = 0
    lines = text.split("\n")
    decls = {}
    keep = []
    for ln in lines:
        stripped = ln.strip()
        if depth == 0:
            m = DECL.match(ln)
            if m and not stripped.startswith("static") and not stripped.startswith("const") and not stripped.startswith("groupshared"):
                decls[m.group(2)] = (m.group(1).replace(" ", ""), bool(m.group(3)))
                keep.append("")
                depth += ln.count("{") - ln.count("}")
                continue
        keep.append(ln)
        depth += ln.count("{") - ln.count("}")
    return decls, "\n".join(keep)


IDENT = re.compile(r"[A-Za-z_]\w*")
KEYWORDS = {"if", "for", "while", "switch", "return", "struct", "else", "do"}


def functions(text):
    """Top-level function definitions: name -> body text (brace-matched)."""
    out = {}
    depth = 0
    start = 0        # where the current depth-0 header began
    body_start = None
    name = None
    i = 0
    n = len(text)
    while i < n:
        c = text[i]
        if c == "{":
            if depth == 0:
                header = text[start:i]
                if re.search(r"\bstruct\b", header):
                    name = None
                else:
                    m = re.search(r"([A-Za-z_]\w*)\s*\([^()]*(?:\([^()]*\)[^()]*)*\)\s*(?::\s*\w+\s*)?$", header.strip())
                    name = m.group(1) if m else None
                body_start = i
            depth += 1
        elif c == "}":
            depth -= 1
            if depth == 0:
                if name:
                    out.setdefault(name, "")
                    out[name] += text[body_start:i + 1]
                name = None
                start = i + 1
        elif c == ";" and depth == 0:
            start = i + 1
        i += 1
    return out


def reachable_text(text, entry):
    fns = functions(text)
    seen = set()
    todo = [entry]
    while todo:
        f = todo.pop()
        if f in seen or f not in fns:
            continue
        seen.add(f)
        for t in set(IDENT.findall(fns[f])):
            if t in fns and t not in seen and t not in KEYWORDS:
                todo.append(t)
    return "\n".join(fns[f] for f in seen), entry in fns


# Resources the call graph reaches only through a branch the compiler folds away (a literal
# argument), so the shader does not bind them: (shader, resource) -> why.
FOLDED = {
    ("FUniGalMapBakeCS", "UniWebSampler"): "Uni_GalaxyMapTexel calls Uni_GalaxySharedK with InTex = false (D-212)",
    ("FUniGalMapBakeCS", "UniGalWarpFine"): "Uni_GalaxyMapTexel calls Uni_GalaxySharedK with InTex = false (D-212)",
    ("FUniGalMapBakeCS", "UniGalWarpLarge"): "Uni_GalaxyMapTexel calls Uni_GalaxySharedK with InTex = false (D-212)",
    ("FUniGalMapBakeCS", "UniGalDetail"): "Uni_GalaxyMapTexel calls Uni_GalaxySharedK with InTex = false (D-212)",
}


def main():
    verbose = "--verbose" in sys.argv
    h = strip_comments(open(SH).read())
    cls, structs = classes(h)
    impl = re.findall(r'IMPLEMENT_GLOBAL_SHADER\((\w+),\s*"([^"]+)",\s*"(\w+)",\s*(\w+)\)', open(CPP).read())
    problems = 0
    for name, path, entry, stage in impl:
        if name not in cls:
            print(f"!! {name}: no class found in UniShaders.h")
            problems += 1
            continue
        mem, defines = cls[name]
        params = {n: hlsl_type(k, t) for k, t, n in flatten(mem, structs)}
        usf = os.path.basename(path)
        text = preprocess(usf, defines)
        decls, rest = shader_globals(text)
        reach, bEntry = reachable_text(rest, entry)
        used = {n: d for n, d in decls.items() if re.search(r"\b%s\b" % re.escape(n), reach)}
        if not bEntry:
            print(f"!! {name}: entry {entry} not found in {usf} with {defines}")
            problems += 1
        folded = [n for n in used if n not in params and (name, n) in FOLDED]
        missing = [n for n in used if n not in params and (name, n) not in FOLDED]
        mism = []
        for n, (t, arr) in used.items():
            if n in params:
                ct = params[n]
                if ct.endswith("[]"):
                    ok = arr and ct[:-2] == t
                else:
                    ok = ct == t
                if not ok:
                    mism.append(f"{n}: shader {t}{'[]' if arr else ''}, C++ {ct}")
        unused = [n for n in params if n not in used]
        tag = "OK" if not missing and not mism else "!!"
        print(f"{tag} {name:28s} {usf:20s} {entry:14s} {len(used):3d} used, {len(params):3d} in struct")
        for n in missing:
            print(f"     MISSING in C++: {n} ({used[n][0]})")
        if verbose:
            for n in folded:
                print(f"     folded away (not bound): {n}: {FOLDED[(name, n)]}")
        for x in mism:
            print(f"     TYPE: {x}")
        if verbose and unused:
            print(f"     unused by the shader: {', '.join(unused)}")
        problems += len(missing) + len(mism)
    print(f"{len(impl)} shaders, {problems} problems")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
