"""Replace translated functions in a shard selection by calls to native ARM implementations.

port/wiicompiled_arm32/natives_3ds.cpp defines `extern "C" void Brawl3dsNative_<ADDRESS>(CpuContext*)` for guest
functions worth writing by hand (the SDK's paired-single matrix code, memory fills). This tool makes the translated
body of each a call to its native, in the selection the build compiles; every caller, static or through the
dispatch tables, then reaches the native. A function whose native is removed needs its shard restored from the
full set (tools/select_translated_code.py) before this runs again.

Usage: python -I tools/apply_natives.py <natives_3ds.cpp> <selection dir>
Only shards that change are rewritten.
"""
import re
import sys
from pathlib import Path

MARKER = re.compile(r'^#line 1 "func_([0-9A-F]{8})\.cpp"$', re.M)
CLOSING = "#undef MKW_STATIC_TRANSLATED_CALL"
NATIVE = re.compile(r'extern "C" void Brawl3dsNative_([0-9A-F]{8})\(CpuContext\*')
STUB = ('#line 1 "func_{a}.cpp"\n'
        'extern "C" void Brawl3dsNative_{a}(CpuContext* ctx);\n'
        'extern "C" void func_{a}(CpuContext* MKW_RESTRICT ctx) {{ Brawl3dsNative_{a}(ctx); }}\n\n')


def main():
    natives = set(NATIVE.findall(Path(sys.argv[1]).read_text(encoding="utf-8")))
    selection = Path(sys.argv[2])
    done = set()
    changed = 0
    for path in sorted(selection.glob("shard_*.cpp")):
        text = path.read_text(encoding="utf-8")
        marks = [m for m in MARKER.finditer(text) if m.group(1) in natives]
        if not marks:
            continue
        body = text.rstrip()
        assert body.endswith(CLOSING), f"{path.name} does not end with {CLOSING}"
        body = body[: -len(CLOSING)]
        all_marks = list(MARKER.finditer(body))
        out = [body[: all_marks[0].start()]]
        touched = False
        for i, mark in enumerate(all_marks):
            end = all_marks[i + 1].start() if i + 1 < len(all_marks) else len(body)
            part = body[mark.start(): end]
            address = mark.group(1)
            if address in natives:
                done.add(address)
                if "Brawl3dsNative_" not in part:
                    assert "Brawl3ds_NotLinked(0x" not in part, f"func_{address} is not in the selection"
                    part = STUB.format(a=address)
                    touched = True
            out.append(part)
        if touched:
            out.append(CLOSING + "\n")
            path.write_text("".join(out), encoding="utf-8")
            changed += 1
    missing = sorted(natives - done)
    assert not missing, f"natives without a translated function in {selection}: {missing}"
    print(f"{len(natives)} natives; {changed} shards rewritten")


if __name__ == "__main__":
    main()
