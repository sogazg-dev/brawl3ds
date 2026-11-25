"""Compare the game's memory between two deterministic runs of the 3DS build.

A run with sdmc:/3ds/brawl3ds/det_hash.txt ("<every> [<dump frame> ...]") writes det_hash_out.txt (one line per
sampled frame: the frame, then a hash per MiB of MEM1, MEM2 and the module homes) and, at the dump frames,
mem_<frame>_<region>.bin. Keep each run's files in its own folder.

Usage: python -I tools/det_compare.py <run A folder> <run B folder> [--ignore <file>]
Prints the first frame whose hashes differ and the MiB blocks that differ there; then, for every dump frame both
runs have, the differing byte ranges as guest addresses. Exit code 0 when everything matches, 1 otherwise.

--ignore names a text file of guest ranges that are allowed to differ, one "<start hex> <end hex> [comment]" per
line (end exclusive). Blocks wholly explained by ignored ranges need a dump to be told apart, so with --ignore the
hash lines are only reported, and the verdict comes from the dumps.
"""
import sys
from pathlib import Path

MIB = 1 << 20
REGIONS = [("mem1", 0x80000000, 24), ("mem2", 0x90000000, 54), ("homes", 0x94000000, 12)]


def block_address(index):
    for name, base, blocks in REGIONS:
        if index < blocks:
            return name, base + index * MIB
        index -= blocks
    return "?", 0


def read_hashes(folder):
    lines = {}
    path = folder / "det_hash_out.txt"
    if path.exists():
        for line in path.read_text().splitlines():
            parts = line.split()
            if len(parts) > 1:
                lines[int(parts[0])] = parts[1:]
    return lines


def read_ignore(path):
    ranges = []
    for line in Path(path).read_text().splitlines():
        parts = line.split("#")[0].split()
        if len(parts) >= 2:
            ranges.append((int(parts[0], 16), int(parts[1], 16)))
    return ranges


def diff_ranges(a, b, base, gap=16):
    """Differing byte ranges of two equal-sized images, merging ranges closer than `gap` bytes."""
    ranges = []
    step = 1 << 16
    for at in range(0, len(a), step):
        if a[at: at + step] == b[at: at + step]:
            continue
        for i in range(at, min(at + step, len(a))):
            if a[i] != b[i]:
                if ranges and i - ranges[-1][1] <= gap:
                    ranges[-1][1] = i + 1
                else:
                    ranges.append([i, i + 1])
    return [(base + s, base + e) for s, e in ranges]


def main():
    args = [a for a in sys.argv[1:] if a != "--ignore"]
    ignore = []
    if "--ignore" in sys.argv:
        ignore = read_ignore(sys.argv[sys.argv.index("--ignore") + 1])
        args.remove(sys.argv[sys.argv.index("--ignore") + 1])
    a_dir, b_dir = Path(args[0]), Path(args[1])
    a, b = read_hashes(a_dir), read_hashes(b_dir)
    frames = sorted(set(a) & set(b))
    hash_differs = False
    print(f"hash lines: {len(a)} and {len(b)}, {len(frames)} frames in common")
    for frame in frames:
        blocks = [i for i, (x, y) in enumerate(zip(a[frame], b[frame])) if x != y]
        if blocks:
            hash_differs = True
            print(f"first differing frame: {frame} ({len(blocks)} MiB blocks differ)")
            for i in blocks[:40]:
                name, address = block_address(i)
                print(f"  {name} {address:08X}-{address + MIB:08X}")
            previous = [f for f in frames if f < frame]
            print(f"  last identical frame: {previous[-1] if previous else 'none'}")
            break
    else:
        if frames:
            print(f"all {len(frames)} sampled frames identical (up to frame {frames[-1]})")

    dump_differs = False
    dumps = 0
    for path in sorted(a_dir.glob("mem_*_*.bin")):
        other = b_dir / path.name
        if not other.exists():
            continue
        dumps += 1
        region = path.stem.split("_")[2]
        base = next(base for name, base, _ in REGIONS if name == region)
        x, y = path.read_bytes(), other.read_bytes()
        if x == y:
            print(f"{path.name}: identical")
            continue
        ranges = diff_ranges(x, y, base)
        kept = [(s, e) for s, e in ranges if not any(s >= lo and e <= hi for lo, hi in ignore)]
        total = sum(e - s for s, e in kept)
        print(f"{path.name}: {len(ranges)} differing ranges, {len(ranges) - len(kept)} ignored, "
              f"{len(kept)} left ({total} bytes)")
        for s, e in kept[:60]:
            off = s - base
            print(f"  {s:08X}-{e:08X} ({e - s:5d} bytes)  A {x[off: off + min(e - s, 16)].hex()}  "
                  f"B {y[off: off + min(e - s, 16)].hex()}")
        if len(kept) > 60:
            print(f"  ... {len(kept) - 60} more")
        dump_differs |= bool(kept)

    if not frames and not dumps:
        print("RESULT: NOTHING COMPARED (no sampled frame or memory image in common)")
        sys.exit(2)
    if ignore and dumps:
        failed = dump_differs
    else:
        failed = hash_differs or dump_differs
    print("RESULT: " + ("DIFFERENT" if failed else "identical"))
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
