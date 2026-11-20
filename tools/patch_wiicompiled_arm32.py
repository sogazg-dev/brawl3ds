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
  * host_context.cpp            3DS fiber backend (port/wiicompiled_arm32/host_context_3ds.inc + co_switch_arm32.S)
  * memory.h, memory.cpp        on the 3DS MEM2 covers 54 MiB (game + IOS area) plus a separate 12 MiB module-homes
                                region, instead of the PC's 128 MiB; ax_memory.cpp keeps the PC's EXRAM mask
  * wii_es_crypto.h             tick-seeded RNG for NAND file signatures (no OS entropy source)
  * hle/network_poll_contract.h poll-flag fallbacks for newlib
  * main.cpp                    POSIX fault handler compiled out (newlib has no si_addr)
  Also copies port/wiicompiled_arm32/guest_flat_memory_3ds.cpp into runtime/src/platform/3ds/ (the 3DS build uses
  it instead of guest_flat_memory.cpp).
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

    port = Path(__file__).resolve().parent.parent / "port" / "wiicompiled_arm32"
    src = wiicompiled / "runtime" / "src"
    (src / "platform" / "3ds").mkdir(parents=True, exist_ok=True)
    for name, dest in [("brawl3ds_neon_shim.h", inc / "brawl3ds_neon_shim.h"),
                       ("host_context_3ds.inc", src / "host_context_3ds.inc"),
                       ("co_switch_arm32.S", src / "platform" / "3ds" / "co_switch.S"),
                       ("guest_flat_memory_3ds.cpp", src / "platform" / "3ds" / "guest_flat_memory_3ds.cpp")]:
        shutil.copyfile(port / name, dest)
        print(f"  copied: {name}")

    # Fibers: the 3DS backend (stack-swap, like the AArch64 one) in host_context_3ds.inc + co_switch.S.
    edit(src / "host_context.cpp", [
        ("#else\n#error \"HostContext needs a supported cooperative-context backend\"",
         "#elif defined(__3DS__)\n#include <cstdlib>\n\n"
         "extern \"C\" void mkw_co_switch(void** targetSp, void** sourceSp);\n"
         "extern \"C\" void* mkw_co_init(void* stackTop, void (*entry)(void*), void* argument);\n"
         "#else\n#error \"HostContext needs a supported cooperative-context backend\"", 1),
        ("\n#endif\n\n} // namespace HostContext",
         "\n#elif defined(__3DS__)\n\n#include \"host_context_3ds.inc\"\n\n#endif\n\n} // namespace HostContext", 1),
    ])

    # MEM2: the PC backs 128 MiB (the game's 64 MiB plus Brawl's pre-linked module homes from 0x94000000). The game's
    # arena and the IOS IPC area end at 0x93600000 (system_bridge.cpp kIosMem2End) and the homes end at 0x94BA3300,
    # so on the 3DS MEM2 and its mirrors cover 54 MiB and the homes are a separate 12 MiB region: 66 MiB, not 128.
    edit(inc / "memory.h", [
        ("    static constexpr size_t kMem2Size = 128u * 1024u * 1024u;",
         "#if defined(__3DS__)\n    static constexpr size_t kMem2Size = 0x03600000u;  // 54 MiB: game MEM2 + IOS IPC area\n"
         "    static constexpr uint32_t kModuleHomesBase = 0x94000000u;  // Brawl's pre-linked module homes\n"
         "    static constexpr size_t kModuleHomesSize = 0x00C00000u;\n"
         "#else\n    static constexpr size_t kMem2Size = 128u * 1024u * 1024u;\n#endif", 1),
    ])
    edit(src / "memory.cpp", [
        ("        .name = \"MEM2_UNCACHED\",\n        .baseAddress = Memory::kMem2UncachedBase,\n"
         "        .sizeBytes = Memory::kMem2Size,\n    });\n",
         "        .name = \"MEM2_UNCACHED\",\n        .baseAddress = Memory::kMem2UncachedBase,\n"
         "        .sizeBytes = Memory::kMem2Size,\n    });\n"
         "#if defined(__3DS__)\n    config.regions.push_back(RegionConfig{\n        .name = \"MODULE_HOMES\",\n"
         "        .baseAddress = Memory::kModuleHomesBase,\n        .sizeBytes = Memory::kModuleHomesSize,\n    });\n#endif\n", 1),
        # The Kamek module overlay (2 MiB above MEM1) is for Mario Kart Wii's Kamek mods; Brawl never uses it.
        ("    config.regions.push_back(RegionConfig{\n        .name = \"MEM1_KAMEK_OVERLAY\",",
         "#if !defined(__3DS__)  // Mario Kart Wii mods only: saves 2 MiB on the 3DS\n"
         "    config.regions.push_back(RegionConfig{\n        .name = \"MEM1_KAMEK_OVERLAY\",", 1),
        ("        .baseAddress = 0x81800000,\n        .sizeBytes = 0x200000,\n    });\n",
         "        .baseAddress = 0x81800000,\n        .sizeBytes = 0x200000,\n    });\n#endif\n", 1),
    ])
    # The DSP's EXRAM alias decode masks with kMem2Size - 1, which assumed a power-of-two size: keep the PC mask.
    edit(src / "hle" / "audio" / "ax_memory.cpp", [
        ("        const uint32_t exramOffset = addr & static_cast<uint32_t>(Memory::kMem2Size - 1u);",
         "#if defined(__3DS__)\n        const uint32_t exramOffset = addr & 0x07FFFFFFu;  // PC's 128 MiB mask (kMem2Size is not a power of 2)\n"
         "#else\n        const uint32_t exramOffset = addr & static_cast<uint32_t>(Memory::kMem2Size - 1u);\n#endif", 1),
    ])

    # Crypto++'s AutoSeededRandomPool needs an OS entropy source the 3DS lacks. It only signs the virtual NAND's
    # own files (the game never verifies those signatures), so a tick-seeded xorshift generator is enough.
    edit(inc / "wii_es_crypto.h", [
        ("inline EcSignature SignMessage(",
         "#if defined(__3DS__)\nclass Brawl3dsTickRng : public CryptoPP::RandomNumberGenerator {\npublic:\n"
         "    void GenerateBlock(CryptoPP::byte* output, size_t size) override {\n"
         "        for (size_t i = 0; i < size; ++i) {\n"
         "            m_state ^= m_state << 13; m_state ^= m_state >> 7; m_state ^= m_state << 17;\n"
         "            output[i] = static_cast<CryptoPP::byte>(m_state >> 56);\n        }\n    }\n"
         "private:\n    uint64_t m_state = static_cast<uint64_t>(\n"
         "        std::chrono::steady_clock::now().time_since_epoch().count()) | 1u;\n};\n#endif\n\n"
         "inline EcSignature SignMessage(", 1),
        ("    thread_local CryptoPP::AutoSeededRandomPool random;",
         "#if defined(__3DS__)\n    thread_local Brawl3dsTickRng random;\n#else\n"
         "    thread_local CryptoPP::AutoSeededRandomPool random;\n#endif", 1),
    ])

    # newlib's <poll.h> only has the basic flags; online play is not part of the VS port.
    edit(inc / "hle" / "network_poll_contract.h", [
        ("inline short WiiEventsToNative(uint32_t events) {",
         "#if defined(__3DS__)\n#ifndef POLLRDNORM\n#define POLLRDNORM POLLIN\n#endif\n#ifndef POLLRDBAND\n#define POLLRDBAND 0\n#endif\n"
         "#ifndef POLLWRNORM\n#define POLLWRNORM POLLOUT\n#endif\n#ifndef POLLWRBAND\n#define POLLWRBAND 0\n#endif\n#endif\n\n"
         "inline short WiiEventsToNative(uint32_t events) {", 1),
    ])

    # newlib's siginfo_t has no si_addr, and the 3DS reports CPU faults through libctru's exception handler
    # instead of signals: compile the POSIX fault path to a report-only stub there.
    edit(src / "main.cpp", [
        ("    void* faultAddress = info != nullptr ? info->si_addr : nullptr;",
         "#if defined(__3DS__)\n    (void)info;\n    void* faultAddress = nullptr;\n#else\n"
         "    void* faultAddress = info != nullptr ? info->si_addr : nullptr;\n#endif", 1),
        ("            {\"auto\", BACKEND_AUTO}, {\"d3d12\", BACKEND_D3D12}, {\"vulkan\", BACKEND_VULKAN},\n        }};\n\n#endif",
         "            {\"auto\", BACKEND_AUTO}, {\"d3d12\", BACKEND_D3D12}, {\"vulkan\", BACKEND_VULKAN},\n        }};\n"
         "#elif defined(__3DS__)\n        static constexpr std::array<GraphicsBackendEntry, 1> kGraphicsBackends{{\n"
         "            {\"auto\", BACKEND_AUTO},  // the 3DS renderer replaces Aurora's backends\n        }};\n\n#endif", 1),
        ("#else\nint main(int argc, char** argv) {\n    return RuntimeMain(argc, argv);\n}\n#endif",
         "#elif defined(__3DS__)\n// brawl3ds: port/wiicompiled_arm32/main_3ds.cpp owns main() (3DS setup) and calls RuntimeMain.\n"
         "#else\nint main(int argc, char** argv) {\n    return RuntimeMain(argc, argv);\n}\n#endif", 1),
        ("        siglongjmp(*g_sehJumpTarget, 1);",
         "#if defined(__3DS__)\n        longjmp(*g_sehJumpTarget, 1);  // MkwJmpBuf is a plain jmp_buf on the 3DS\n#else\n"
         "        siglongjmp(*g_sehJumpTarget, 1);\n#endif", 1),
        ("void InstallPosixMemoryFaultHandler() {\n    struct sigaction action {};",
         "void InstallPosixMemoryFaultHandler() {\n#if !defined(__3DS__)  // the 3DS uses libctru's exception handler\n"
         "    struct sigaction action {};", 1),
        ("    sigaction(SIGBUS, &action, nullptr);\n}",
         "    sigaction(SIGBUS, &action, nullptr);\n#endif\n}", 1),
    ])

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

    # The 3DS build keeps everything a desktop build has beside its executable (UserData, game, wii_bootstrap,
    # portable.txt) in one SD card folder.
    edit(src / "platform" / "host_platform.cpp", [
        ("#elif defined(__ANDROID__)\n    // The game is a library inside an APK",
         "#elif defined(__3DS__)\n    return std::filesystem::path(\"sdmc:/3ds/brawl3ds\");\n"
         "#elif defined(__ANDROID__)\n    // The game is a library inside an APK", 1),
    ])

    edit(inc / "system_bridge.h", [
        ("#if defined(_WIN32)\nusing MkwJmpBuf = jmp_buf;",
         "#if defined(_WIN32) || defined(__3DS__)\nusing MkwJmpBuf = jmp_buf;", 1),
    ])


if __name__ == "__main__":
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    main(Path(sys.argv[1]))
