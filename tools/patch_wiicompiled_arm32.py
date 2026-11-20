"""Teach a WiiCompiled tree (Brawl Recompiled's wiicompiled/) to compile for the 3DS's 32-bit ARM11.

Usage: python -I tools/patch_wiicompiled_arm32.py <path to wiicompiled>

What it changes (all guarded by __arm__ && !__aarch64__, so x86-64 and AArch64 builds are untouched):
  * isa/ppc_isa_config.h        include our scalar NEON stand-in instead of <arm_neon.h>
  * isa/ppc_isa_float.h,        reuse the AArch64 paths (they only use the intrinsics the shim provides)
    isa/ppc_isa_quantized.h
  * isa/ppc_isa_fpenv.h         FPSCR.FZ (bit 24, same position as AArch64 FPCR.FZ) via vmrs/vmsr
  * guest_flat_memory.h         no 4 GiB flat reservation on a 32-bit host: the flat fast paths are
                                compiled out (RequiresCheckedAccess() is constexpr true), and every
                                guest access goes through the runtime's 1 MiB page table instead
  * memory_access.h             FlatLoad/FlatStore (used directly by the translator's proven-RAM
                                writes) go through that page table too
  * runtime_config.h            the graphics-API list gets a 3DS entry ("auto")
  * system_bridge.h             newlib has no sigjmp_buf: use jmp_buf on the 3DS

Idempotent: a file already patched is left alone. Fails loudly if an expected upstream snippet moved.
"""
import shutil
import sys
from pathlib import Path

MARK = "brawl3ds-arm32"
ARM32 = "(defined(__arm__) && !defined(__aarch64__))"


def edit(path: Path, replacements):
    # Some upstream headers start with a UTF-8 BOM; drop it so our marker line can go first.
    raw = path.read_text(encoding="utf-8")
    text = raw.replace("﻿", "")
    if MARK in text:
        if text != raw:
            path.write_text(text, encoding="utf-8")
        print(f"  already patched: {path.name}")
        return
    for old, new, count in replacements:
        found = text.count(old)
        if found == 0 or (count is not None and found != count):
            sys.exit(f"{path}: expected {count or 'some'} occurrence(s) of:\n{old}\nfound {found}")
        text = text.replace(old, new)
    path.write_text(f"// {MARK}: patched by brawl3ds tools/patch_wiicompiled_arm32.py\n" + text, encoding="utf-8")
    print(f"  patched: {path.name}")


def main(wiicompiled: Path):
    inc = wiicompiled / "runtime" / "include"
    isa = inc / "isa"
    if not (isa / "ppc_isa_config.h").is_file():
        sys.exit(f"{wiicompiled} does not look like a WiiCompiled tree")

    shim = Path(__file__).resolve().parent.parent / "port" / "wiicompiled_arm32" / "brawl3ds_neon_shim.h"
    shutil.copyfile(shim, inc / shim.name)
    print(f"  copied: {shim.name}")

    edit(isa / "ppc_isa_config.h", [(
        "#elif defined(__aarch64__)\n#include <arm_neon.h>\n",
        "#elif defined(__aarch64__)\n#include <arm_neon.h>\n#elif defined(__arm__)\n#include \"brawl3ds_neon_shim.h\"\n",
        1)])

    for name in ("ppc_isa_float.h", "ppc_isa_quantized.h"):
        edit(isa / name, [("defined(__aarch64__)", "(defined(__aarch64__) || defined(__arm__))", None)])

    edit(isa / "ppc_isa_fpenv.h", [
        ("#else\n#error \"ppc_isa_fpenv.h has no host FP control register mapping",
         "#elif defined(__arm__)\n// VFP FPSCR: FZ is bit 24, as in AArch64 FPCR.\n"
         "inline constexpr uint32_t kMkwFpControlFlushToZeroBits = (1u << 24); // FZ\n"
         "#else\n#error \"ppc_isa_fpenv.h has no host FP control register mapping", 1),
        ("    return static_cast<uint32_t>(fpcr);\n#endif",
         "    return static_cast<uint32_t>(fpcr);\n#elif defined(__arm__)\n    uint32_t fpscr = 0;\n"
         "    __asm__ __volatile__(\"vmrs %0, fpscr\" : \"=r\"(fpscr));\n    return fpscr;\n#endif", 1),
        ("    __asm__ __volatile__(\"msr fpcr, %0\" :: \"r\"(fpcr));\n#endif",
         "    __asm__ __volatile__(\"msr fpcr, %0\" :: \"r\"(fpcr));\n#elif defined(__arm__)\n"
         "    __asm__ __volatile__(\"vmsr fpscr, %0\" :: \"r\"(value));\n#endif", 1),
    ])

    edit(inc / "guest_flat_memory.h", [
        ("#else\n#error \"guest_flat_memory.h has no fixed flat guest base",
         "#elif defined(__arm__)\n// No 4 GiB reservation fits a 32-bit address space. The base is never dereferenced:\n"
         "// RequiresCheckedAccess() below is constexpr true, which compiles every flat path out.\n"
         "inline constexpr uintptr_t kFixedFlatGuestBase = 0;\n"
         "#else\n#error \"guest_flat_memory.h has no fixed flat guest base", 1),
        ("#if defined(MKW_GUEST_FLAT_FIXED_PAGE_SIZE)\ninline constexpr bool RequiresCheckedAccess() noexcept { return false; }",
         f"#if {ARM32}\ninline constexpr bool RequiresCheckedAccess() noexcept {{ return true; }}\n"
         "#elif defined(MKW_GUEST_FLAT_FIXED_PAGE_SIZE)\ninline constexpr bool RequiresCheckedAccess() noexcept { return false; }", 1),
    ])

    edit(inc / "memory_access.h", [
        ("MKW_MEMORY_FORCE_INLINE T FlatLoad(uint32_t address) {\n",
         "MKW_MEMORY_FORCE_INLINE T FlatLoad(uint32_t address) {\n"
         f"#if {ARM32}\n"
         "    if constexpr (sizeof(T) == 1) return static_cast<T>(Memory::Read8(address));\n"
         "    else if constexpr (sizeof(T) == 2) return static_cast<T>(Memory::Read16(address));\n"
         "    else if constexpr (sizeof(T) == 4) return static_cast<T>(Memory::Read32(address));\n"
         "    else return static_cast<T>(Memory::Read64(address));\n"
         "#endif\n", 1),
        ("MKW_MEMORY_FORCE_INLINE void FlatStore(uint32_t address, T value) {\n",
         "MKW_MEMORY_FORCE_INLINE void FlatStore(uint32_t address, T value) {\n"
         f"#if {ARM32}\n"
         "    if constexpr (sizeof(T) == 1) Memory::Write8(address, static_cast<uint8_t>(value));\n"
         "    else if constexpr (sizeof(T) == 2) Memory::Write16(address, static_cast<uint16_t>(value));\n"
         "    else if constexpr (sizeof(T) == 4) Memory::Write32(address, static_cast<uint32_t>(value));\n"
         "    else Memory::Write64(address, static_cast<uint64_t>(value));\n"
         "    return;\n"
         "#endif\n", 1),
    ])

    edit(inc / "runtime_config.h", [
        ('#elif defined(_WIN32)\n    static constexpr std::array<std::string_view, 3> values{"auto", "d3d12", "vulkan"};\n#endif',
         '#elif defined(_WIN32)\n    static constexpr std::array<std::string_view, 3> values{"auto", "d3d12", "vulkan"};\n'
         '#elif defined(__3DS__)\n    static constexpr std::array<std::string_view, 1> values{"auto"};  // PICA200 only\n#endif', 1),
    ])

    edit(inc / "system_bridge.h", [
        ("#if defined(_WIN32)\nusing MkwJmpBuf = jmp_buf;",
         "#if defined(_WIN32) || defined(__3DS__)\nusing MkwJmpBuf = jmp_buf;", 1),
    ])


if __name__ == "__main__":
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    main(Path(sys.argv[1]))
