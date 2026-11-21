"""Pick the translated-code shards that get inline guest memory accesses.

Inline loads/stores (memory_access.h on ARM32 with BRAWL3DS_INLINE_GUEST_ACCESS) are the fast path, but compiling
all 600 shards that way grows the code from 38 to 55 MB, which leaves the regular heap too small to boot. Every other
shard calls one shared out-of-line fast path instead (same code size as before). This tool chooses the hot shards
from a real-hardware profile.txt (port/wiicompiled_arm32/profiler_3ds.cpp).

  python -I tools/hot_shards.py functions <profile.txt> <shard dir> <out hot_functions.txt>
      The functions with at least MIN_SELF_PERCENT of the samples in the profile's self ranking, and the callee at
      each return address with at least that share (a leaf's time lands at its caller's return address). Needs the
      shard sources to read call sites. The result is committed as port/headless/hot_functions.txt. (Adding every
      listed function and one more level of callees picked 87 shards, too much code to boot.)

  python -I tools/hot_shards.py shards <hot_functions.txt> <shard dir> <out hot_shards.txt>
      The shard files that define those functions, one file name per line (read by port/headless/CMakeLists.txt).
"""
import os
import re
import sys
from pathlib import Path

DEF_RE = re.compile(r'^extern "C" void func_([0-9A-F]{8})\(', re.M)
CALL_RE = re.compile(r'(?:CALL[A-Z_]*\(0x([0-9A-F]{8})u|InvokeDirectCpu<0x([0-9A-F]{8})u>)')


def read_shards(shard_dir: Path):
    sources, defs = {}, {}
    for path in sorted(shard_dir.glob("shard_*.cpp")):
        text = path.read_text(encoding="utf-8", errors="replace")
        sources[path.name] = text
        for m in DEF_RE.finditer(text):
            defs[m.group(1)] = path.name
    return sources, defs


def body(sources, defs, address):
    name = defs.get(address)
    if not name:
        return ""
    text = sources[name]
    start = text.find(f'extern "C" void func_{address}(')
    end = text.find('extern "C" void func_', start + 10)
    return text[start:end if end > 0 else len(text)]


def callee(match):
    return match.group(1) or match.group(2)


MIN_SELF_PERCENT = 0.1


def hot_functions(profile: Path, shard_dir: Path):
    text = profile.read_text(encoding="utf-8")
    self_ranking = text.split("== inclusive")[0]
    funcs = {a for p, a in re.findall(r"^\s*([\d.]+)\s+[\d.]+\s+([0-9A-F]{8})\s", self_ranking, re.M)
             if float(p) >= MIN_SELF_PERCENT}
    hot_returns = {a for p, a in re.findall(r"^\s*([\d.]+)\s+([0-9A-F]{8})\s+\S+\+0x", text, re.M)
                   if float(p) >= MIN_SELF_PERCENT}
    sources, _ = read_shards(shard_dir)
    for source in sources.values():
        for m in re.finditer(r"ctx->lr = 0x([0-9A-F]{8})u;", source):
            if m.group(1) in hot_returns:
                c = CALL_RE.search(source, m.end(), m.end() + 4000)
                if c:
                    funcs.add(callee(c))
    return sorted(funcs)


def main():
    if len(sys.argv) != 5 or sys.argv[1] not in ("functions", "shards"):
        sys.exit(__doc__)
    mode, source, shard_dir, out = sys.argv[1], Path(sys.argv[2]), Path(sys.argv[3]), Path(sys.argv[4])
    if mode == "functions":
        funcs = hot_functions(source, shard_dir)
        out.write_text("".join(f"{a}\n" for a in funcs), encoding="utf-8")
        print(f"{len(funcs)} hot functions -> {out}")
        return
    wanted = {line.strip() for line in source.read_text(encoding="utf-8").splitlines() if line.strip()}
    _, defs = read_shards(shard_dir)
    shards = sorted({defs[a] for a in wanted if a in defs})
    total = sum(os.path.getsize(p) for p in shard_dir.glob("shard_*.cpp"))
    size = sum(os.path.getsize(shard_dir / s) for s in shards)
    new_text = "".join(f"{s}\n" for s in shards)
    if not out.exists() or out.read_text(encoding="utf-8") != new_text:  # keep the timestamp when unchanged
        out.write_text(new_text, encoding="utf-8")
    print(f"{len(shards)} hot shards ({size / 1e6:.1f} of {total / 1e6:.1f} MB of source) -> {out}")


if __name__ == "__main__":
    main()
