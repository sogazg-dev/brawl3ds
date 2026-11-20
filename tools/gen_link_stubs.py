"""Generate typed no-op definitions for functions the 3DS build leaves out (Aurora's renderer, netplay, Discord, ...).

Usage:
  python -I tools/gen_link_stubs.py --missing <file of undefined mangled symbols> --out <stubs.cpp>
      --header <h> [--header <h> ...] --compiler <clang++> -- <compile flags...>

The headless build links WiiCompiled's runtime without Aurora (WebGPU) and without the PC-only modules. Every symbol
the linker can't find is listed in --missing (mangled, one per line). This compiles a translation unit that includes
the given headers with clang's JSON AST dump, finds each missing symbol by its mangled name, and writes a definition
with the exact declared signature that does nothing and returns a zeroed value (or a static zeroed object for
reference returns). Exact signatures matter on ARM: a function returning a float or a struct uses different
registers than one returning an int, so a generic stub would corrupt the caller.

Definitions are written inside their namespaces, member functions with trailing return types, so names resolve the
way they do in the original declarations. Symbols not found in the headers are reported and left for the caller.
"""
import argparse
import json
import subprocess
import sys
import tempfile
from collections import defaultdict
from pathlib import Path

FUNCTION_KINDS = {"FunctionDecl", "CXXMethodDecl", "CXXConstructorDecl", "CXXDestructorDecl"}


def split_function_type(qual: str):
    """'R (A, B) const noexcept' -> ('R', 'const noexcept'). Finds the parameter list's parentheses from the end."""
    text = qual.strip()
    depth = 0
    close = -1
    for i in range(len(text) - 1, -1, -1):
        if text[i] == ")":
            if depth == 0 and close < 0:
                close = i
            depth += 1
        elif text[i] == "(":
            depth -= 1
            if depth == 0 and close >= 0:
                return text[:i].strip(), text[close + 1:].strip()
    raise ValueError("not a function type: " + qual)


def walk(node, scope, found, wanted):
    kind = node.get("kind")
    if kind in FUNCTION_KINDS and node.get("mangledName") in wanted and not node.get("isImplicit"):
        mangled = node["mangledName"]
        if mangled not in found:
            params = [(p.get("name", ""), p["type"]["qualType"]) for p in node.get("inner", [])
                      if p.get("kind") == "ParmVarDecl"]
            found[mangled] = {"kind": kind, "name": node["name"], "type": node["type"]["qualType"],
                              "params": params, "scope": list(scope),
                              "variadic": node.get("variadic", False)}
    inner_scope = scope
    if kind == "NamespaceDecl" and node.get("name"):
        inner_scope = scope + [("namespace", node["name"])]
    elif kind in ("CXXRecordDecl", "ClassTemplateDecl") and node.get("name") and node.get("completeDefinition", True):
        inner_scope = scope + [("class", node["name"])]
    elif kind == "LinkageSpecDecl":
        inner_scope = scope
    for child in node.get("inner", []) or []:
        if isinstance(child, dict):
            walk(child, inner_scope, found, wanted)


def zero_return(ret: str) -> str:
    if ret == "void":
        return ""
    if ret.endswith("&"):
        base = ret.rstrip("&").strip()
        if base.startswith("const "):
            base = base[len("const "):]
        return f" static std::type_identity_t<{base}> brawl3ds_zero{{}}; return brawl3ds_zero;"
    return " return {};"  # value-initializes any type: pointers, enums, structs


def spelled(t: str) -> str:
    """Any declared type, including arrays and function pointers, as a type usable before a parameter name."""
    return f"std::type_identity_t<{t}>"


def definition(info) -> str:
    namespaces = [n for k, n in info["scope"] if k == "namespace"]
    classes = [n for k, n in info["scope"] if k == "class"]
    ret, tail = split_function_type(info["type"])
    params = ", ".join(f"{spelled(t)} p{i}" for i, (_, t) in enumerate(info["params"]))
    if info["variadic"]:
        params = params + (", ..." if params else "...")
    unused = "".join(f" (void)p{i};" for i in range(len(info["params"])))
    qualifiers = " ".join(q for q in tail.split() if q in ("const", "volatile", "noexcept", "&", "&&"))
    owner = "::".join(classes)
    if info["kind"] == "CXXConstructorDecl":
        body = f"{owner}::{info['name']}({params}) {{{unused} }}"
    elif info["kind"] == "CXXDestructorDecl":
        body = f"{owner}::{info['name']}() {{}}"
    elif classes:
        body = f"auto {owner}::{info['name']}({params}) {qualifiers} -> {spelled(ret)} {{{unused}{zero_return(ret)} }}"
    else:
        body = f"{spelled(ret)} {info['name']}({params}) {qualifiers} {{{unused}{zero_return(ret)} }}"
    is_c = not namespaces and not classes
    if is_c:
        body = 'extern "C" ' + body
    for ns in reversed(namespaces):
        body = f"namespace {ns} {{ {body} }}"
    return body


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--missing", type=Path, required=True)
    ap.add_argument("--out", type=Path, required=True)
    ap.add_argument("--header", action="append", required=True)
    ap.add_argument("--compiler", required=True)
    ap.add_argument("--extra", type=Path, help="hand-written stubs appended verbatim")
    ap.add_argument("--allow-namespace", action="append", default=[],
                    help="stub C++ symbols in this top-level namespace (repeatable)")
    ap.add_argument("--allow-prefix", action="append", default=[],
                    help="stub global (C-linkage) functions whose name starts with this (repeatable)")
    ap.add_argument("--skip", action="append", default=[],
                    help="mangled symbol provided by hand-written code instead (repeatable)")
    ap.add_argument("flags", nargs=argparse.REMAINDER)
    args = ap.parse_args()
    flags = [f for f in args.flags if f != "--"]

    wanted = {line.strip() for line in args.missing.read_text().splitlines() if line.strip()} - set(args.skip)
    includes = "".join(f'#include "{h}"\n' for h in args.header)
    with tempfile.TemporaryDirectory() as tmp:
        tu = Path(tmp) / "stub_headers.cpp"
        tu.write_text(includes, encoding="utf-8")
        result = subprocess.run([args.compiler, *flags, "-fsyntax-only", "-Xclang", "-ast-dump=json", str(tu)],
                                capture_output=True)
    if result.returncode != 0:
        sys.exit("header translation unit failed to compile:\n" + result.stderr.decode(errors="replace")[:4000])
    ast = json.loads(result.stdout)

    found = {}
    walk(ast, [], found, wanted)
    # The headers also declare libc/libstdc++/imgui functions that the real libraries provide: stub only the
    # namespaces and global-name prefixes the caller allows.
    def allowed(info):
        if info["scope"]:
            return info["scope"][0][0] == "namespace" and info["scope"][0][1] in args.allow_namespace
        return any(info["name"].startswith(p) for p in args.allow_prefix)
    found = {k: v for k, v in found.items() if allowed(v)}
    lines = ["// Generated by brawl3ds tools/gen_link_stubs.py: no-op definitions for code the 3DS build leaves out.",
             "// Do not edit; regenerate from the link's missing-symbol list.", includes]
    by_header = defaultdict(list)
    for mangled in sorted(found):
        lines.append(definition(found[mangled]))
        by_header[found[mangled]["scope"][0][1] if found[mangled]["scope"] else "(global)"].append(mangled)
    if args.extra:
        lines.append(args.extra.read_text(encoding="utf-8"))
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text("\n".join(lines) + "\n", encoding="utf-8")

    missing = sorted(wanted - set(found))
    print(f"stubbed {len(found)} of {len(wanted)} symbols -> {args.out}")
    for scope, names in sorted(by_header.items()):
        print(f"  {scope}: {len(names)}")
    if missing:
        (args.out.with_suffix(".unresolved.txt")).write_text("\n".join(missing) + "\n")
        print(f"  not declared in the headers: {len(missing)} (listed in {args.out.with_suffix('.unresolved.txt').name})")


if __name__ == "__main__":
    main()
