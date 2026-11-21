"""Drop the stubs that a linked library now defines from tools/gen_link_stubs.py's output.

  python -I tools/filter_link_stubs.py <link_stubs.cpp> <names.txt> <out.cpp>

gen_link_stubs.py writes one `extern "C" ... Name(...) { ... }` line per missing C function. When the 3DS build
starts linking real code for some of them (Aurora's GX front end, port/headless/CMakeLists.txt b3_aurora), those
stubs would be duplicate definitions. names.txt lists the function names to drop, one per line ('#' comments).
The output is only rewritten when its contents change, so the build does not recompile it every time.
"""
import re
import sys
from pathlib import Path


def main():
    if len(sys.argv) != 4:
        sys.exit(__doc__)
    stubs, names_file, out = (Path(a) for a in sys.argv[1:4])
    names = {line.strip() for line in names_file.read_text(encoding="utf-8").splitlines()
             if line.strip() and not line.startswith("#")}
    kept, dropped = [], 0
    for line in stubs.read_text(encoding="utf-8").splitlines(keepends=True):
        m = re.match(r'extern "C" .*?> ([A-Za-z_][A-Za-z_0-9]*)\(', line)
        if m and m.group(1) in names:
            dropped += 1
            continue
        kept.append(line)
    text = "".join(kept)
    if not out.exists() or out.read_text(encoding="utf-8") != text:
        out.write_text(text, encoding="utf-8")
    print(f"filter_link_stubs: dropped {dropped} of {len(names)} names")


if __name__ == "__main__":
    main()
