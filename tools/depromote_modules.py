"""Move the fighters' and stages' cold translated functions out of the executable, into their code packs.

A function of an ft_* or st_* module that the selection holds in a cold shard (compiled -Oz, like a pack) gains
nothing from being in the executable: there it takes memory whether its fighter is in the match or not. This makes
each a stub again (tools/build_packs.py then puts it in the module's pack), which frees the memory the pack arena
needs. Functions in hot shards (hot_shards.txt: -O2, inline memory access) stay.

Usage: python -I tools/depromote_modules.py <selection dir> <module_layout.json>
"""
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


def main():
    selection = Path(sys.argv[1])
    layout = json.loads(Path(sys.argv[2]).read_text())
    modules = sorted((int(v["image"], 16), n) for n, v in layout.items() if isinstance(v, dict) and "image" in v)
    starts = [m[0] for m in modules]
    hot = set((selection / "hot_shards.txt").read_text().split())
    moved = changed = 0
    for path in sorted(selection.glob("shard_*.cpp")):
        if path.name in hot:
            continue
        body = path.read_text(encoding="utf-8").rstrip()
        assert body.endswith(CLOSING)
        body = body[: -len(CLOSING)]
        marks = list(MARKER.finditer(body))
        out = [body[: marks[0].start()] if marks else body]
        touched = False
        for i, mark in enumerate(marks):
            end = marks[i + 1].start() if i + 1 < len(marks) else len(body)
            part = body[mark.start(): end]
            address = mark.group(1)
            j = bisect.bisect_right(starts, int(address, 16)) - 1
            name = modules[j][1] if j >= 0 else ""
            if (name.startswith("ft_") or name.startswith("st_")) and "Brawl3ds_NotLinked(0x" not in part \
                    and "Brawl3dsNative_" not in part:
                part = STUB.format(a=address)
                moved += 1
                touched = True
            out.append(part)
        if touched:
            out.append(CLOSING + "\n")
            path.write_text("".join(out), encoding="utf-8")
            changed += 1
    print(f"{moved} functions moved to the packs ({changed} shards rewritten)")


if __name__ == "__main__":
    main()
