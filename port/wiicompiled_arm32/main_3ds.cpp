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
//  * Stops by itself when the demo match ends (~1,141 frames of scMelee, capped at kMatchFramesToMeasure), or on START.
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
constexpr uint32_t kMatchFramesToMeasure = 1800;  // cap; the attract demo match itself ends after ~1,141 frames

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

// ---- Steps of a run, shown on the bottom screen and logged ------------------------------------------------------
// The waits in steps 5 and 7 are Brawl's own idle timers, counted in frames, so at a lower frame rate they last
// longer in real time. input_script.txt (written by package_headless.py) presses A once on the title screen: that
// skips the opening movie (decoded in software, ~1 s per video frame in Azahar) and the demo match starts after
// ~1,186 title frames instead of ~7,855, with the same fighters. The safety screen is left to time out: skipping it
// too changes which demo match plays, and the build only holds the code the PC coverage run of this match used.
struct Step {
    const char* name;
    const char* detail;
    uint32_t pcFrames;  // frames the PC build spends in this step (0: not frame-based)
};
constexpr Step kSteps[] = {
    {"Checking game files", "sdmc:/3ds/brawl3ds/game and the save", 0},
    {"Indexing the game folder", "5314 files on the SD card", 0},
    {"Reserving Wii memory", "MEM1 + MEM2 + module homes", 0},
    {"Starting the runtime", "game code, modules, virtual NAND", 0},
    {"Safety screen (scStrap)", "Brawl's own ~24 s timeout", 1435},
    {"Loading boot data + save", "scBoot: menus, fonts, save files", 44},
    {"Title screen idle (scTitle)", "A skips the movie; demo starts after", 1186},
    {"Measuring the VS demo match", "scMelee: the whole ~19 s demo match", 1141},
    {"Writing results", "headless.log", 0},
};
constexpr int kStepCount = sizeof(kSteps) / sizeof(kSteps[0]);
constexpr int kStepStrap = 4, kStepBoot = 5, kStepTitle = 6, kStepMatch = 7, kStepResults = 8;  // indices

int g_step = 0;
uint64_t g_runStart = 0, g_stepStart = 0;
uint32_t g_stepFrameStart = 0;
uint64_t g_stepTicks[kStepCount] = {};
uint32_t g_stepFrames[kStepCount] = {};
double g_recentFrameMs = 0;  // smoothed frame time, for the time estimates

// Bottom screen text: libctru's 8x8 console font drawn straight into the framebuffer (40 x 30 characters).
extern "C" const u8 default_font_bin[];

void PutText(u8* fb, int col, int row, const char* text, u32 rgb)
{
    for (; *text && col < 40; ++text, ++col) {
        const u8* glyph = default_font_bin + static_cast<u8>(*text) * 8;
        for (int gy = 0; gy < 8; ++gy) {
            for (int gx = 0; gx < 8; ++gx) {
                if (!(glyph[gy] & (0x80 >> gx))) continue;
                const int x = col * 8 + gx, y = row * 8 + gy;  // the framebuffer is rotated: columns of 240 px
                u8* px = fb + (x * 240 + (239 - y)) * 3;
                px[0] = rgb & 0xFF;
                px[1] = (rgb >> 8) & 0xFF;
                px[2] = (rgb >> 16) & 0xFF;
            }
        }
    }
}

void PrintAt(u8* fb, int col, int row, u32 rgb, const char* fmt, ...)
{
    char text[48];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(text, sizeof(text), fmt, args);
    va_end(args);
    PutText(fb, col, row, text, rgb);
}

void Bar(u8* fb, int row, double fraction, u32 rgb)
{
    fraction = fraction < 0 ? 0 : (fraction > 1 ? 1 : fraction);
    const int lit = static_cast<int>(fraction * 304);
    for (int x = 8; x < 312; ++x) {
        const u32 c = x - 8 < lit ? rgb : 0x303030;
        for (int y = row * 8 + 1; y < row * 8 + 7; ++y) {
            u8* px = fb + (x * 240 + (239 - y)) * 3;
            px[0] = c & 0xFF;
            px[1] = (c >> 8) & 0xFF;
            px[2] = (c >> 16) & 0xFF;
        }
    }
}

void FormatDuration(char* out, size_t size, double seconds)
{
    if (seconds < 0) seconds = 0;
    const unsigned s = static_cast<unsigned>(seconds);
    if (s >= 3600) std::snprintf(out, size, "%uh%02um%02us", s / 3600, s / 60 % 60, s % 60);
    else std::snprintf(out, size, "%um%02us", s / 60, s % 60);
}

double SecondsSince(uint64_t tick) { return TicksToMs(svcGetSystemTick() - tick) / 1000.0; }

// Frames still to run, from the PC timeline: the rest of this step plus every later frame-based step.
uint32_t RemainingFrames()
{
    uint32_t remaining = 0;
    for (int i = g_step; i < kStepCount; ++i) {
        const uint32_t done = i == g_step ? g_frames - g_stepFrameStart : 0;
        remaining += kSteps[i].pcFrames > done ? kSteps[i].pcFrames - done : 0;
    }
    return remaining;
}

void DrawStatus(const char* warning = nullptr)
{
    u8* fb = gfxGetFramebuffer(GFX_BOTTOM, GFX_LEFT, nullptr, nullptr);
    if (!fb) return;
    std::memset(fb, 0, 320 * 240 * 3);
    constexpr u32 kWhite = 0xFFFFFF, kGrey = 0xA0A0A0, kGreen = 0x40D040, kYellow = 0xFFD040, kRed = 0xFF5050;
    char a[24], b[24];

    PutText(fb, 1, 1, "brawl3ds headless test (diagnostic)", kGrey);
    PrintAt(fb, 1, 3, kWhite, "Step %d of %d:", g_step + 1, kStepCount);
    PutText(fb, 1, 4, kSteps[g_step].name, g_step == kStepMatch ? kGreen : kYellow);
    PutText(fb, 1, 5, kSteps[g_step].detail, kGrey);

    const uint32_t stepFrames = g_frames - g_stepFrameStart;
    FormatDuration(a, sizeof(a), SecondsSince(g_stepStart));
    if (kSteps[g_step].pcFrames) {
        PrintAt(fb, 1, 7, kWhite, "frame %u of ~%u in this step", stepFrames, kSteps[g_step].pcFrames);
        Bar(fb, 8, static_cast<double>(stepFrames) / kSteps[g_step].pcFrames, g_step == kStepMatch ? kGreen : kWhite);
    } else {
        PutText(fb, 1, 7, "(not frame based)", kGrey);
    }
    PrintAt(fb, 1, 9, kWhite, "time in this step   %s", a);

    // Overall: one segment per step.
    PutText(fb, 1, 11, "All steps:", kWhite);
    for (int i = 0; i < kStepCount; ++i) {
        const u32 c = i < g_step ? kGreen : (i == g_step ? kYellow : 0x404040);
        for (int x = 8 + i * 34; x < 8 + i * 34 + 30; ++x) {
            for (int y = 12 * 8 + 1; y < 12 * 8 + 7; ++y) {
                u8* px = fb + (x * 240 + (239 - y)) * 3;
                px[0] = c & 0xFF;
                px[1] = (c >> 8) & 0xFF;
                px[2] = (c >> 16) & 0xFF;
            }
        }
    }
    FormatDuration(a, sizeof(a), g_runStart ? SecondsSince(g_runStart) : 0);
    PrintAt(fb, 1, 14, kWhite, "total time          %s", a);
    if (g_recentFrameMs > 0) {
        const uint32_t remaining = RemainingFrames();
        FormatDuration(b, sizeof(b), remaining * g_recentFrameMs / 1000.0);
        PrintAt(fb, 1, 15, kWhite, "estimated left      ~%s", b);
        PrintAt(fb, 1, 16, kGrey, "(%u frames at the current speed)", remaining);
        PrintAt(fb, 1, 18, kWhite, "speed %5.1f fps  (%.1f ms/frame)", 1000.0 / g_recentFrameMs, g_recentFrameMs);
    }
    PrintAt(fb, 1, 19, kWhite, "frame %u   memory %.1f MB", g_frames, (mallinfo().uordblks >> 10) / 1024.0);
    if (!g_scene.empty()) PrintAt(fb, 1, 20, kGrey, "scene %s", g_scene.c_str());

    if (warning) {
        PutText(fb, 1, 23, warning, kRed);
    } else {
        PutText(fb, 1, 23, "Waits in steps 5 and 7 are the game's", kGrey);
        PutText(fb, 1, 24, "own idle timers (no input in this test).", kGrey);
    }
    PutText(fb, 1, 27, "START: stop early (results are kept)", kGrey);
    gfxFlushBuffers();
    gfxSwapBuffers();
}

void EnterStep(int step)
{
    if (step <= g_step && g_runStart) return;  // steps only move forward
    const uint64_t now = svcGetSystemTick();
    if (g_runStart) {
        g_stepTicks[g_step] = now - g_stepStart;
        g_stepFrames[g_step] = g_frames - g_stepFrameStart;
        char d[24];
        FormatDuration(d, sizeof(d), TicksToMs(g_stepTicks[g_step]) / 1000.0);
        Log("== step %d of %d done: %s, %s, %u frames\n", g_step + 1, kStepCount, kSteps[g_step].name, d,
            g_stepFrames[g_step]);
    } else {
        g_runStart = now;
    }
    g_step = step;
    g_stepStart = now;
    g_stepFrameStart = g_frames;
    Log("== step %d of %d: %s (%s)\n", step + 1, kStepCount, kSteps[step].name, kSteps[step].detail);
    DrawStatus();
}

void LogStepSummary()
{
    Log("\n== time per step ==\n");
    for (int i = 0; i < kStepCount; ++i) {
        if (i > g_step) break;
        const uint64_t ticks = i == g_step ? svcGetSystemTick() - g_stepStart : g_stepTicks[i];
        const uint32_t frames = i == g_step ? g_frames - g_stepFrameStart : g_stepFrames[i];
        char d[24];
        FormatDuration(d, sizeof(d), TicksToMs(ticks) / 1000.0);
        Log("  %d. %-30s %10s  %6u frames%s\n", i + 1, kSteps[i].name, d, frames, i == g_step ? "  (last)" : "");
    }
    char d[24];
    FormatDuration(d, sizeof(d), g_runStart ? SecondsSince(g_runStart) : 0);
    Log("  total %s\n", d);
}

// Scene -> step. Scenes between the named ones (scMemoryChange, sq*) stay in the step they follow.
int StepForScene(const std::string& scene)
{
    if (scene == "scStrap") return g_step < kStepStrap ? kStepStrap : g_step;
    if (scene == "scBoot" || scene == "sqPrizeCheck") return g_step < kStepBoot ? kStepBoot : g_step;
    if (scene == "scTitle" || scene == "sqTitle") return g_step < kStepTitle ? kStepTitle : g_step;
    if (scene == "scMelee") return kStepMatch;
    return g_step;
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
    Log("STOP in step %d of %d (%s): %s: %s\n", g_step + 1, kStepCount, kSteps[g_step].name, title, detail);
    LogStepSummary();
    DrawStatus("Stopped - see the top screen.");
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
        // The demo match is ~1,141 frames (it ends before the 1,800-frame cap): leaving scMelee ends the measurement.
        const bool matchEnded = g_scene == "scMelee" && g_matchFrames > 0 && g_step == kStepMatch;
        g_scene = scene;
        if (matchEnded) {
            WriteMatchSummary();
            EnterStep(kStepResults);
            g_exitRequested = true;
        } else {
            EnterStep(StepForScene(scene));
        }
    }
    {
        const double ms = TicksToMs(total);
        g_recentFrameMs = g_recentFrameMs == 0 ? ms : g_recentFrameMs * 0.97 + ms * 0.03;
    }

    // Stall watchdog: a frame-based step running far past the PC build's frame count is waiting for something that
    // will not come (in this unattended build, usually a prompt that wants a button press).
    if (kSteps[g_step].pcFrames && g_step != kStepMatch) {
        const uint32_t stepFrames = g_frames - g_stepFrameStart;
        const uint32_t limit = kSteps[g_step].pcFrames * 2 + 600;
        if (stepFrames == limit) {
            char detail[200];
            std::snprintf(detail, sizeof(detail),
                          "step %d (%s) ran %u frames; the PC build\nneeds ~%u. Brawl is probably waiting for input\n"
                          "(e.g. a save data prompt). See headless.log.",
                          g_step + 1, kSteps[g_step].name, stepFrames, kSteps[g_step].pcFrames);
            StopWithMessage("Stalled", detail);
        }
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
            EnterStep(kStepResults);
            g_exitRequested = true;
        }
    }

    // Keep the console responsive (HOME menu, sleep, power button) and let START end the run.
    if ((g_frames & 7u) == 0) {
        if (!aptMainLoop()) g_exitRequested = true;
        hidScanInput();
        if (hidKeysDown() & KEY_START) g_exitRequested = true;
        DrawStatus();
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
    std::printf("This is a diagnostic build, not the game:\n"
                "no graphics, no input. It boots Brawl,\n"
                "lets the attract demo start a VS match\n"
                "and measures how fast the 3DS runs it.\n\n"
                "The bottom screen shows the step (1-%d),\n"
                "time spent and an estimate of time left.\n"
                "Steps 5 and 7 are Brawl's own idle\n"
                "timers: a player pressing A skips them.\n\n"
                "Stops by itself after 30 s of the match\n"
                "(or press START). Results:\n"
                "  sdmc:/3ds/brawl3ds/headless.log\n", kStepCount);

    EnterStep(0);
    struct stat info {};
    if (stat("sdmc:/3ds/brawl3ds/game/sys/boot.bin", &info) != 0) {
        StopWithMessage("Game files missing", "copy the extracted disc to sdmc:/3ds/brawl3ds/game/ (sys/ and files/)");
    }
    // A first boot (no save) stops at Brawl's "create save data?" prompt, which this build cannot answer.
    if (stat("sdmc:/3ds/brawl3ds/UserData/NAND/title/00010000/52534245/data/autosv0.bin", &info) != 0) {
        StopWithMessage("No Brawl save data",
                        "Brawl would stop at its 'create save data?'\nprompt and this build has no input.\n"
                        "Copy your save (the PC build's\nUserData/NAND/title/00010000/52534245/data)\n"
                        "to sdmc:/3ds/brawl3ds/UserData/NAND/title/\n00010000/52534245/data");
    }

    // The runtime indexes the whole disc folder at boot (5314 files for Brawl). Time the same kind of walk here so
    // the log shows how long the SD card takes, and whether directory iteration works at all.
    EnterStep(1);
    {
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
    EnterStep(2);
    Log("before reserving guest RAM: malloc in use %d KiB, linear free %lu KiB\n", mallinfo().uordblks >> 10,
        static_cast<unsigned long>(linearSpaceFree() >> 10));
    Brawl3dsReserveGuestMemory();
    Log("after reserving: malloc in use %d KiB, linear free %lu KiB\n", mallinfo().uordblks >> 10,
        static_cast<unsigned long>(linearSpaceFree() >> 10));

    EnterStep(3);
    SDL_SetMainReady();
    char program[] = "sdmc:/3ds/brawl3ds/brawl3ds.3dsx";
    char* argv[] = {program, nullptr};
    const int result = RuntimeMain(1, argv);

    WriteWindow();
    if (g_step != kStepResults) WriteMatchSummary();  // already written when the match measurement completed
    Log("RuntimeMain returned %d after %u frames\n", result, g_frames);
    LogStepSummary();
    if (g_log) std::fclose(g_log);
    gfxExit();
    return result;
}
