"""Make a 3DS version of WiiCompiled's data_sections_init.cpp that reads the DOL/REL section images from the SD card.

On PC the translator embeds every section image (22 MiB for Brawl: main.dol's text/data and the pre-linked modules)
into the executable with .incbin, and InitializeDataSections() memcpy's them into guest RAM. On the 3DS that would
keep a second 22 MiB copy resident next to the Wii's RAM, which the memory budget can't afford. This rewrites the
generated initializer so each `memcpy(dest, kData_X, kSize_X)` becomes `Brawl3dsLoadBlob("X", dest, kSize_X)`,
which reads sdmc:/3ds/brawl3ds/blobs/X.bin straight into guest memory, and stages those files for the SD card.

Usage: python -I tools/gen_data_init_3ds.py <wiicompiled/generated> <out .cpp> <SD staging dir for blobs>
The blobs come from the user's own disc (the translator extracts them); they are never committed.
"""
import re
import shutil
import sys
from pathlib import Path

LOADER = r'''
// brawl3ds: section images are read from the SD card instead of being embedded (see tools/gen_data_init_3ds.py).
#include <cstdio>
#include <cstdlib>
extern "C" void Brawl3dsFatal(const char* what);
namespace {
void Brawl3dsLoadBlob(const char* name, uint8_t* destination, size_t size) {
    char path[128];
    std::snprintf(path, sizeof(path), "sdmc:/3ds/brawl3ds/blobs/%s.bin", name);
    std::FILE* file = std::fopen(path, "rb");
    if (!file) {
        char message[192];
        std::snprintf(message, sizeof(message), "missing %s (copy the blobs folder to the SD card)", path);
        Brawl3dsFatal(message);
        return;
    }
    const size_t read = std::fread(destination, 1, size, file);
    std::fclose(file);
    if (read != size) {
        char message[192];
        std::snprintf(message, sizeof(message), "%s: read %u of %u bytes (rebuild the blobs)", path,
                      static_cast<unsigned>(read), static_cast<unsigned>(size));
        Brawl3dsFatal(message);
    }
}
} // namespace
'''

COPY = re.compile(r"std::memcpy\((Memory::GetPointer\([^;]*?\)),\s*kData_(\w+),\s*kSize_(\w+)\);", re.S)
EXTERN = re.compile(r"^extern const uint8_t kData_\w+\[\];\n", re.M)
SIZE = re.compile(r"constexpr size_t kSize_(\w+) = (\d+)u;")


def main(generated: Path, out_cpp: Path, sd_blobs: Path):
    source = (generated / "data_sections_init.cpp").read_text(encoding="utf-8")
    sizes = {name: int(n) for name, n in SIZE.findall(source)}
    copies = COPY.findall(source)
    if not copies or len(copies) != len(sizes):
        sys.exit(f"unexpected data_sections_init.cpp layout: {len(copies)} copies, {len(sizes)} sizes")

    text = EXTERN.sub("", source)
    text = COPY.sub(lambda m: f'Brawl3dsLoadBlob("{m.group(2)}", {m.group(1)}, kSize_{m.group(3)});', text)
    text = text.replace('#include <iostream>\n', '#include <iostream>\n' + LOADER, 1)
    if "Brawl3dsLoadBlob(\"" not in text or "kData_" in text:
        sys.exit("rewrite failed: embedded blob references remain")
    out_cpp.parent.mkdir(parents=True, exist_ok=True)
    out_cpp.write_text("// brawl3ds: generated from data_sections_init.cpp by tools/gen_data_init_3ds.py\n" + text,
                       encoding="utf-8")

    sd_blobs.mkdir(parents=True, exist_ok=True)
    total = 0
    for _, name, _ in copies:
        blob = generated / "data_sections_init_blobs" / f"{name}.bin"
        if blob.stat().st_size != sizes[name]:
            sys.exit(f"{blob}: {blob.stat().st_size} bytes, initializer expects {sizes[name]}")
        shutil.copyfile(blob, sd_blobs / blob.name)
        total += sizes[name]
    print(f"{out_cpp.name}: {len(copies)} sections loaded from SD; staged {total / 2**20:.1f} MiB in {sd_blobs}")


if __name__ == "__main__":
    if len(sys.argv) != 4:
        sys.exit(__doc__)
    main(Path(sys.argv[1]), Path(sys.argv[2]), Path(sys.argv[3]))
