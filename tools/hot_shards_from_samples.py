"""More hot shards from a sampling profile of the game thread (brawl3ds-work/scripts/gdb_sample.py output).

Hot shards (port/headless/CMakeLists.txt: -O2 and inline guest loads/stores instead of -Oz and one shared call) cost
~30% more code each. tools/hot_shards.py chose the first set from the console profiler's top functions; this adds
the shards with the most samples among the rest, as many as the code budget allows.

Usage: python -I tools/hot_shards_from_samples.py <shard dir> <extra shards> <profile.txt> [more profiles ...]
Reads "== thread N" sections of each profile, takes the section with the most `func_XXXXXXXX` samples (the game
thread), adds up samples per shard and appends the best shards not already in <shard dir>/hot_shards.txt.
"""
import re
import sys
from collections import Counter
from pathlib import Path

MARKER = re.compile(r'^#line 1 "func_([0-9A-F]{8})\.cpp"$', re.M)
LINE = re.compile(r"^\s*[\d.]+%\s+(\d+)\s+func_([0-9A-F]{8})\s*$")


def game_thread_samples(path):
    best = Counter()
    current = None
    sections = []
    for line in Path(path).read_text(encoding="utf-8", errors="replace").split("\n"):
        if line.startswith("== "):
            current = Counter()
            sections.append(current)
        elif current is not None:
            m = LINE.match(line)
            if m:
                current[m.group(2)] += int(m.group(1))
    for section in sections:
        if sum(section.values()) > sum(best.values()):
            best = section
    return best


def main():
    shard_dir, extra = Path(sys.argv[1]), int(sys.argv[2])
    samples = Counter()
    for profile in sys.argv[3:]:
        samples.update(game_thread_samples(profile))
    where = {}
    for path in sorted(shard_dir.glob("shard_*.cpp")):
        for address in MARKER.findall(path.read_text(encoding="utf-8")):
            where[address] = path.name
    per_shard = Counter()
    for address, count in samples.items():
        if address in where:
            per_shard[where[address]] += count
    hot_file = shard_dir / "hot_shards.txt"
    hot = hot_file.read_text().split()
    total = sum(per_shard.values())
    in_hot = sum(c for s, c in per_shard.items() if s in hot)
    added = [s for s, _ in per_shard.most_common() if s not in hot][:extra]
    gain = sum(per_shard[s] for s in added)
    hot_file.write_text("\n".join(hot + added) + "\n")
    print(f"{total} samples in translated functions: {100.0 * in_hot / max(total, 1):.1f}% already in the {len(hot)} "
          f"hot shards; {len(added)} shards added hold {100.0 * gain / max(total, 1):.1f}% more")


if __name__ == "__main__":
    main()
