"""Code packs: each fighter's and stage's translated code as a file the 3DS build loads when the game needs it.

The whole game's translated code is ~95 MiB and the console has ~44 MiB for code and data, so the executable links
the functions a few recorded sessions ran and interprets the rest. A fighter (ft_*) or stage (st_*) module is
0.2-1.6 MiB of translated code and only a handful are in use at a time: this tool compiles the functions of those
modules that the executable left out and links them, per module, into `<name>.b3p` for
sdmc:/3ds/brawl3ds/packs/. The console loads a pack the first time a function of that module is called
(port/wiicompiled_arm32/code_packs_3ds.cpp), into memory it then makes executable.

A pack is linked against one executable: every reference to the runtime, to translated code in the executable
and to its thread-local variables is resolved here to that ELF's addresses. Only the pack's own position is left
for the console (two lists of words to adjust). packs/index.bin carries a few of the executable's addresses; the
console refuses packs whose index does not match the executable it is.

Usage:
  python -I tools/build_packs.py --full <base_common.orig> --selection <shards dir> --layout <module_layout.json>
         --elf <brawl3ds.elf> --build-ninja <build dir>/build.ninja --work <dir for sources and objects>
         --out <.../sd/3ds/brawl3ds/packs> [--only ft_mario st_final ...] [--jobs N]
Sources and objects are reused when unchanged; packs are always relinked (the executable's addresses move).
"""
import argparse
import bisect
import concurrent.futures
import json
import re
import struct
import subprocess
import sys
import zlib
from collections import defaultdict
from pathlib import Path

MARKER = re.compile(r'^#line 1 "func_([0-9A-F]{8})\.cpp"$', re.M)
CLOSING = "#undef MKW_STATIC_TRANSLATED_CALL"
READELF = r"C:\devkitPro\devkitARM\bin\arm-none-eabi-readelf.exe"
PROBES = ["Brawl3ds_NotLinked", "_Z17InvokeIndirectCpujP10CpuContext", "memcpy", "g_mkwGuestCodePatched"]

SHT_PROGBITS, SHT_SYMTAB, SHT_NOBITS, SHT_REL, SHT_EXIDX = 1, 2, 8, 9, 0x70000001
SHF_WRITE, SHF_ALLOC, SHF_EXEC, SHF_TLS = 1, 2, 4, 0x400
R_NONE, R_ABS32, R_REL32, R_CALL, R_JUMP24, R_TARGET1, R_V4BX, R_PREL31, R_TLS_LE32 = 0, 2, 3, 28, 29, 38, 40, 42, 108


def split(text):
    body = text.rstrip()
    assert body.endswith(CLOSING)
    body = body[: -len(CLOSING)]
    marks = list(MARKER.finditer(body))
    head = body[: marks[0].start()] if marks else body
    parts = []
    for i, mark in enumerate(marks):
        end = marks[i + 1].start() if i + 1 < len(marks) else len(body)
        parts.append((mark.group(1), body[mark.start(): end]))
    return head, parts


class Obj:
    def __init__(self, path):
        self.path = path
        d = self.data = path.read_bytes()
        shoff, = struct.unpack_from("<I", d, 0x20)
        shentsize, shnum, shstrndx = struct.unpack_from("<HHH", d, 0x2E)
        self.sec = [struct.unpack_from("<10I", d, shoff + i * shentsize) for i in range(shnum)]
        # (name, type, flags, addr, offset, size, link, info, addralign, entsize)
        self.sym = []
        self.rels = defaultdict(list)   # section index -> [(offset, type, symbol index)]
        self.exidx_of = {}              # text section index -> its .ARM.exidx section index
        strtab = None
        for i, s in enumerate(self.sec):
            if s[1] == SHT_SYMTAB:
                strtab = self.sec[s[6]]
                for k in range(s[5] // 16):
                    name, value, size, info, other, shndx = struct.unpack_from("<IIIBBH", d, s[4] + k * 16)
                    self.sym.append([name, value, info >> 4, info & 15, shndx])
            elif s[1] == SHT_REL:
                target = s[7]
                for k in range(s[5] // 8):
                    off, info = struct.unpack_from("<II", d, s[4] + k * 8)
                    self.rels[target].append((off, info & 0xFF, info >> 8))
            elif s[1] == SHT_EXIDX:
                self.exidx_of[s[6]] = i
        base = strtab[4]
        for entry in self.sym:
            end = d.index(b"\0", base + entry[0])
            entry[0] = d[base + entry[0]: end].decode()
        self.globals = {}  # name -> symbol index, for defined global and weak symbols
        for k, (name, value, bind, typ, shndx) in enumerate(self.sym):
            if bind in (1, 2) and shndx != 0 and shndx < 0xFF00:
                self.globals.setdefault(name, k)

    def bytes_of(self, index):
        s = self.sec[index]
        return bytes(s[5]) if s[1] == SHT_NOBITS else self.data[s[4]: s[4] + s[5]]


def read_main(elf):
    """Addresses of the executable's global symbols, and its thread-local variables' offsets from the thread pointer."""
    out = subprocess.run([READELF, "-sW", str(elf)], capture_output=True, text=True, errors="replace").stdout
    syms, tls = {}, {}
    for line in out.split("\n"):
        f = line.split()
        if len(f) < 8 or f[4] not in ("GLOBAL", "WEAK") or f[6] == "UND":
            continue
        try:
            value = int(f[1], 16)
        except ValueError:
            continue
        if f[3] == "TLS":
            tls[f[7]] = value + 8  # libctru: the thread pointer is 8 bytes before the thread's TLS block
        elif f[7] not in syms:
            syms[f[7]] = value
    return syms, tls


def compile_flags(build_ninja):
    """The cold shards' compiler and flags, from the build that made the executable."""
    text = Path(build_ninja).read_text(encoding="utf-8", errors="replace")
    rules = (Path(build_ninja).parent / "CMakeFiles" / "rules.ninja").read_text(encoding="utf-8", errors="replace")
    compiler = re.search(r"rule CXX_COMPILER__b3_shards\S*\n(?:.*\n)*?\s*command = \$\{LAUNCHER\}\$\{CODE_CHECK\}(\S+)", rules).group(1)
    best = None
    for m in re.finditer(r"build CMakeFiles/b3_shards\.dir/\S+: CXX_COMPILER__b3_shards\S+ .*\n((?:  .*\n)+)", text):
        block = dict(re.findall(r"  (\w+) = (.*)", m.group(1)))
        if "BRAWL3DS_INLINE_GUEST_ACCESS" not in block.get("DEFINES", ""):
            best = block
            break
    assert best, "no cold shard in build.ninja"
    return compiler, (best["DEFINES"] + " " + best["INCLUDES"] + " " + best["FLAGS"]).split()


def link_pack(name, exports, objs, main_syms, main_tls, report):
    """exports: {symbol name: guest address}. Returns the pack file's bytes."""
    defs = {}
    for obj in objs:
        for sym_name, k in obj.globals.items():
            defs.setdefault(sym_name, (obj, k))
    missing = set()
    internal_data = set()

    def resolve(obj, k):
        sym_name, value, bind, typ, shndx = obj.sym[k]
        if bind == 0:  # local
            return ("abs", value) if shndx == 0xFFF1 else ("int", obj, shndx, value)
        if sym_name in exports and sym_name in defs:
            o, kk = defs[sym_name]
            return ("int", o, o.sym[kk][4], o.sym[kk][1])
        if sym_name in main_tls:
            return ("tls", main_tls[sym_name])
        if sym_name in main_syms:
            return ("ext", main_syms[sym_name])
        if sym_name in defs:
            o, kk = defs[sym_name]
            if o.sym[kk][3] == 6:
                missing.add(sym_name + " (thread-local, not in the executable)")
                return ("abs", 0)
            if o.sym[kk][3] == 1:
                internal_data.add(sym_name)
            return ("int", o, o.sym[kk][4], o.sym[kk][1])
        if bind == 2:  # weak and nowhere: 0
            return ("abs", 0)
        missing.add(sym_name)
        return ("abs", 0)

    # Sections reachable from the exported functions.
    kept, order, work = set(), [], []

    def keep(obj, index):
        key = (id(obj), index)
        if key in kept or index == 0 or index >= 0xFF00:
            return
        kept.add(key)
        order.append((obj, index))
        work.append((obj, index))

    for sym_name in sorted(exports, key=exports.get):
        if sym_name in defs:
            o, kk = defs[sym_name]
            keep(o, o.sym[kk][4])
    while work:
        obj, index = work.pop()
        if index in obj.exidx_of:
            keep(obj, obj.exidx_of[index])
        for off, typ, k in obj.rels.get(index, ()):
            if typ in (R_NONE, R_V4BX):
                continue
            r = resolve(obj, k)
            if r[0] == "int":
                keep(r[1], r[2])

    # Layout: code, read-only data, unwind index (in code order), veneers | writable data, zeroed data.
    def kind(obj, index):
        name, typ, flags = obj.sec[index][0:3]
        assert not flags & SHF_TLS, f"{name}: a thread-local section of its own in {obj.path.name}"
        if typ == SHT_EXIDX:
            return 2
        if flags & SHF_EXEC:
            return 0
        if not flags & SHF_WRITE:
            return 1
        return 4 if typ == SHT_NOBITS else 3

    place = {}
    image = bytearray()
    text_order = [(o, i) for o, i in order if kind(o, i) == 0]
    exidx_order = [(o, o.exidx_of[i]) for o, i in text_order if i in o.exidx_of]
    groups = [text_order, [(o, i) for o, i in order if kind(o, i) == 1], exidx_order]
    exidx_start = exidx_count = 0
    for g, group in enumerate(groups):
        if g == 2:
            image.extend(bytes(-len(image) % 4))
            exidx_start = len(image)
        for obj, index in group:
            align = max(obj.sec[index][8], 1)
            image.extend(bytes(-len(image) % align))
            place[(id(obj), index)] = len(image)
            image.extend(obj.bytes_of(index))
        if g == 2:
            exidx_count = (len(image) - exidx_start) // 8
    # A last unwind entry at the end of the code: "cannot unwind" from there on.
    code_end = max((place[(id(o), i)] + o.sec[i][5] for o, i in text_order), default=0)
    terminator = len(image)
    image.extend(struct.pack("<II", (code_end - terminator) & 0x7FFFFFFF, 1))
    exidx_count += 1
    veneer_base = len(image)
    veneers = {}
    relocs_later = []
    # Reserve the veneers: one per external branch target.
    for obj, index in order:
        for off, typ, k in obj.rels.get(index, ()):
            if typ in (R_CALL, R_JUMP24):
                r = resolve(obj, k)
                if r[0] in ("ext", "abs") and r[1] not in veneers:
                    veneers[r[1]] = veneer_base + 8 * len(veneers)
    for target in veneers:
        image.extend(struct.pack("<II", 0xE51FF004, target))  # ldr pc, [pc, #-4] ; .word target
    text_size = len(image)
    image.extend(bytes(-len(image) % 0x1000))
    rx_size = len(image)
    for obj, index in order:
        if kind(obj, index) == 3:
            align = max(obj.sec[index][8], 1)
            image.extend(bytes(-len(image) % align))
            place[(id(obj), index)] = len(image)
            image.extend(obj.bytes_of(index))
    file_size = len(image)
    mem_size = file_size
    for obj, index in order:
        if kind(obj, index) == 4:
            align = max(obj.sec[index][8], 1)
            mem_size += -mem_size % align
            place[(id(obj), index)] = mem_size
            mem_size += obj.sec[index][5]

    fix_abs, fix_rel = [], []

    def addr_of(r):
        return place[(id(r[1]), r[2])] + r[3]

    for obj, index in order:
        if kind(obj, index) == 4:
            continue
        base = place[(id(obj), index)]
        for off, typ, k in obj.rels.get(index, ()):
            if typ in (R_NONE, R_V4BX):
                continue
            p = base + off
            word, = struct.unpack_from("<I", image, p)
            r = resolve(obj, k)
            internal = r[0] == "int"
            s = addr_of(r) if internal else r[1]
            if typ in (R_ABS32, R_TARGET1):
                word = (word + s) & 0xFFFFFFFF
                if internal:
                    fix_abs.append(p)
            elif typ == R_TLS_LE32:
                assert r[0] == "tls", f"TLS relocation against {obj.sym[k][0]} which is not thread-local in the executable"
                word = (word + s) & 0xFFFFFFFF
            elif typ == R_REL32:
                word = (word + s - p) & 0xFFFFFFFF
                if not internal:
                    fix_rel.append(p)
            elif typ == R_PREL31:
                a = word & 0x7FFFFFFF
                if a & 0x40000000:
                    a -= 0x80000000
                word = (word & 0x80000000) | ((a + s - p) & 0x7FFFFFFF)
                if not internal:
                    fix_rel.append(p | 0x80000000)  # 31 bits: the top one is kept
            elif typ in (R_CALL, R_JUMP24):
                a = word & 0xFFFFFF
                if a & 0x800000:
                    a -= 0x1000000
                a <<= 2
                if not internal:
                    assert a == -8, "branch to an external symbol with an addend"
                    s = veneers[s]
                delta = s + a - p
                assert -(1 << 25) <= delta < (1 << 25) and delta % 4 == 0, "branch out of range"
                word = (word & 0xFF000000) | ((delta >> 2) & 0xFFFFFF)
            else:
                raise SystemExit(f"{name}: relocation type {typ} in {obj.path.name} ({obj.sym[k][0]})")
            struct.pack_into("<I", image, p, word)
    if missing:
        raise SystemExit(f"{name}: unresolved symbols: {sorted(missing)[:12]} ({len(missing)})")
    if internal_data:
        report.append(f"{name}: data of its own (not shared with the executable): {sorted(internal_data)[:6]}")
    table = []
    for sym_name, guest in exports.items():
        if sym_name in defs:
            o, kk = defs[sym_name]
            table.append((guest, place[(id(o), o.sym[kk][4])] + o.sym[kk][1]))
    table.sort()
    header = struct.pack("<4s9I", b"B3PK", 1, rx_size, file_size, mem_size, len(fix_abs), len(fix_rel), len(table),
                         exidx_start, exidx_count)
    body = (struct.pack(f"<{len(fix_abs)}I", *fix_abs) + struct.pack(f"<{len(fix_rel)}I", *fix_rel) +
            b"".join(struct.pack("<II", g, o) for g, o in table) + bytes(image[:file_size]))
    return header + body + struct.pack("<I", zlib.crc32(body)), text_size, len(table), len(veneers)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--full", type=Path, required=True)
    ap.add_argument("--selection", type=Path, required=True)
    ap.add_argument("--layout", type=Path, required=True)
    ap.add_argument("--elf", type=Path, required=True)
    ap.add_argument("--build-ninja", type=Path, required=True)
    ap.add_argument("--work", type=Path, required=True)
    ap.add_argument("--out", type=Path, required=True)
    ap.add_argument("--only", nargs="*")
    ap.add_argument("--jobs", type=int, default=12)
    args = ap.parse_args()

    layout = json.loads(args.layout.read_text())
    modules = sorted((int(v["image"], 16), n) for n, v in layout.items() if isinstance(v, dict) and "image" in v)
    starts = [m[0] for m in modules]

    def module_of(address):
        j = bisect.bisect_right(starts, address) - 1
        return modules[j][1] if j >= 0 else "main.dol"

    wanted = lambda n: (n.startswith("ft_") or n.startswith("st_")) and (not args.only or n in args.only)
    # The functions each pack holds: its module's functions the executable has as stubs.
    per_module = defaultdict(dict)   # module -> {symbol: guest address}
    per_shard = defaultdict(set)     # shard file -> {address strings}
    shards_of = defaultdict(set)
    for path in sorted(args.selection.glob("shard_*.cpp")):
        text = path.read_text(encoding="utf-8")
        for mark in MARKER.finditer(text):
            address = mark.group(1)
            mod = module_of(int(address, 16))
            if wanted(mod) and f"Brawl3ds_NotLinked(0x{address}u" in text[mark.end(): mark.end() + 400]:
                per_module[mod]["func_" + address] = int(address, 16)
                per_shard[path.name].add(address)
                shards_of[mod].add(path.name)
    print(f"{len(per_module)} packs, {sum(len(v) for v in per_module.values())} functions, {len(per_shard)} shards")

    src_dir, obj_dir = args.work / "src", args.work / "obj"
    src_dir.mkdir(parents=True, exist_ok=True)
    obj_dir.mkdir(parents=True, exist_ok=True)
    args.out.mkdir(parents=True, exist_ok=True)
    compiler, flags = compile_flags(args.build_ninja)
    flags = [f for f in flags if not f.startswith("-IC:/b3ds/brawl3ds-work/shards")] + ["-I" + str(args.full).replace("\\", "/")]
    todo = []
    for shard, addresses in sorted(per_shard.items()):
        head, parts = split((args.full / shard).read_text(encoding="utf-8"))
        text = head + "".join(part for address, part in parts if address in addresses) + CLOSING + "\n"
        src = src_dir / shard
        obj = obj_dir / (shard + ".o")
        if not src.exists() or src.read_text(encoding="utf-8") != text:
            src.write_text(text, encoding="utf-8")
        if not obj.exists() or obj.stat().st_mtime < src.stat().st_mtime:
            todo.append((src, obj))

    def compile_one(job):
        src, obj = job
        r = subprocess.run([compiler, *flags, "-o", str(obj), "-c", str(src)], capture_output=True, text=True)
        return src.name, r.returncode, r.stderr[-2000:]

    if todo:
        print(f"compiling {len(todo)} shards with {args.jobs} jobs ...", flush=True)
        failed = 0
        with concurrent.futures.ThreadPoolExecutor(args.jobs) as pool:
            for n, (shard, code, err) in enumerate(pool.map(compile_one, todo)):
                if code != 0:
                    failed += 1
                    print(f"FAILED {shard}\n{err}")
        if failed:
            raise SystemExit(f"{failed} shards did not compile")

    main_syms, main_tls = read_main(args.elf)
    objs = {}
    report = []
    index_entries = []
    total = 0
    for mod in sorted(per_module):
        mod_objs = []
        for shard in sorted(shards_of[mod]):
            if shard not in objs:
                objs[shard] = Obj(obj_dir / (shard + ".o"))
            mod_objs.append(objs[shard])
        data, text_size, count, veneer_count = link_pack(mod, per_module[mod], mod_objs, main_syms, main_tls, report)
        (args.out / (mod + ".b3p")).write_bytes(data)
        guest = sorted(per_module[mod].values())
        index_entries.append((guest[0], guest[-1], mod))
        total += len(data)
        print(f"  {mod:16s} {count:5d} functions  {text_size >> 10:5d} KiB of code  {veneer_count:4d} veneers")
    probes = [main_syms[p] for p in PROBES]
    index = struct.pack("<4sI", b"B3PI", 1) + struct.pack(f"<{len(PROBES)}I", *probes) + struct.pack("<I", len(index_entries))
    for start, end, mod in sorted(index_entries):
        index += struct.pack("<II24s", start, end, mod.encode())
    (args.out / "index.bin").write_bytes(index)
    for line in report:
        print("note:", line)
    print(f"{len(index_entries)} packs, {total / 1048576:.1f} MiB in {args.out}")


if __name__ == "__main__":
    main()
