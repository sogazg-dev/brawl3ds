"""Keep only chosen Brawl modules' translated code; replace every other function with a tiny stub.

The whole game's translated code is ~95 MiB at -Oz on the ARM11, more than the 3DS has room for next to
the Wii's RAM. This writes a copy of WiiCompiled's base_common shards where every function outside the kept
modules becomes

    extern "C" void func_XXXXXXXX(CpuContext* ctx) { Brawl3ds_NotLinked(0xXXXXXXXX, ctx); }

so the dispatch and registration tables (which name all 120k functions) still link, and a call into
left-out code stops with a report naming the function instead of crashing. main.dol is always kept.

Usage:
  python -I tools/select_translated_code.py <build_shards/base_common> <module_layout.json> <out dir> MODE
  MODE: --keep "sora_melee sora_scene ft_mario ..."  module names (main.dol always kept)
        --keep-all
        --keep-covered a.bin [b.bin ...]               only functions that ran in any of these coverage runs
        --instrument-coverage                        keep all, and record which functions run: build the PC
                                                     version with these shards, play, and it writes
                                                     brawl3ds_coverage.bin beside the executable at exit

Shards are split on the translator's per-function markers (`#line 1 "func_<addr>.cpp"`); the shard header
(includes, traits, the MKW_STATIC_TRANSLATED_CALL macro) and its closing #undef are kept as they are.
Unchanged shards are not rewritten, so incremental builds only recompile what a new selection touched.
"""
import argparse
import bisect
import json
import re
import sys
from pathlib import Path

MARKER = re.compile(r'^#line 1 "func_([0-9A-F]{8})\.cpp"$', re.M)
CLOSING = "#undef MKW_STATIC_TRANSLATED_CALL"
STUB = ('#line 1 "func_{a}.cpp"\n'
        'extern "C" void Brawl3ds_NotLinked(uint32_t guestAddress, CpuContext* ctx);\n'
        'extern "C" void func_{a}(CpuContext* MKW_RESTRICT ctx) {{ Brawl3ds_NotLinked(0x{a}u, ctx); }}\n\n')

# Coverage: one bit per guest instruction address in 0x80000000..0x95800000 (the translator's image window).
COVER_BASE, COVER_END = 0x80000000, 0x95800000
COVER_WORDS = ((COVER_END - COVER_BASE) >> 2) // 32
COVER_HEADER = f'''
// brawl3ds coverage instrumentation (tools/select_translated_code.py --instrument-coverage)
extern "C" unsigned int g_brawl3dsCoverage[{COVER_WORDS}];
extern "C" int g_brawl3dsCoverageStarted;
extern "C" void Brawl3dsCoverageStart();
#define BRAWL3DS_COVER(address) ((g_brawl3dsCoverageStarted ? (void)0 : Brawl3dsCoverageStart()), \\
    g_brawl3dsCoverage[((address) - 0x{COVER_BASE:08X}u) >> 7] |= 1u << ((((address) - 0x{COVER_BASE:08X}u) >> 2) & 31u))
'''
# Defined once, in the first shard. The runtime ends the process without running static destructors, so the bitmap
# is written every 5 seconds by a thread the first instrumented call starts (not from a static constructor: that
# would run while Windows is still loading the engine DLL).
COVER_STORAGE = f'''
#include <chrono>
#include <cstdio>
#include <thread>
extern "C" unsigned int g_brawl3dsCoverage[{COVER_WORDS}] = {{}};
extern "C" int g_brawl3dsCoverageStarted = 0;
extern "C" void Brawl3dsCoverageStart() {{
    g_brawl3dsCoverageStarted = 1;
    std::thread([] {{
        for (;;) {{
            std::this_thread::sleep_for(std::chrono::seconds(5));
            if (std::FILE* file = std::fopen("brawl3ds_coverage.tmp", "wb")) {{
                std::fwrite(g_brawl3dsCoverage, sizeof(g_brawl3dsCoverage), 1, file);
                std::fclose(file);
                std::remove("brawl3ds_coverage.bin");
                std::rename("brawl3ds_coverage.tmp", "brawl3ds_coverage.bin");
            }}
        }}
    }}).detach();
}}
'''
FUNCTION_OPEN = re.compile(r'(extern "C" void func_([0-9A-F]{8})\(CpuContext\* MKW_RESTRICT ctx\)\n\{\n)')


def covered_addresses(bitmap_path: Path):
    data = bitmap_path.read_bytes()
    words = len(data) // 4
    found = set()
    for w in range(words):
        value = int.from_bytes(data[w * 4: w * 4 + 4], "little")
        while value:
            bit = (value & -value).bit_length() - 1
            found.add(COVER_BASE + ((w * 32 + bit) << 2))
            value &= value - 1
    return found


def instrument(text: str, first: bool) -> str:
    """Adds BRAWL3DS_COVER(address) as the first statement of every translated function."""
    body = FUNCTION_OPEN.sub(lambda m: m.group(1) + f"    BRAWL3DS_COVER(0x{m.group(2)}u);\n", text)
    head_end = MARKER.search(body).start() if MARKER.search(body) else 0
    return body[:head_end] + COVER_HEADER + (COVER_STORAGE if first else "") + body[head_end:]


def module_lookup(layout_path: Path):
    layout = json.loads(layout_path.read_text(encoding="utf-8"))
    ranges = sorted((int(v["image"], 16), int(v["image"], 16) + int(v["fix_size"], 16), k) for k, v in layout.items())
    starts = [r[0] for r in ranges]

    def module_of(address: int) -> str:
        i = bisect.bisect_right(starts, address) - 1
        return ranges[i][2] if i >= 0 and address < ranges[i][1] else "main.dol"
    return module_of, {name for _, _, name in ranges}


def rewrite(text: str, keep_function):
    """keep_function(address) -> bool. Returns (new text, kept count, stubbed count)."""
    body = text.rstrip()
    if not body.endswith(CLOSING):
        raise ValueError("shard does not end with " + CLOSING)
    body = body[: -len(CLOSING)]
    marks = list(MARKER.finditer(body))
    if not marks:
        return text, 0, 0
    out = [body[: marks[0].start()]]
    kept = stubbed = 0
    for i, mark in enumerate(marks):
        end = marks[i + 1].start() if i + 1 < len(marks) else len(body)
        address = mark.group(1)
        if keep_function(int(address, 16)):
            out.append(body[mark.start(): end])
            kept += 1
        else:
            out.append(STUB.format(a=address))
            stubbed += 1
    out.append(CLOSING + "\n")
    return "".join(out), kept, stubbed


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("shards", type=Path)
    ap.add_argument("layout", type=Path)
    ap.add_argument("out", type=Path)
    group = ap.add_mutually_exclusive_group(required=True)
    group.add_argument("--keep", help="space-separated module names to keep (main.dol is always kept)")
    group.add_argument("--keep-all", action="store_true")
    group.add_argument("--keep-covered", type=Path, nargs="+",
                       help="keep only functions that ran in a coverage run (brawl3ds_coverage.bin)")
    group.add_argument("--instrument-coverage", action="store_true",
                       help="keep everything and record which functions run (for a PC coverage build)")
    args = ap.parse_args()

    module_of, modules = module_lookup(args.layout)
    if args.keep:
        keep = set(args.keep.split()) | {"main.dol"}
        unknown = keep - modules - {"main.dol"}
        if unknown:
            sys.exit("unknown module(s): " + " ".join(sorted(unknown)))
        keep_function = lambda address: module_of(address) in keep  # noqa: E731
    elif args.keep_covered:
        covered = set().union(*(covered_addresses(path) for path in args.keep_covered))
        print(f"coverage: {len(covered)} function entry points ran")
        keep_function = covered.__contains__
    else:
        keep_function = lambda address: True  # noqa: E731

    args.out.mkdir(parents=True, exist_ok=True)
    total_kept = total_stubbed = written = 0
    sources = sorted(args.shards.glob("shard_*.cpp"))
    for index, source in enumerate(sources):
        new, kept, stubbed = rewrite(source.read_text(encoding="utf-8"), keep_function)
        if args.instrument_coverage:
            new = instrument(new, first=index == 0)
        total_kept += kept
        total_stubbed += stubbed
        for path, content in ((args.out / source.name, new),
                              (args.out / (source.stem + "_traits.h"),
                               (args.shards / (source.stem + "_traits.h")).read_text(encoding="utf-8"))):
            if not path.exists() or path.read_text(encoding="utf-8") != content:
                path.write_text(content, encoding="utf-8")
                written += 1
    print(f"{len(sources)} shards: {total_kept} functions kept, {total_stubbed} stubbed, {written} files (re)written")


if __name__ == "__main__":
    main()
