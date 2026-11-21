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
                       ("brawl3ds_fast_tree.h", inc / "brawl3ds_fast_tree.h"),
                       ("host_context_3ds.inc", src / "host_context_3ds.inc"),
                       ("co_switch_arm32.S", src / "platform" / "3ds" / "co_switch.S"),
                       ("guest_flat_memory_3ds.cpp", src / "platform" / "3ds" / "guest_flat_memory_3ds.cpp")]:
        # Only when the content changed: rewriting an unchanged header would make every object depending on it
        # rebuild (the NEON shim is included by everything).
        if dest.exists() and dest.read_bytes() == (port / name).read_bytes():
            continue
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
        # A fatal error shows on the 3DS's screen and in sdmc:/3ds/brawl3ds/crash.txt (main_3ds.cpp), and stops there:
        # returning into the runtime's shutdown can wait forever on threads that never started properly.
        ("#else\n        // The shipped product is Windows-first. Keep non-Windows builds safe\n",
         "#elif defined(__3DS__)\n        RT_LOGF(RT_TAG_RUNTIME, \"fatal dialog: %s\\n\", message.c_str());\n"
         "        Brawl3dsFatal(message.c_str());\n"
         "#else\n        // The shipped product is Windows-first. Keep non-Windows builds safe\n", 1),
        ("void ShowRuntimeFatalPopup(std::string_view category, std::string_view details) noexcept {\n",
         "#if defined(__3DS__)\nextern \"C\" void Brawl3dsFatal(const char* what);  // port/wiicompiled_arm32/main_3ds.cpp\n#endif\n"
         "void ShowRuntimeFatalPopup(std::string_view category, std::string_view details) noexcept {\n", 1),
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

    # Gekko floating point on the ARM11. Two changes, both 3DS-only:
    # - Multiply-add: std::fma / std::fmaf are newlib's unfused vmla.f64 here (VFPv2 has no fused multiply-add),
    #   called out of line for every guest multiply-add. Brawl3dsHostFma/Fmaf (brawl3ds_neon_shim.h) compute the same
    #   thing inline, bit for bit.
    # - BRAWL3DS_FAST_FP (port/headless/CMakeLists.txt): drop the exact Gekko single-precision emulation, which on
    #   the ARM11 costs integer work on the double's bits and FPU<->core transfers around every fmuls/fmadds/ps_*
    #   (rounding operand C to 25 bits, the FPSCR.NI flush and the madd tie correction). The VFP's own flush-to-zero,
    #   set from FPSCR.NI, still flushes. Results can differ in the last bit, so the attract demo may play out
    #   slightly differently; the PC build keeps the exact path.
    edit(isa / "ppc_isa_float.h", [
        ("defined(__aarch64__)", "(defined(__aarch64__) || defined(__arm__))", None),
        ("std::fma(", "PPC_HOST_FMA(", None),
        ("std::fmaf(", "PPC_HOST_FMAF(", None),
        ("inline float PpcForceSingleValueInline(double value)\n{\n",
         f"#if {ARM32}\n"
         "#define PPC_HOST_FMA(a, b, c) Brawl3dsHostFma((a), (b), (c))\n"
         "#define PPC_HOST_FMAF(a, b, c) Brawl3dsHostFmaf((a), (b), (c))\n"
         "#else\n"
         "#define PPC_HOST_FMA(a, b, c) std::fma((a), (b), (c))\n"
         "#define PPC_HOST_FMAF(a, b, c) std::fmaf((a), (b), (c))\n"
         "#endif\n"
         f"#if {ARM32} && defined(BRAWL3DS_FAST_FP)\n"
         "#define BRAWL3DS_FAST_FP_ACTIVE 1\n"
         "#endif\n\n"
         "inline float PpcForceSingleValueInline(double value)\n{\n"
         "#if defined(BRAWL3DS_FAST_FP_ACTIVE)\n"
         "    return static_cast<float>(value);  // the VFP's FZ (set from FPSCR.NI) flushes\n"
         "#endif\n", 1),
        ("inline double PpcForce25BitInline(double value)\n{\n",
         "inline double PpcForce25BitInline(double value)\n{\n"
         "#if defined(BRAWL3DS_FAST_FP_ACTIVE)\n"
         "    return value;\n"
         "#endif\n", 1),
        ("inline double PpcAccurateSingleMaddIntermediateInline(double a, double c, double b)\n{\n",
         "inline double PpcAccurateSingleMaddIntermediateInline(double a, double c, double b)\n{\n"
         "#if defined(BRAWL3DS_FAST_FP_ACTIVE)\n"
         "    return Subtract ? a * c - b : a * c + b;\n"
         "#endif\n", 1),
    ])
    # Paired-single loads/stores (psq_l/psq_st) in the hot shards take a host pointer straight from the page-bias
    # tables, like the scalar accesses in memory_access.h below; a null pointer keeps the checked Memory::Read64/
    # Write64 path (all other shards, as before).
    edit(isa / "ppc_isa_quantized.h", [
        ("defined(__aarch64__)", "(defined(__aarch64__) || defined(__arm__))", None),
        ("MKW_PPC_FORCE_INLINE const uint8_t* PpcTryGetPsqReadableHostInline(uint32_t addr)\n{\n",
         "MKW_PPC_FORCE_INLINE const uint8_t* PpcTryGetPsqReadableHostInline(uint32_t addr)\n{\n"
         f"#if {ARM32} && defined(BRAWL3DS_INLINE_GUEST_ACCESS)\n"
         "    return MemoryInline::Brawl3dsReadableHost(addr, 8);\n"
         "#endif\n", 1),
        ("MKW_PPC_FORCE_INLINE uint8_t* PpcTryGetPsqWritableHostInline(uint32_t addr)\n{\n",
         "MKW_PPC_FORCE_INLINE uint8_t* PpcTryGetPsqWritableHostInline(uint32_t addr)\n{\n"
         f"#if {ARM32} && defined(BRAWL3DS_INLINE_GUEST_ACCESS)\n"
         "    return MemoryInline::Brawl3dsWritableHost(addr, 8);\n"
         "#endif\n", 1),
    ])

    # Per-function call counts for profile.txt, in the hot shards only (they carry the profile's functions; the
    # other shards stay as they are). Every translated function enters through MKW_GUEST_CODE_GUARD with its table
    # index and guest address; port/wiicompiled_arm32/profiler_3ds.cpp owns the array and resets it when the match
    # starts. Main-thread only: every guest thread is a fiber on the main host thread.
    edit(inc / "abi_bridge.h", [
        ("#define MKW_GUEST_CODE_GUARD(index, address)                    \\\n    do {                                                        \\\n",
         f"#if {ARM32} && defined(BRAWL3DS_INLINE_GUEST_ACCESS)\n"
         "struct Brawl3dsCallCount { uint32_t count; uint32_t address; };\n"
         "extern Brawl3dsCallCount g_brawl3dsCallCounts[];\n"
         "#define BRAWL3DS_COUNT_CALL(slot, guestAddress) \\\n"
         "    (++g_brawl3dsCallCounts[slot].count, g_brawl3dsCallCounts[slot].address = (guestAddress))\n"
         "#else\n"
         "#define BRAWL3DS_COUNT_CALL(slot, guestAddress) ((void)0)\n"
         "#endif\n"
         "#define MKW_GUEST_CODE_GUARD(index, address)                    \\\n    do {                                                        \\\n"
         "        BRAWL3DS_COUNT_CALL(index, address);                    \\\n", 1),
    ])

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

    # Guest loads and stores in translated code. The first ARM32 bring-up sent every one of them through the
    # out-of-line Memory::Read*/Write* (a call and about 20 instructions each; the first real-hardware profile found
    # the game's hottest functions made of little else). On ARM32 they now use the runtime's own 1 MiB page-bias
    # tables, the tables MemoryInline::TryReadGuestScalar/TryWriteGuestScalar read: one table load, an add and the
    # access. A zero entry (MMIO, pages with deferred reads, pages shared with executable code, unmapped memory)
    # keeps the checked Memory::* path, which applies those policies.
    # Inline at every access site the code grows from 38 to 55 MB and the regular heap gets too small to boot, so
    # only the hot shards (tools/hot_shards.py, built with BRAWL3DS_INLINE_GUEST_ACCESS) inline it; every other
    # access calls one shared copy (Brawl3dsLoadCall/Brawl3dsStoreCall: the same call size as Memory::Read32).
    edit(inc / "memory_access.h", [
        ("template <typename T>\nMKW_MEMORY_FORCE_INLINE T FlatLoad(uint32_t address) {\n",
         f"#if {ARM32}\n"
         "template <typename T>\n"
         "MKW_MEMORY_FORCE_INLINE T Brawl3dsLoadInline(uint32_t address) {\n"
         "    // A readable bias is only set for a full page whose successor is contiguous, so an access of up to\n"
         "    // 8 bytes may run into the next page.\n"
         "    const uintptr_t bias = g_fullReadablePageBias[address >> kPageShift];\n"
         "    if (bias != 0) [[likely]] {\n"
         "        T value;\n"
         "        std::memcpy(&value, reinterpret_cast<const uint8_t*>((bias - 1u) + address), sizeof(T));\n"
         "        return MaybeByteSwap(value);\n"
         "    }\n"
         "    if constexpr (sizeof(T) == 1) return static_cast<T>(Memory::Read8(address));\n"
         "    else if constexpr (sizeof(T) == 2) return static_cast<T>(Memory::Read16(address));\n"
         "    else if constexpr (sizeof(T) == 4) return static_cast<T>(Memory::Read32(address));\n"
         "    else return static_cast<T>(Memory::Read64(address));\n"
         "}\n"
         "template <typename T>\n"
         "MKW_MEMORY_FORCE_INLINE void Brawl3dsStoreInline(uint32_t address, T value) {\n"
         "    // Coarse writable pages only; a store that would cross into the next page, or any page holding\n"
         "    // executable code, MMIO or nothing, takes the checked call (it applies those policies).\n"
         "    const uintptr_t bias = g_fullWritablePageBias[address >> kPageShift];\n"
         "    if (bias != 0 && (sizeof(T) == 1 || (address & kPageMask) <= kPageSize - sizeof(T))) [[likely]] {\n"
         "        const T swapped = MaybeByteSwap(value);\n"
         "        std::memcpy(reinterpret_cast<uint8_t*>((bias - 1u) + address), &swapped, sizeof(T));\n"
         "        return;\n"
         "    }\n"
         "    if constexpr (sizeof(T) == 1) Memory::Write8(address, static_cast<uint8_t>(value));\n"
         "    else if constexpr (sizeof(T) == 2) Memory::Write16(address, static_cast<uint16_t>(value));\n"
         "    else if constexpr (sizeof(T) == 4) Memory::Write32(address, static_cast<uint32_t>(value));\n"
         "    else Memory::Write64(address, static_cast<uint64_t>(value));\n"
         "}\n"
         "template <typename T>\n"
         "__attribute__((noinline)) T Brawl3dsLoadCall(uint32_t address) { return Brawl3dsLoadInline<T>(address); }\n"
         "template <typename T>\n"
         "__attribute__((noinline)) void Brawl3dsStoreCall(uint32_t address, T value) {\n"
         "    Brawl3dsStoreInline<T>(address, value);\n"
         "}\n"
         "#endif\n\n"
         "namespace Brawl3dsEarly {\n"
         f"#if {ARM32} && defined(BRAWL3DS_INLINE_GUEST_ACCESS)\n"
         "inline constexpr bool kTake = false;\n"
         f"#elif {ARM32}\n"
         "inline constexpr bool kTake = true;\n"
         "#else\n"
         "inline constexpr bool kTake = GuestFlat::RequiresCheckedAccess();\n"
         "#endif\n"
         f"#if {ARM32}\n"
         "MKW_MEMORY_FORCE_INLINE uint8_t Read8(uint32_t a) { return Brawl3dsLoadCall<uint8_t>(a); }\n"
         "MKW_MEMORY_FORCE_INLINE uint16_t Read16(uint32_t a) { return Brawl3dsLoadCall<uint16_t>(a); }\n"
         "MKW_MEMORY_FORCE_INLINE uint32_t Read32(uint32_t a) { return Brawl3dsLoadCall<uint32_t>(a); }\n"
         "MKW_MEMORY_FORCE_INLINE void Write8(uint32_t a, uint8_t v) { Brawl3dsStoreCall<uint8_t>(a, v); }\n"
         "MKW_MEMORY_FORCE_INLINE void Write16(uint32_t a, uint16_t v) { Brawl3dsStoreCall<uint16_t>(a, v); }\n"
         "MKW_MEMORY_FORCE_INLINE void Write32(uint32_t a, uint32_t v) { Brawl3dsStoreCall<uint32_t>(a, v); }\n"
         "MKW_MEMORY_FORCE_INLINE void WriteFloat32(uint32_t a, double v) {\n"
         "    Brawl3dsStoreCall<uint32_t>(a, ConvertPpcDoubleToSingleBits(v));\n"
         "}\n"
         "MKW_MEMORY_FORCE_INLINE void WriteFloat64(uint32_t a, double v) {\n"
         "    uint64_t bits;\n"
         "    std::memcpy(&bits, &v, sizeof(bits));\n"
         "    Brawl3dsStoreCall<uint64_t>(a, bits);\n"
         "}\n"
         "#else\n"
         "MKW_MEMORY_FORCE_INLINE uint8_t Read8(uint32_t a) { return Memory::Read8(a); }\n"
         "MKW_MEMORY_FORCE_INLINE uint16_t Read16(uint32_t a) { return Memory::Read16(a); }\n"
         "MKW_MEMORY_FORCE_INLINE uint32_t Read32(uint32_t a) { return Memory::Read32(a); }\n"
         "MKW_MEMORY_FORCE_INLINE void Write8(uint32_t a, uint8_t v) { Memory::Write8(a, v); }\n"
         "MKW_MEMORY_FORCE_INLINE void Write16(uint32_t a, uint16_t v) { Memory::Write16(a, v); }\n"
         "MKW_MEMORY_FORCE_INLINE void Write32(uint32_t a, uint32_t v) { Memory::Write32(a, v); }\n"
         "MKW_MEMORY_FORCE_INLINE void WriteFloat32(uint32_t a, double v) { Memory::WriteFloat32(a, v); }\n"
         "MKW_MEMORY_FORCE_INLINE void WriteFloat64(uint32_t a, double v) { Memory::WriteFloat64(a, v); }\n"
         "#endif\n"
         "}  // namespace Brawl3dsEarly\n\n"
         "template <typename T>\nMKW_MEMORY_FORCE_INLINE T FlatLoad(uint32_t address) {\n"
         f"#if {ARM32} && defined(BRAWL3DS_INLINE_GUEST_ACCESS)\n"
         "    return Brawl3dsLoadInline<T>(address);\n"
         f"#elif {ARM32}\n"
         "    return Brawl3dsLoadCall<T>(address);\n"
         "#endif\n", 1),
        ("MKW_MEMORY_FORCE_INLINE void FlatStore(uint32_t address, T value) {\n",
         "MKW_MEMORY_FORCE_INLINE void FlatStore(uint32_t address, T value) {\n"
         f"#if {ARM32} && defined(BRAWL3DS_INLINE_GUEST_ACCESS)\n"
         "    Brawl3dsStoreInline<T>(address, value);\n"
         "    return;\n"
         f"#elif {ARM32}\n"
         "    Brawl3dsStoreCall<T>(address, value);\n"
         "    return;\n"
         "#endif\n", 1),
        # FlatRead*/FlatWrite* go to Memory::* first thing when RequiresCheckedAccess() (constexpr true on ARM32,
        # which keeps the flat-base pointer paths compiled out). On ARM32 that early exit becomes Brawl3dsEarly:
        # skipped in the hot shards (so they reach the inline FlatLoad/FlatStore, FlatWrite* sending MMIO stores to
        # Write*Slow first), the shared fast-path call everywhere else (one call per access, as before).
        ("    if (GuestFlat::RequiresCheckedAccess()) return Memory::Read",
         "    if (Brawl3dsEarly::kTake) return Brawl3dsEarly::Read", 3),
        ("    const uint64_t bits = GuestFlat::RequiresCheckedAccess()\n",
         "    const uint64_t bits = (GuestFlat::RequiresCheckedAccess() && !kBrawl3dsPageBiasAccess)\n", 1),
        ("    if (GuestFlat::RequiresCheckedAccess()) { Memory::Write",
         "    if (Brawl3dsEarly::kTake) { Brawl3dsEarly::Write", 5),
        # Range accesses (ReadResolved*/WriteResolved*: one resolve, then several accesses at offsets from it), in
        # the hot shards only; elsewhere a null resolve keeps the checked path as before.
        ("    if (GuestFlat::RequiresCheckedAccess()) {\n        // A host page can cover multiple",
         f"#if {ARM32} && defined(BRAWL3DS_INLINE_GUEST_ACCESS)\n"
         "    if (needsWrite) {\n"
         "        uint8_t* host = Brawl3dsWritableHost(guestStart, length);\n"
         "        if (!host || (needsRead && Brawl3dsReadableHost(guestStart, length) != host)) return nullptr;\n"
         "        return host;\n"
         "    }\n"
         "    return Brawl3dsReadableHost(guestStart, length);\n"
         "#endif\n"
         "    if (GuestFlat::RequiresCheckedAccess()) {\n        // A host page can cover multiple", 1),
        # The helpers, placed before their first user (ResolveRangeHost).
        ("MKW_MEMORY_FORCE_INLINE uint8_t* ResolveRangeHost(",
         f"#if {ARM32}\n"
         "inline constexpr bool kBrawl3dsPageBiasAccess = true;\n"
         "// Host pointer for [address, address + length) when it lies in one plain-RAM 1 MiB page, else null.\n"
         "MKW_MEMORY_FORCE_INLINE uint8_t* Brawl3dsReadableHost(uint32_t address, uint32_t length) {\n"
         "    const uint32_t page = address >> kPageShift;\n"
         "    if (((address + (length - 1u)) >> kPageShift) != page) [[unlikely]] return nullptr;\n"
         "    const uintptr_t bias = g_fullReadablePageBias[page];\n"
         "    return bias ? reinterpret_cast<uint8_t*>((bias - 1u) + address) : nullptr;\n"
         "}\n"
         "MKW_MEMORY_FORCE_INLINE uint8_t* Brawl3dsWritableHost(uint32_t address, uint32_t length) {\n"
         "    const uint32_t page = address >> kPageShift;\n"
         "    if (((address + (length - 1u)) >> kPageShift) != page) [[unlikely]] return nullptr;\n"
         "    const uintptr_t bias = g_fullWritablePageBias[page];\n"
         "    return bias ? reinterpret_cast<uint8_t*>((bias - 1u) + address) : nullptr;\n"
         "}\n"
         "#else\n"
         "inline constexpr bool kBrawl3dsPageBiasAccess = false;\n"
         "#endif\n\n"
         "MKW_MEMORY_FORCE_INLINE uint8_t* ResolveRangeHost(", 1),
    ])

    edit(inc / "runtime_config.h", [
        # Its own ExecutableDirectory() falls back to Linux's /proc/self/exe; the 3DS answers through
        # RuntimePlatform (host_platform.cpp), so portable.txt and UserData live in sdmc:/3ds/brawl3ds.
        ("#elif defined(__APPLE__) || defined(__ANDROID__)\n    return RuntimePlatform::ExecutableDirectory();",
         "#elif defined(__APPLE__) || defined(__ANDROID__) || defined(__3DS__)\n"
         "    return RuntimePlatform::ExecutableDirectory();", 1),
        ('#elif defined(_WIN32)\n    static constexpr std::array<std::string_view, 3> values{"auto", "d3d12", "vulkan"};\n#endif',
         '#elif defined(_WIN32)\n    static constexpr std::array<std::string_view, 3> values{"auto", "d3d12", "vulkan"};\n'
         '#elif defined(__3DS__)\n    static constexpr std::array<std::string_view, 1> values{"auto"};  // PICA200 only\n#endif', 1),
    ])

    # The 3DS build keeps everything a desktop build has beside its executable (UserData, game, wii_bootstrap,
    # portable.txt) in one SD card folder.
    edit(src / "platform" / "host_platform.cpp", [
        ("#elif defined(__ANDROID__)\n    // The game is a library inside an APK",
         "#elif defined(__3DS__)\n    return std::filesystem::path(\"/3ds/brawl3ds\");  // no sdmc: prefix: newlib std::filesystem treats it as relative\n"
         "#elif defined(__ANDROID__)\n    // The game is a library inside an APK", 1),
    ])

    # Brawl's module loader keeps a pristine copy of all 126 pre-linked module images in host memory (12 MiB) to
    # restore a module each time the game loads it. On the 3DS those bytes are read back from the section blobs on
    # the SD card when a module is linked (brawl_modules.bin for the homes, text2.bin for the boot-resident ones).
    pristine_loader = (
        "#if defined(__3DS__)\n"
        "// brawl3ds: no resident copies of the pristine images (12 MiB); read the one needed from the SD card blobs.\n"
        "// Bytes [offset, offset + size) of module `index`'s pristine image, read straight into `dest`.\n"
        "void ReadPristine3ds(size_t index, uint32_t offset, void* dest, uint32_t size) {\n"
        "    const BrawlModule& m = kModules[index];\n"
        "    const bool home = m.image >= 0x94000000u;\n"
        "    const char* blob = home ? \"sdmc:/3ds/brawl3ds/blobs/brawl_modules.bin\" : \"sdmc:/3ds/brawl3ds/blobs/text2.bin\";\n"
        "    const uint32_t base = home ? 0x94000000u : 0x80673FA0u;  // brawl_modules / text2 load addresses\n"
        "    std::FILE* file = std::fopen(blob, \"rb\");\n"
        "    if (!file || std::fseek(file, static_cast<long>(m.image - base + offset), SEEK_SET) != 0 ||\n"
        "        std::fread(dest, 1, size, file) != size) {\n"
        "        if (file) std::fclose(file);\n"
        "        throw std::runtime_error(std::string(\"cannot read the pristine image of \") + m.name + \" from \" + blob);\n"
        "    }\n"
        "    std::fclose(file);\n"
        "}\n"
        "std::vector<uint8_t> LoadPristine3ds(size_t index) {\n"
        "    std::vector<uint8_t> out(kModules[index].fixSize);\n"
        "    ReadPristine3ds(index, 0, out.data(), static_cast<uint32_t>(out.size()));\n"
        "    return out;\n"
        "}\n"
        "// The start of the image, where the header and its section table are: HomeCodeSections reads nothing else, and\n"
        "// loading a whole image (4.7 MB for sora_melee) on every link was the largest allocation of a run, the one that\n"
        "// failed first in a fragmented heap.\n"
        "std::vector<uint8_t> LoadPristineHead3ds(size_t index) {\n"
        "    std::vector<uint8_t> out(std::min<uint32_t>(kModules[index].fixSize, 64u * 1024u));\n"
        "    ReadPristine3ds(index, 0, out.data(), static_cast<uint32_t>(out.size()));\n"
        "    return out;\n"
        "}\n"
        "#define BRAWL3DS_PRISTINE(index) LoadPristine3ds(index)\n"
        "#define BRAWL3DS_PRISTINE_MISSING(index) false\n"
        "#else\n"
        "#define BRAWL3DS_PRISTINE(index) g_pristine[index]\n"
        "#define BRAWL3DS_PRISTINE_MISSING(index) g_pristine[index].empty()\n"
        "#endif\n")
    # Aurora's command processor (linked on the 3DS for the renderer, docs/RENDERER_3DS.md) keeps a 1,024-entry
    # pipeline-state cache of full configs and shader infos: 2.9 MB of static data, all of it out of the regular heap
    # on the 3DS. A Brawl frame uses far fewer distinct pipeline states; 128 entries is about 360 KB.
    edit(wiicompiled / "aurora-main" / "lib" / "gx" / "command_processor.cpp", [
        ("  constexpr size_t CacheSize = 1024;\n  struct Entry {\n",
         "#if defined(__3DS__)\n  constexpr size_t CacheSize = 128;\n#else\n  constexpr size_t CacheSize = 1024;\n#endif\n"
         "  struct Entry {\n", 1),
        # Extra Tracy zones on the per-command handlers. On the PC they cost nothing without TRACY_ENABLE; on the 3DS
        # aurora3ds/tracy/Tracy.hpp turns every zone into a per-site timer reported in headless.log, which is how the
        # front end's cost per match frame is broken down on the console.
        ("static void handle_bp(u32 value, bool bigEndian) {\n",
         "static void handle_bp(u32 value, bool bigEndian) {\n  ZoneScopedN(\"handle_bp\");\n", 1),
        ("static void handle_cp(u8 addr, u32 value, bool bigEndian) {\n",
         "static void handle_cp(u8 addr, u32 value, bool bigEndian) {\n  ZoneScopedN(\"handle_cp\");\n", 1),
        ("static void handle_xf(const u8* data, u32& pos, u32 size, bool bigEndian) {\n",
         "static void handle_xf(const u8* data, u32& pos, u32 size, bool bigEndian) {\n  ZoneScopedN(\"handle_xf\");\n", 1),
        ("static const CachedPipelineState& resolve_pipeline_state(GXPrimitive prim, GXVtxFmt fmt) {\n",
         "static const CachedPipelineState& resolve_pipeline_state(GXPrimitive prim, GXVtxFmt fmt) {\n"
         "  ZoneScopedN(\"resolve_pipeline_state\");\n", 1),
        # Brawl sends ~11,000 GX draws per match frame, ~10,700 of them with no state change since the previous one.
        # Aurora merges those into the previous WebGPU draw, but first runs the z-freeze reference, its diagnostics,
        # a vertex copy and a triangle-list index expansion for each: 13 us a draw in Azahar, ~140 ms a frame. The
        # PICA200 draws strips and fans itself and the 3DS back end reads every vertex once anyway, so on the 3DS an
        # unchanged-state draw goes straight to the back end as an append to its current draw. State changes (~555 a
        # frame) still take Aurora's full path, which builds the pipeline state and uniforms the back end draws with.
        ("  if (!g_gxState.zFreeze) update_zfreeze_reference(prim, fmt, data + pos, vtxCount, vtxSize, bigEndian);\n"
         "  // SPM: with the view widened",
         "#if defined(__3DS__)\n"
         "  if (!g_gxState.stateDirty && brawl3ds_append_draw(prim, fmt, data + pos, vtxCount, vtxSize, bigEndian)) {\n"
         "    pos += totalVtxBytes;\n"
         "    return true;\n"
         "  }\n"
         "#endif\n"
         "  if (!g_gxState.zFreeze) update_zfreeze_reference(prim, fmt, data + pos, vtxCount, vtxSize, bigEndian);\n"
         "  // SPM: with the view widened", 1),
        ("static bool handle_draw(u8 cmd, const u8* data, u32& pos, u32 size, bool bigEndian) {\n  ZoneScoped;\n",
         "#if defined(__3DS__)\n"
         "// port/wiicompiled_arm32/aurora3ds/backend_3ds.cpp: appends to the current draw; false when there is none.\n"
         "bool brawl3ds_append_draw(GXPrimitive prim, GXVtxFmt fmt, const u8* vertices, u16 vtxCount, u32 vtxSize,\n"
         "                          bool bigEndian) noexcept;\n"
         "#endif\n"
         "static bool handle_draw(u8 cmd, const u8* data, u32& pos, u32 size, bool bigEndian) {\n  ZoneScoped;\n", 1),
    ])

    edit(src / "hle" / "brawl_modules.cpp", [
        ("#include <cstring>\n#include <mutex>\n", "#include <algorithm>\n#include <cstdio>\n#include <cstring>\n"
         "#include <mutex>\n#include <stdexcept>\n#include <string>\n", 1),
        ("std::vector<uint8_t> g_pristine[sizeof(kModules) / sizeof(kModules[0])];  // each pre-linked image, from boot\n",
         "std::vector<uint8_t> g_pristine[sizeof(kModules) / sizeof(kModules[0])];  // each pre-linked image, from boot\n"
         + pristine_loader, 1),
        ("    const std::vector<uint8_t>& image = g_pristine[index];",
         "#if defined(__3DS__)\n"
         "    const std::vector<uint8_t> image = LoadPristineHead3ds(index);  // header + section table only\n"
         "#else\n"
         "    const std::vector<uint8_t>& image = g_pristine[index];\n"
         "#endif", 1),
        ("if (m == nullptr || m->resident || g_pristine[index].empty()) return;",
         "if (m == nullptr || m->resident || BRAWL3DS_PRISTINE_MISSING(index)) return;", 1),
        # The link itself: on the 3DS the pristine bytes go from the SD card blob straight into the module's home.
        ("    const auto& pristine = g_pristine[index];\n", "", 1),
        ("    std::memcpy(Memory::GetPointer(m->image + keep, m->fixSize - keep), pristine.data() + keep, m->fixSize - keep);\n",
         "#if defined(__3DS__)\n"
         "    ReadPristine3ds(index, keep, Memory::GetPointer(m->image + keep, m->fixSize - keep), m->fixSize - keep);\n"
         "#else\n"
         "    const auto& pristine = g_pristine[index];\n"
         "    std::memcpy(Memory::GetPointer(m->image + keep, m->fixSize - keep), pristine.data() + keep, m->fixSize - keep);\n"
         "#endif\n", 1),
        ("        g_pristine[i].assign(image, image + kModules[i].fixSize);",
         "#if !defined(__3DS__)\n        g_pristine[i].assign(image, image + kModules[i].fixSize);\n#else\n"
         "        (void)image;\n#endif", 1),
        ("        const auto* now = static_cast<const uint8_t*>(Memory::GetPointer(m.image, m.fixSize));\n",
         "        const auto* now = static_cast<const uint8_t*>(Memory::GetPointer(m.image, m.fixSize));\n"
         "        const auto& pristineImage = BRAWL3DS_PRISTINE(i);\n", 1),
        ("if (std::memcmp(now + offset, g_pristine[i].data() + offset, 4) == 0) continue;",
         "if (std::memcmp(now + offset, pristineImage.data() + offset, 4) == 0) continue;", 1),
    ])

    # The PC overlay is ImGui, whose frames Aurora begins and renders. With no Aurora on the 3DS nothing ever ends an
    # ImGui frame, so every overlay draw call only piles up draw commands (~256 KiB per frame, found with
    # alloc_tracker_3ds.cpp). Keep the overlay's non-drawing work and skip its drawing.
    # The PC's startup cover ("press A to start") blocks all game input until it is lifted; the 3DS never shows it,
    # and its automatic lift for scripted runs looks for input_script.txt in the current directory, which on the 3DS
    # is not the program folder. Lift it at once, or every pad reads as idle forever.
    edit(src / "settings_overlay.cpp", [
        ("    UpdateBootShaderState();\n    UpdateStartupPress();\n",
         "    UpdateBootShaderState();\n    UpdateStartupPress();\n"
         "#if defined(__3DS__)\n    NotifyStrapInputAccepted();  // no startup cover on the 3DS\n"
         "    UpdateGameSpeed();\n    ApplyInputBlockState();\n"
         "    return;  // no ImGui frame is ever rendered on the 3DS: drawing would only pile up draw lists\n#endif\n", 1),
    ])

    # Every guest thread runs on a host fiber stack, 1 MiB each upstream. Translated functions use small host frames,
    # so the 3DS gives each 256 KiB of its scarce regular heap.
    edit(src / "fiber_manager.cpp", [
        ("    constexpr size_t kHostStackSize = 1024 * 1024;",
         "#if defined(__3DS__)\n    constexpr size_t kHostStackSize = 256 * 1024;  // regular heap is scarce on the 3DS\n"
         "#else\n    constexpr size_t kHostStackSize = 1024 * 1024;\n#endif", 1),
    ])

    # Finding the extracted disc in a "game" folder beside the program is Windows-only upstream; the 3DS uses the
    # same rule (sdmc:/3ds/brawl3ds/game), so no Config.toml editing is needed on the console.
    edit(src / "game_folder.cpp", [
        ("bool EnsureConfigured() {\n#if defined(_WIN32)",
         "#if defined(__3DS__)\nfs::path ProgramDirectory() { return fs::path(\"/3ds/brawl3ds\"); }\n#endif\n\n"
         "bool EnsureConfigured() {\n#if defined(_WIN32) || defined(__3DS__)", 1),
    ])

    # Start-up walks the disc folder twice (the save key and the DVD index, 5,314 files each), getting every file's
    # size with stat and its relative path with fs::relative (a stat per path component). On a real New 3DS one
    # stat is ~50 ms (newlib opens the file, asks its size, closes it), so step 4 never finished in an hour. On the
    # 3DS both walks use Brawl3dsListTree (port/wiicompiled_arm32/fast_tree_3ds.cpp) instead: FSDIR_Read returns
    # names and sizes 64 at a time, and the tree is read from the card once and kept.
    edit(inc / "nand_path.h", [
        ("#include <vector>\n\nnamespace RuntimeNandPath {",
         "#include <vector>\n\n#if defined(__3DS__)\n#include \"brawl3ds_fast_tree.h\"\n#endif\n\nnamespace RuntimeNandPath {", 1),
        ("    for (std::filesystem::recursive_directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec)) {\n",
         "#if defined(__3DS__)\n"
         "    if (const auto* tree = Brawl3dsListTree(root.generic_string())) {  // no stat per file on the SD card\n"
         "        for (const auto& file : *tree) files.emplace_back(file.relative, file.size);\n"
         "    }\n"
         "#else\n"
         "    for (std::filesystem::recursive_directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec)) {\n", 1),
        ("        }\n    }\n    if (files.empty()) {\n",
         "        }\n    }\n#endif\n    if (files.empty()) {\n", 1),
    ])
    edit(src / "hle" / "storage" / "dvd.cpp", [
        ("#include \"runtime_product.h\"\n",
         "#include \"runtime_product.h\"\n#if defined(__3DS__)\n#include \"brawl3ds_fast_tree.h\"\n#endif\n", 1),
        ("    const auto registerEntry = [&](const fs::directory_entry& entry) {\n",
         "#if defined(__3DS__)\n"
         "    // One FSDIR_Read per folder instead of a stat per file (~50 ms each on the console).\n"
         "    if (recursive) {\n"
         "        if (const auto* tree = Brawl3dsListTree(root.generic_string())) {\n"
         "            std::string prefix = virtualPrefix.empty() ? \"/\" : virtualPrefix;\n"
         "            if (prefix.back() != '/' && prefix.back() != '\\\\') {\n"
         "                prefix += \"/\";\n"
         "            }\n"
         "            for (const auto& file : *tree) {\n"
         "                std::string dvdPath = prefix + file.relative;\n"
         "                if (!addNewFiles && !DvdEntryExists(dvdPath)) {\n"
         "                    continue;\n"
         "                }\n"
         "                RegisterFileEntry(std::move(dvdPath), root / file.relative, static_cast<uint32_t>(file.size));\n"
         "            }\n"
         "            return;\n"
         "        }\n"
         "    }\n"
         "#endif\n"
         "    const auto registerEntry = [&](const fs::directory_entry& entry) {\n", 1),
    ])

    # Opening an existing save file for writing first copies it to a shadow file with std::filesystem::copy_file.
    # On the 3DS that copy crawls: the first real-hardware run spent 235 s in one scBoot frame copying the 14 MB
    # collect.vff. On the 3DS NandCopyFile copies in 1 MiB unbuffered chunks instead (same copy_options rules).
    edit(src / "hle" / "storage" / "nand_internal.h", [
        ("#include <unistd.h>\n#endif\n",
         "#include <unistd.h>\n#endif\n\n"
         "inline bool NandCopyFile(const std::filesystem::path& from, const std::filesystem::path& to,\n"
         "                         std::filesystem::copy_options options, std::error_code& ec) {\n"
         "#if defined(__3DS__)\n"
         "    using std::filesystem::copy_options;\n"
         "    ec.clear();\n"
         "    if (std::filesystem::exists(to, ec)) {\n"
         "        if ((options & copy_options::skip_existing) != copy_options::none) return false;\n"
         "        if ((options & copy_options::overwrite_existing) == copy_options::none) {\n"
         "            ec = std::make_error_code(std::errc::file_exists);\n"
         "            return false;\n"
         "        }\n"
         "    }\n"
         "    ec.clear();\n"
         "    FILE* in = std::fopen(from.string().c_str(), \"rb\");\n"
         "    if (!in) {\n"
         "        ec = std::error_code(errno, std::generic_category());\n"
         "        return false;\n"
         "    }\n"
         "    FILE* out = std::fopen(to.string().c_str(), \"wb\");\n"
         "    if (!out) {\n"
         "        ec = std::error_code(errno, std::generic_category());\n"
         "        std::fclose(in);\n"
         "        return false;\n"
         "    }\n"
         "    std::setvbuf(in, nullptr, _IONBF, 0);\n"
         "    std::setvbuf(out, nullptr, _IONBF, 0);\n"
         "    constexpr size_t kChunk = 1u << 20;\n"
         "    std::vector<char> buffer(kChunk);\n"
         "    bool ok = true;\n"
         "    for (;;) {\n"
         "        const size_t got = std::fread(buffer.data(), 1, kChunk, in);\n"
         "        if (got > 0 && std::fwrite(buffer.data(), 1, got, out) != got) { ok = false; break; }\n"
         "        if (got < kChunk) { ok = !std::ferror(in); break; }\n"
         "    }\n"
         "    std::fclose(in);\n"
         "    if (std::fclose(out) != 0) ok = false;\n"
         "    if (!ok) {\n"
         "        ec = std::make_error_code(std::errc::io_error);\n"
         "        std::remove(to.string().c_str());\n"
         "    }\n"
         "    return ok;\n"
         "#else\n"
         "    return std::filesystem::copy_file(from, to, options, ec);\n"
         "#endif\n"
         "}\n"
         "inline bool NandCopyFile(const std::filesystem::path& from, const std::filesystem::path& to, std::error_code& ec) {\n"
         "    return NandCopyFile(from, to, std::filesystem::copy_options::none, ec);\n"
         "}\n", 1),
    ])
    # Plain NANDOpen for writing (mode 2/3) also seeds a shadow copy of the whole file first, a PC-runtime guard
    # against a crash between NANDWrite and NANDClose (the console's plain NANDOpen writes in place; NANDSafeOpen is
    # the game's own crash-safe path and keeps its copy). Brawl opens the 14 MB collect.vff that way at boot and
    # closes it without writing a byte, and on a real New 3DS the copy took from 9 s to over 3 minutes from one
    # run to the next (SD write speed for a new 14 MB file). On the 3DS plain opens write in place.
    edit(src / "hle" / "storage" / "nand_api.cpp", [
        ("std::filesystem::copy_file(", "NandCopyFile(", 2),
        ("    if ((mode == 2 || mode == 3) && PathExists(hostPath) && !IsDirectory(hostPath)) {\n",
         "#if defined(__3DS__)\n"
         "    if (false) {  // brawl3ds: no shadow copy for plain write opens (see tools/patch_wiicompiled_arm32.py)\n"
         "#else\n"
         "    if ((mode == 2 || mode == 3) && PathExists(hostPath) && !IsDirectory(hostPath)) {\n"
         "#endif\n", 1),
    ])
    for name, count in (("nand_async.cpp", 1), ("nand_fs.cpp", 1)):
        edit(src / "hle" / "storage" / name, [("std::filesystem::copy_file(", "NandCopyFile(", count)])

    edit(inc / "system_bridge.h", [
        ("#if defined(_WIN32)\nusing MkwJmpBuf = jmp_buf;",
         "#if defined(_WIN32) || defined(__3DS__)\nusing MkwJmpBuf = jmp_buf;", 1),
    ])


if __name__ == "__main__":
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    main(Path(sys.argv[1]))
