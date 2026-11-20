// brawl3ds headless timing build: the 3DS entry point around WiiCompiled's RuntimeMain.
//
// What it does:
//  * Memory: asks libctru for the largest regular heap (96 MiB, the kernel's heap window) and leaves everything else
//    of the 178 MB system mode to the linear heap (libctru does that when only __ctru_heap_size is set).
//  * Files: the runtime treats sdmc:/3ds/brawl3ds as its executable directory (host_platform.cpp patch), so
//    UserData/, game/ (the extracted disc), wii_bootstrap/ and blobs/ all live there.
//  * Timing: Brawl presents once per game frame through aurora_end_frame (aurora_headless_3ds.cpp). Each frame's wall
//    time is measured here and summarised once a second to sdmc:/3ds/brawl3ds/headless.log, with the current scene.
//    Deterministic mode (guest clocks on a virtual timeline) makes the attract demo reach the same VS match
//    (Bowser, Olimar, Zelda, Mario on Smashville) however slowly frames run.
//  * Stops by itself after kMatchFramesToMeasure frames of scMelee, or when START is pressed.
//  * Failures (a guest call into code the build left out, a fatal runtime error, a CPU exception) are written to
//    sdmc:/3ds/brawl3ds/crash.txt and shown on screen.
#include <3ds.h>

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <malloc.h>
#include <string>
#include <sys/stat.h>

#include "isa/ppc_isa_context.h"
#include "save_states.h"

#include <SDL3/SDL_main.h>  // SDL_SetMainReady: the build defines SDL_MAIN_HANDLED, this file owns main()

int RuntimeMain(int argc, char** argv);
extern "C" void Brawl3dsReserveGuestMemory();
extern "C" void Brawl3dsAllocTrackStart();                            // alloc_tracker_3ds.cpp
extern "C" void Brawl3dsAllocTrackDump(const char* path, unsigned frame);

// Heap layout. libctru's default __system_allocateHeaps (weak) caps the regular heap at 96 MiB and, depending on
// which sizes are set, either leaves memory unallocated or gives all the rest to the linear heap. This build wants
// a fixed linear heap (guest regions that fit there, audio and later GPU buffers) and every other byte of the
// 178 MB system mode as regular heap (the runtime's own allocations and the larger guest regions), whatever size the
// code turns out to be. Same steps as libctru's version, different split.
extern "C" {
u32 __ctru_heap_size = 0;
u32 __ctru_linear_heap_size = 0;
extern u32 __ctru_heap;
extern u32 __ctru_linear_heap;
extern char* fake_heap_start;
extern char* fake_heap_end;

void __system_allocateHeaps(void)
{
    // Room for MEM1 (24 MiB) and the locked cache with GuestFlat's 4 MiB reserve for audio/GPU buffers, so the
    // regular heap only carries MEM2 and the module homes besides the runtime's own allocations.
    constexpr u32 kLinearHeap = 29u * 1024u * 1024u;
    constexpr u32 kRegularHeapMax = 96u * 1024u * 1024u;  // the kernel's heap window at 0x08000000

    Handle limits = 0;
    if (R_FAILED(svcGetResourceLimit(&limits, CUR_PROCESS_HANDLE))) svcBreak(USERBREAK_PANIC);
    ResourceLimitType commit = RESLIMIT_COMMIT;
    s64 limit = 0, used = 0;
    svcGetResourceLimitLimitValues(&limit, limits, &commit, 1);
    svcGetResourceLimitCurrentValues(&used, limits, &commit, 1);
    svcCloseHandle(limits);
    const u32 available = static_cast<u32>(limit - used) & ~0xFFFu;

    u32 linear = kLinearHeap < available / 2 ? kLinearHeap : (available / 2) & ~0xFFFu;
    u32 heap = available - linear;
    if (heap > kRegularHeapMax) {
        heap = kRegularHeapMax;
        linear = available - heap;
    }

    u32 mapped = 0;
    __ctru_heap = OS_HEAP_AREA_BEGIN;
    if (R_FAILED(svcControlMemory(&mapped, __ctru_heap, 0, heap, MEMOP_ALLOC,
                                  static_cast<MemPerm>(MEMPERM_READ | MEMPERM_WRITE)))) {
        svcBreak(USERBREAK_PANIC);
    }
    if (R_FAILED(svcControlMemory(&__ctru_linear_heap, 0, 0, linear, MEMOP_ALLOC_LINEAR,
                                  static_cast<MemPerm>(MEMPERM_READ | MEMPERM_WRITE)))) {
        svcBreak(USERBREAK_PANIC);
    }
    mappableInit(OS_MAP_AREA_BEGIN, OS_MAP_AREA_END);
    __ctru_heap_size = heap;
    __ctru_linear_heap_size = linear;
    fake_heap_start = reinterpret_cast<char*>(__ctru_heap);
    fake_heap_end = fake_heap_start + heap;
}
}

namespace {

constexpr const char* kRoot = "sdmc:/3ds/brawl3ds";
constexpr uint32_t kMatchFramesToMeasure = 1800;  // 30 seconds of the demo match at 60 frames per second

std::FILE* g_log = nullptr;
std::atomic<bool> g_exitRequested{false};
bool g_consoleActive = false;

uint64_t g_frameStart = 0, g_lastEnd = 0;
uint32_t g_frames = 0, g_matchFrames = 0;
// Per-second window and match totals (ticks).
uint64_t g_windowSum = 0, g_windowMin = UINT64_MAX, g_windowMax = 0, g_windowWork = 0;
uint32_t g_windowFrames = 0;
uint64_t g_matchSum = 0, g_matchWork = 0, g_matchMax = 0;
uint32_t g_matchOver16 = 0, g_matchOver33 = 0;
std::string g_scene;

double TicksToMs(uint64_t ticks) { return static_cast<double>(ticks) * 1000.0 / SYSCLOCK_ARM11; }

void Log(const char* fmt, ...)
{
    if (!g_log) return;
    va_list args;
    va_start(args, fmt);
    std::vfprintf(g_log, fmt, args);
    va_end(args);
    std::fflush(g_log);
}

// Bottom screen, no text renderer needed: a bar that grows with frames (white while booting, green in the match).
void DrawProgress()
{
    u16 width = 0, height = 0;  // the framebuffer is rotated: "width" is the 240 px axis
    auto* fb = gfxGetFramebuffer(GFX_BOTTOM, GFX_LEFT, &width, &height);
    if (!fb) return;
    const bool inMatch = g_scene == "scMelee";
    const uint32_t progress = inMatch ? (g_matchFrames * 320u) / kMatchFramesToMeasure : (g_frames / 30u) % 320u;
    for (uint32_t x = 0; x < 320; ++x) {
        const bool lit = x <= progress;
        for (uint32_t y = 100; y < 140; ++y) {
            u8* px = fb + (x * 240 + (239 - y)) * 3;
            px[0] = lit ? (inMatch ? 0x40 : 0xC0) : 0x20;  // B
            px[1] = lit ? (inMatch ? 0xD0 : 0xC0) : 0x20;  // G
            px[2] = lit ? (inMatch ? 0x40 : 0xC0) : 0x20;  // R
        }
    }
    gfxFlushBuffers();
    gfxSwapBuffers();
}

[[noreturn]] void StopWithMessage(const char* title, const char* detail)
{
    if (std::FILE* crash = std::fopen("sdmc:/3ds/brawl3ds/crash.txt", "w")) {
        const struct mallinfo heap = mallinfo();
        std::fprintf(crash, "%s\n%s\nframes=%u matchFrames=%u scene=%s\n"
                     "heap: in use %d KiB, free inside the heap %d KiB (largest gaps fragment it), linear free %lu KiB\n",
                     title, detail, g_frames, g_matchFrames, g_scene.c_str(), heap.uordblks >> 10, heap.fordblks >> 10,
                     static_cast<unsigned long>(linearSpaceFree() >> 10));
        std::fclose(crash);
    }
    Brawl3dsAllocTrackDump("sdmc:/3ds/brawl3ds/leaks.txt", g_frames);
    Log("STOP: %s: %s\n", title, detail);
    if (g_log) std::fclose(g_log);
    g_log = nullptr;
    if (!g_consoleActive) {
        consoleInit(GFX_TOP, nullptr);
        g_consoleActive = true;
    }
    std::printf("\n\x1b[31m%s\x1b[0m\n%s\n\nSaved to sdmc:/3ds/brawl3ds/crash.txt\nPress START to exit.\n", title,
                detail);
    while (aptMainLoop()) {
        hidScanInput();
        if (hidKeysDown() & KEY_START) break;
        gfxFlushBuffers();
        gfxSwapBuffers();
        gspWaitForVBlank();
    }
    gfxExit();
    std::exit(1);
}

// CPU exceptions (data abort, undefined instruction, ...): libctru calls this on its own stack; it records the
// registers and redirects the faulting thread into CrashReport, which may use the file system.
alignas(8) uint8_t g_exceptionStack[16 * 1024];
ERRF_ExceptionInfo g_exceptionInfo;
CpuRegisters g_exceptionRegs;

[[noreturn]] void CrashReport()
{
    char detail[256];
    std::snprintf(detail, sizeof(detail),
                  "exception type %d at pc=%08lX lr=%08lX sp=%08lX\nfault address %08lX (fsr %08lX)",
                  static_cast<int>(g_exceptionInfo.type), static_cast<unsigned long>(g_exceptionRegs.pc),
                  static_cast<unsigned long>(g_exceptionRegs.lr), static_cast<unsigned long>(g_exceptionRegs.sp),
                  static_cast<unsigned long>(g_exceptionInfo.far), static_cast<unsigned long>(g_exceptionInfo.fsr));
    StopWithMessage("CPU exception", detail);
}

void OnException(ERRF_ExceptionInfo* info, CpuRegisters* regs)
{
    g_exceptionInfo = *info;
    g_exceptionRegs = *regs;
    regs->pc = reinterpret_cast<u32>(&CrashReport);
    regs->sp = reinterpret_cast<u32>(g_exceptionStack + sizeof(g_exceptionStack) - 4096);  // leave room
    regs->cpsr &= ~(1u << 5);  // CrashReport is ARM code
}

void WriteWindow()
{
    if (g_windowFrames == 0) return;
    const double avg = TicksToMs(g_windowSum) / g_windowFrames;
    Log("frame %6u  scene %-16s  avg %6.2f ms (game %6.2f)  min %6.2f  max %6.2f  ~%4.1f fps  malloc %d KiB\n",
        g_frames, g_scene.c_str(), avg, TicksToMs(g_windowWork) / g_windowFrames, TicksToMs(g_windowMin),
        TicksToMs(g_windowMax), avg > 0 ? 1000.0 / avg : 0.0, mallinfo().uordblks >> 10);
    g_windowSum = g_windowWork = g_windowMax = 0;
    g_windowMin = UINT64_MAX;
    g_windowFrames = 0;
}

void WriteMatchSummary()
{
    if (g_matchFrames == 0) return;
    const double avg = TicksToMs(g_matchSum) / g_matchFrames;
    Log("\n== VS demo match: %u frames measured ==\n"
        "average frame  %.2f ms (%.1f fps)\n"
        "average game   %.2f ms (time between aurora_begin_frame and aurora_end_frame)\n"
        "worst frame    %.2f ms\n"
        "frames > 16.7 ms (miss 60 fps): %u (%.1f%%)\n"
        "frames > 33.3 ms (miss 30 fps): %u (%.1f%%)\n",
        g_matchFrames, avg, avg > 0 ? 1000.0 / avg : 0.0, TicksToMs(g_matchWork) / g_matchFrames,
        TicksToMs(g_matchMax), g_matchOver16, 100.0 * g_matchOver16 / g_matchFrames, g_matchOver33,
        100.0 * g_matchOver33 / g_matchFrames);
}

} // namespace

extern "C" void Brawl3dsFatal(const char* what)
{
    StopWithMessage("Fatal error", what);
}

// Every translated function the build left out (tools/select_translated_code.py) calls this.
extern "C" void Brawl3ds_NotLinked(uint32_t guestAddress, CpuContext* ctx)
{
    char detail[200];
    std::snprintf(detail, sizeof(detail),
                  "guest function %08lX is not in this build\n(called from guest lr=%08lX; add its module to --keep)",
                  static_cast<unsigned long>(guestAddress), static_cast<unsigned long>(ctx ? ctx->lr : 0));
    StopWithMessage("Code not linked", detail);
}

extern "C" bool Brawl3dsExitRequested()
{
    return g_exitRequested.load(std::memory_order_relaxed);
}

extern "C" void Brawl3dsFrameBegin()
{
    g_frameStart = svcGetSystemTick();
}

extern "C" void Brawl3dsFrameEnd()
{
    const uint64_t now = svcGetSystemTick();
    const uint64_t work = g_frameStart ? now - g_frameStart : 0;
    const uint64_t total = g_lastEnd ? now - g_lastEnd : work;
    g_lastEnd = now;
    ++g_frames;
    // Track allocations from frame 10 (past boot) and report what is still held at frame 60: per-frame leaks.
    if (g_frames == 10) Brawl3dsAllocTrackStart();
    if (g_frames == 60) Brawl3dsAllocTrackDump("sdmc:/3ds/brawl3ds/leaks.txt", g_frames);

    const std::string scene = SaveStates::SceneName();
    if (scene != g_scene) {
        WriteWindow();
        Log("-- scene %s -> %s at frame %u\n", g_scene.empty() ? "(none)" : g_scene.c_str(), scene.c_str(), g_frames);
        g_scene = scene;
    }

    g_windowSum += total;
    g_windowWork += work;
    g_windowMin = total < g_windowMin ? total : g_windowMin;
    g_windowMax = total > g_windowMax ? total : g_windowMax;
    if (++g_windowFrames >= (g_scene == "scMelee" ? 60u : 10u)) WriteWindow();  // finer before the match: memory growth

    if (g_scene == "scMelee") {
        ++g_matchFrames;
        g_matchSum += total;
        g_matchWork += work;
        g_matchMax = total > g_matchMax ? total : g_matchMax;
        const double ms = TicksToMs(total);
        g_matchOver16 += ms > 16.7 ? 1u : 0u;
        g_matchOver33 += ms > 33.3 ? 1u : 0u;
        if (g_matchFrames == kMatchFramesToMeasure) {
            WriteWindow();
            WriteMatchSummary();
            g_exitRequested = true;
        }
    }

    // Keep the console responsive (HOME menu, sleep, power button) and let START end the run.
    if ((g_frames & 7u) == 0) {
        if (!aptMainLoop()) g_exitRequested = true;
        hidScanInput();
        if (hidKeysDown() & KEY_START) g_exitRequested = true;
        DrawProgress();
    }
}

int main(int, char**)
{
    gfxInitDefault();
    consoleInit(GFX_TOP, nullptr);
    g_consoleActive = true;
    osSetSpeedupEnable(true);
    threadOnException(OnException, g_exceptionStack + sizeof(g_exceptionStack), WRITE_DATA_TO_HANDLER_STACK);

    mkdir("sdmc:/3ds", 0777);
    mkdir(kRoot, 0777);
    g_log = std::fopen("sdmc:/3ds/brawl3ds/headless.log", "w");

    bool isNew = false;
    APT_CheckNew3DS(&isNew);
    std::printf("brawl3ds headless timing build\n\n");
    Log("brawl3ds headless timing build\nconsole: %s 3DS, APPLICATION region %lu MiB, free at start %lu MiB\n",
        isNew ? "New" : "Old", static_cast<unsigned long>(osGetMemRegionSize(MEMREGION_APPLICATION) >> 20),
        static_cast<unsigned long>(osGetMemRegionFree(MEMREGION_APPLICATION) >> 20));
    Log("heaps: regular %lu MiB, linear %lu MiB (linear free %lu KiB)\n",
        static_cast<unsigned long>(__ctru_heap_size >> 20), static_cast<unsigned long>(__ctru_linear_heap_size >> 20),
        static_cast<unsigned long>(linearSpaceFree() >> 10));
    std::printf("Booting Brawl. This screen stays still while it\nruns; the bottom screen shows progress:\n"
                "  white bar = booting / title / demo setup\n  green bar = measuring the VS demo match\n\n"
                "It stops by itself after 30 s of the match\n(or press START). Results:\n"
                "  sdmc:/3ds/brawl3ds/headless.log\n");

    struct stat info {};
    if (stat("sdmc:/3ds/brawl3ds/game/sys/boot.bin", &info) != 0) {
        StopWithMessage("Game files missing", "copy the extracted disc to sdmc:/3ds/brawl3ds/game/ (sys/ and files/)");
    }

    // The runtime indexes the whole disc folder at boot (5314 files for Brawl). Time the same kind of walk here so
    // the log shows how long the SD card takes, and whether directory iteration works at all.
    {
        std::printf("Indexing the game folder...\n");
        const uint64_t start = svcGetSystemTick();
        uint32_t files = 0, dirs = 0;
        uint64_t bytes = 0;
        std::error_code ec;
        for (std::filesystem::recursive_directory_iterator it("/3ds/brawl3ds/game/files", ec), end;
             !ec && it != end; it.increment(ec)) {
            if (it->is_directory(ec)) {
                ++dirs;
            } else {
                ++files;
                bytes += it->file_size(ec);
            }
            if ((files & 511u) == 0 && files) Log("  ...%u files after %.0f ms\n", files, TicksToMs(svcGetSystemTick() - start));
        }
        Log("game folder walk: %u files, %u folders, %llu MiB in %.0f ms%s%s\n", files, dirs,
            static_cast<unsigned long long>(bytes >> 20), TicksToMs(svcGetSystemTick() - start),
            ec ? ", error: " : "", ec ? ec.message().c_str() : "");
    }

    // Deterministic mode: guest clocks follow frames, so the attract demo is the same match on every run.
    setenv("NSMBW_DETERMINISTIC", "1", 1);

    // From here on the runtime's own output goes to a file; the screen keeps the instructions above.
    std::freopen("sdmc:/3ds/brawl3ds/stdout.txt", "w", stdout);
    std::freopen("sdmc:/3ds/brawl3ds/stderr.txt", "w", stderr);
    g_consoleActive = false;

    // Guest RAM first, before the runtime's own allocations fragment the regular heap.
    Log("before reserving guest RAM: malloc in use %d KiB, linear free %lu KiB\n", mallinfo().uordblks >> 10,
        static_cast<unsigned long>(linearSpaceFree() >> 10));
    Brawl3dsReserveGuestMemory();
    Log("after reserving: malloc in use %d KiB, linear free %lu KiB\n", mallinfo().uordblks >> 10,
        static_cast<unsigned long>(linearSpaceFree() >> 10));

    SDL_SetMainReady();
    char program[] = "sdmc:/3ds/brawl3ds/brawl3ds.3dsx";
    char* argv[] = {program, nullptr};
    const int result = RuntimeMain(1, argv);

    WriteWindow();
    WriteMatchSummary();
    Log("RuntimeMain returned %d after %u frames\n", result, g_frames);
    if (g_log) std::fclose(g_log);
    gfxExit();
    return result;
}
