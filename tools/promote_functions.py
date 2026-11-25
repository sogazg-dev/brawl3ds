"""Add interpreted functions to the translated set: the costliest in interp_profile.txt, within a budget.

The 3DS build links the translated functions the VS demo runs and interprets the rest (main_3ds.cpp,
Brawl3ds_NotLinked). Real play leaves that set at once: other fighters, stages, menus. The play build writes
sdmc:/3ds/brawl3ds/interp_profile.txt ("<address> <calls> <own instructions>", costliest first); this tool restores
the translated bodies of the costliest from the full shards, until the added source reaches a budget (translated
C++ source bytes are a fair measure of the ARM code they become: ~0.19 bytes of code per byte of source at -Oz).

Usage: python -I tools/promote_functions.py <full shards dir> <selection dir> <budget KiB of source>
                                             <interp_profile.txt> [more profiles ...]
Profiles are summed. Functions already translated are skipped. Only shards that change are rewritten.
Prints what it added and how much of the profiles' interpreted cost that covers.
"""
import re
import sys
from collections import defaultdict
from pathlib import Path

MARKER = re.compile(r'^#line 1 "func_([0-9A-F]{8})\.cpp"$', re.M)
CLOSING = "#undef MKW_STATIC_TRANSLATED_CALL"
CALL_COST = 40  # instructions a call into the interpreter is worth (dispatch, frame, profile)


def split(text):
    body = text.rstrip()
    assert body.endswith(CLOSING), "shard does not end with " + CLOSING
    body = body[: -len(CLOSING)]
    marks = list(MARKER.finditer(body))
    head = body[: marks[0].start()] if marks else body
    parts = []
    for i, mark in enumerate(marks):
        end = marks[i + 1].start() if i + 1 < len(marks) else len(body)
        parts.append((mark.group(1), body[mark.start(): end]))
    return head, parts


def main():
    full_dir, selection, budget = Path(sys.argv[1]), Path(sys.argv[2]), int(sys.argv[3]) * 1024
    cost = defaultdict(int)
    for profile in sys.argv[4:]:
        for line in Path(profile).read_text().split("\n"):
            fields = line.split()
            if len(fields) == 3:
                cost[fields[0].upper()] += int(fields[2]) + CALL_COST * int(fields[1])
    total = sum(cost.values())
    # Where each stubbed function of the selection lives.
    where = {}
    for path in sorted(selection.glob("shard_*.cpp")):
        text = path.read_text(encoding="utf-8")
        for mark in MARKER.finditer(text):
            address = mark.group(1)
            if address in cost and f"Brawl3ds_NotLinked(0x{address}u" in text[mark.end(): mark.end() + 400]:
                where[address] = path.name
    full_cache = {}

    def full_part(shard, address):
        if shard not in full_cache:
            full_cache[shard] = dict(split((full_dir / shard).read_text(encoding="utf-8"))[1])
        return full_cache[shard].get(address)

    chosen = defaultdict(dict)
    used = covered = 0
    skipped_big = 0
    for address in sorted(where, key=lambda a: -cost[a]):
        part = full_part(where[address], address)
        if part is None or "Brawl3ds_NotLinked(0x" in part:
            continue
        # Cheapest cost first is not the aim: a function must repay its size. Skip one that would take the rest of
        # the budget for under a thousandth of the total.
        if used + len(part) > budget:
            skipped_big += 1
            continue
        chosen[where[address]][address] = part
        used += len(part)
        covered += cost[address]
    for shard, parts in chosen.items():
        path = selection / shard
        head, sel_parts = split(path.read_text(encoding="utf-8"))
        out = [head] + [parts.get(address, part) for address, part in sel_parts] + [CLOSING + "\n"]
        path.write_text("".join(out), encoding="utf-8")
    count = sum(len(p) for p in chosen.values())
    already = sum(c for a, c in cost.items() if a not in where)
    print(f"{count} functions added ({used >> 10} KiB of source, {len(chosen)} shards); they are "
          f"{100.0 * covered / max(total, 1):.1f}% of the profiled interpreter cost; "
          f"{100.0 * already / max(total, 1):.1f}% was not a stub of this selection (veneers, already translated); "
          f"{skipped_big} did not fit the budget")


if __name__ == "__main__":
    main()
