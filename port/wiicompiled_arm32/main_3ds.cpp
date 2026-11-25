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

#include "ppc_interpreter.h"
#include "audio_backend.h"
#include <string>
#include <sys/stat.h>

#include <unistd.h>

#include "brawl3ds_fast_tree.h"
#include "imgui.h"
#include "memory.h"
#include <xxhash.h>
#include "isa/ppc_isa_context.h"
#include "save_states.h"

void Brawl3dsProfilerStart(CpuContext* volatile* mainContextSlot);
void Brawl3dsProfilerSetRecording(bool on);
void Brawl3dsProfilerWrite(const char* path, unsigned frames, double averageFrameMs);

#include <SDL3/SDL_main.h>  // SDL_SetMainReady: the build defines SDL_MAIN_HANDLED, this file owns main()

int RuntimeMain(int argc, char** argv);
extern "C" void Brawl3dsReserveGuestMemory();
extern "C" void Brawl3dsAllocTrackStart();                            // alloc_tracker_3ds.cpp
extern "C" int Brawl3dsReserveFiberStacks(int count);                // runtime host_context_3ds.inc
extern "C" void Brawl3dsAllocTrackDump(const char* path, unsigned frame);

// Heap layout. libctru's default __system_allocateHeaps (weak) caps the regular heap at 96 MiB and, depending on
// which sizes are set, either leaves memory unallocated or gives all the rest to the linear heap. This build wants
// a fixed linear heap (guest regions that fit there, audio and later GPU buffers) and every other byte of the
// 178 MB system mode as regular heap (the runtime's own allocations and the larger guest regions), whatever size the
// code turns out to be. Same steps as libctru's version, different split.
extern "C" {
// The main thread's stack (libctru carves it from the start of the heap; its default is 32 KiB). The game's main
// thread runs on it: the VS demo reaches ~28 KiB deep, the end-of-match countdown went past 32 KiB and wrote below
// the heap. The other guest threads' fibers have 256 KiB (fiber_manager.cpp); the same here.
u32 __stacksize__ = 256 * 1024;
u32 __ctru_heap_size = 0;
u8* g_brawl3dsPackArena = nullptr;
u32 g_brawl3dsPackArenaSize = 0;
u32 __ctru_linear_heap_size = 0;
extern u32 __ctru_heap;
extern u32 __ctru_linear_heap;
extern char* fake_heap_start;
extern char* fake_heap_end;

void __system_allocateHeaps(void)
{
    // Room for MEM1 (24 MiB), the module homes (12 MiB) and the locked cache (1 MiB) with GuestFlat's 1 MiB reserve
    // for audio/GPU buffers, so the regular heap only carries MEM2 (54 MiB) besides the runtime's own allocations
    // (~20 MiB in the match). Sized explicitly: with 38 MB of code the 96 MiB regular cap happened to leave 42 MiB
    // linear, but the inline memory accesses grew the code to 55 MB, the old 29 MiB pushed MEM1 into the regular
    // heap, and the runtime ran out of memory in step 4 (std::bad_alloc).
    // 37 MiB of guest regions + the 1 MiB reserve + slack, + 3 MiB for the PICA renderer (citro3d's command buffer,
    // the per-frame vertex and index buffers: pica_renderer_3ds.cpp).
    // (44 MiB, 2 MiB more for the renderer, left the regular heap too small to start: 96-byte mallocs failed in
    // step 4. The renderer fits in the ~3.5 MiB guest RAM leaves: pica_renderer_3ds.cpp.)
    constexpr u32 kLinearHeap = 42u * 1024u * 1024u;
    constexpr u32 kRegularHeapMax = 96u * 1024u * 1024u;  // the kernel's heap window at 0x08000000
    // A match's fighters and stage are 1.5-4 MiB of translated code; moving the fighters' and stages' cold
    // functions out of the executable (tools/depromote_modules.py) freed 2.8 MiB of it.
    constexpr u32 kPackArena = 10u * 1024u * 1024u;

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
    // The top of the mapping is the code packs' arena (code_packs_3ds.cpp), outside malloc's heap: taken from it
    // later, megabytes in one piece left the heap without a 256 KiB block for a thread stack.
    g_brawl3dsPackArenaSize = heap > (64u << 20) ? kPackArena : 0;
    g_brawl3dsPackArena = reinterpret_cast<u8*>(__ctru_heap) + heap - g_brawl3dsPackArenaSize;
    fake_heap_start = reinterpret_cast<char*>(__ctru_heap);
    fake_heap_end = fake_heap_start + heap - g_brawl3dsPackArenaSize;
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
// Draws reaching the 3DS renderer back end (aurora3ds/backend_3ds.cpp), per log window and over the match.
uint64_t g_windowDraws = 0, g_windowVerts = 0, g_matchDraws = 0, g_matchVerts = 0;
uint64_t g_matchDisplayListTicks = 0;  // time in Aurora's GXCallDisplayList during the match
uint32_t g_pipelineConfigs = 0;
extern "C" void Brawl3dsRendererTakeStats(uint64_t* displayListTicks, uint32_t* pipelines);
extern "C" bool Brawl3dsPicaActive();            // aurora3ds/pica_renderer_3ds.cpp
extern "C" void Brawl3dsPicaCaptureNextFrame(const char* path);
extern "C" void Brawl3dsPicaWriteCaptures();
extern "C" void Brawl3dsPicaSetDebugFlags(unsigned flags) __attribute__((weak));  // testing aid: absent in older renderers
extern "C" void Brawl3dsPicaWriteStats(std::FILE* f, unsigned frames);
extern "C" void Brawl3dsPicaTakeWindowStats(double* busyMs, double* waitMs, uint32_t* wakeups);
extern "C" void Brawl3dsPicaTakeCpStats(double* cpMs, uint32_t* syncs, double* syncMs);
extern "C" uint32_t Brawl3dsPicaTakeDropped();
extern "C" void Brawl3dsPicaDebugNextFrame();
extern "C" const char* volatile g_brawl3dsRenderPhase;  // aurora3ds/pica_renderer_3ds.cpp
uint64_t g_droppedDraws = 0;                     // draws the renderer's per-frame buffers had no room for
extern "C" void Brawl3dsZonesReset();
extern "C" void Brawl3dsZonesWrite(std::FILE* f, unsigned frames);
uint64_t g_matchSum = 0, g_matchWork = 0, g_matchMax = 0;
uint32_t g_matchOver16 = 0, g_matchOver33 = 0;
std::string g_scene;

// Play mode (the playable build): the 3DS buttons drive GameCube controller port 1, nothing is scripted and the run
// does not stop at the demo match. Chosen at start-up: the play CIA's title (unique ID 0xBB3D3) or a
// sdmc:/3ds/brawl3ds/play.txt file (for the .3dsx and for tests with the headless title).
bool g_play = false;
extern "C" unsigned char g_brawl3dsGxTiming;
uint64_t g_interpretedCalls = 0;  // calls into functions this build left out (Brawl3ds_NotLinked)
uint32_t g_interpretedLogged = 0;
}  // namespace
void Brawl3dsInterpProfileWrite();
namespace {
unsigned g_renderEveryUser = 0;  // play mode's render frame skip: 0 automatic, else draw 1 game frame in N
double g_renderedFrameMs = 0;    // recent time of the game frames that were drawn (for the automatic skip)

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
// Heartbeat (HeartbeatThread): steps 1-4 run inside one long call with no game frames, so a second thread redraws
// the screen once a second and shows the runtime's latest output line, to tell "slow" apart from "stuck".
LightLock g_drawLock;
char g_lastLine[96] = "";
long g_stdoutBytes = 0;
uint64_t g_lastProgress = 0;  // tick of the last new runtime output or game frame
bool g_hungReported = false;
const char* volatile g_heartbeatWait = nullptr;  // the heartbeat's current file operation, shown on screen
uint32_t g_heartbeats = 0;
thread_local bool tl_isHeartbeat = false;
unsigned g_memoryKiB = 0;
// The main thread's g_currentCpuContext (thread_local), recorded by the main thread itself so the heartbeat can
// sample the running guest code: lr is the return address of the latest guest call, r1 the guest stack.
CpuContext* volatile* g_mainGuestContextSlot = nullptr;
uint32_t g_sampleLr = 0, g_sampleSp = 0, g_sampleR3 = 0;
uint32_t g_sameLrBeats = 0;

void SampleGuest()
{
    if (!g_mainGuestContextSlot) return;
    const CpuContext* ctx = *g_mainGuestContextSlot;
    if (!ctx) return;
    const uint32_t lr = ctx->lr;
    g_sameLrBeats = lr == g_sampleLr ? g_sameLrBeats + 1 : 0;
    g_sampleLr = lr;
    g_sampleSp = ctx->gpr[1];
    g_sampleR3 = ctx->gpr[3];
}
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

void DrawStatusLocked(const char* warning);

void DrawStatus(const char* warning = nullptr)
{
    LightLock_Lock(&g_drawLock);
    DrawStatusLocked(warning);
    LightLock_Unlock(&g_drawLock);
}

void DrawStatusLocked(const char* warning)
{
    u8* fb = gfxGetFramebuffer(GFX_BOTTOM, GFX_LEFT, nullptr, nullptr);
    if (!fb) return;
    std::memset(fb, 0, 320 * 240 * 3);
    constexpr u32 kWhite = 0xFFFFFF, kGrey = 0xA0A0A0, kGreen = 0x40D040, kYellow = 0xFFD040, kRed = 0xFF5050;
    char a[24], b[24];

    if (g_play && g_frames > 0) {
        PutText(fb, 1, 1, "Brawl3DS (playable test build)", kGreen);
        if (!g_scene.empty()) PrintAt(fb, 1, 3, kGrey, "scene %s   frame %u", g_scene.c_str(), g_frames);
        PrintAt(fb, 1, 9, kGrey, "interpreted: %llu calls, %llu M instr.",
                static_cast<unsigned long long>(g_interpretedCalls),
                static_cast<unsigned long long>(PpcInterpreter::InstructionCount() / 1000000u));
        if (g_recentFrameMs > 0) {
            // Brawl computes 60 game frames a second: the share of that this console reaches is the game's speed.
            const double gameFps = 1000.0 / g_recentFrameMs;
            PrintAt(fb, 1, 5, kWhite, "game speed %3.0f%%  (%.1f of 60 frames/s)", gameFps * 100.0 / 60.0, gameFps);
            PrintAt(fb, 1, 6, kWhite, "frame time %.0f ms", g_recentFrameMs);
        }
        if (g_renderEveryUser) {
            PrintAt(fb, 1, 8, kWhite, "pictures: 1 game frame in %u", g_renderEveryUser);
        } else {
            PutText(fb, 1, 8, "pictures: automatic", kWhite);
        }
        PutText(fb, 1, 11, "GameCube controller, port 1:", kGrey);
        PutText(fb, 1, 12, " A B X Y START D-pad as printed", kGrey);
        PutText(fb, 1, 13, " L R shoulder buttons, ZL/ZR = Z", kGrey);
        PutText(fb, 1, 14, " Circle Pad = stick, C-Stick = C", kGrey);
        PutText(fb, 1, 17, "SELECT + D-pad up/down: pictures", kGrey);
        PutText(fb, 1, 18, " (fewer pictures = faster game)", kGrey);
        PutText(fb, 1, 19, "SELECT + START: quit", kGrey);
        if (warning) PutText(fb, 1, 23, warning, kRed);
        gfxFlushBuffers();
        if (Brawl3dsPicaActive()) {
            gfxScreenSwapBuffers(GFX_BOTTOM, false);
        } else {
            gfxSwapBuffers();
        }
        return;
    }

    PutText(fb, 1, 1, "brawl3ds headless test (diagnostic)", kGrey);
    PrintAt(fb, 1, 3, kWhite, "Step %d of %d:", g_step + 1, kStepCount);
    PutText(fb, 1, 4, kSteps[g_step].name, g_step == kStepMatch ? kGreen : kYellow);
    PutText(fb, 1, 5, kSteps[g_step].detail, kGrey);

    const uint32_t stepFrames = g_frames - g_stepFrameStart;
    FormatDuration(a, sizeof(a), SecondsSince(g_stepStart));
    if (kSteps[g_step].pcFrames) {
        PrintAt(fb, 1, 7, kWhite, "frame %u of ~%u in this step", stepFrames, kSteps[g_step].pcFrames);
        Bar(fb, 8, static_cast<double>(stepFrames) / kSteps[g_step].pcFrames, g_step == kStepMatch ? kGreen : kWhite);
    } else if (g_stdoutBytes > 0) {
        // Runtime start-up: show its log growing and the latest line, so a slow step is visibly alive.
        const unsigned idle = g_lastProgress ? static_cast<unsigned>(SecondsSince(g_lastProgress)) : 0;
        PrintAt(fb, 1, 7, idle > 60 ? kRed : kGrey, "runtime log %ld KB, new output %us ago", g_stdoutBytes >> 10, idle);
        PutText(fb, 1, 8, g_lastLine, kGrey);
    } else {
        PutText(fb, 1, 7, "(not frame based)", kGrey);
    }
    if (g_frames == 0 && g_heartbeats) {
        PrintAt(fb, 1, 21, kGrey, "heartbeat %u%s%s", g_heartbeats, g_heartbeatWait ? ", next: " : "",
                g_heartbeatWait ? g_heartbeatWait : "");
        if (g_sampleLr) {
            PrintAt(fb, 1, 22, kWhite, "guest lr %08lX sp %08lX (same %us)", static_cast<unsigned long>(g_sampleLr),
                    static_cast<unsigned long>(g_sampleSp), g_sameLrBeats);
        }
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
    // mallinfo() takes the allocator's lock: only the main thread may ask (the heartbeat must keep drawing even if
    // the main thread is stuck inside malloc). Integer formatting only: newlib's float printf can allocate.
    if (!tl_isHeartbeat) g_memoryKiB = static_cast<unsigned>(mallinfo().uordblks >> 10);
    PrintAt(fb, 1, 19, kWhite, "frame %u   memory %u.%u MB", g_frames, g_memoryKiB / 1024, g_memoryKiB % 1024 * 10 / 1024);
    if (!g_scene.empty()) PrintAt(fb, 1, 20, kGrey, "scene %s", g_scene.c_str());

    if (warning) {
        PutText(fb, 1, 23, warning, kRed);
    } else {
        PutText(fb, 1, 23, "Waits in steps 5 and 7 are the game's", kGrey);
        PutText(fb, 1, 24, "own idle timers (no input in this test).", kGrey);
    }
    PutText(fb, 1, 27, "START: stop early (results are kept)", kGrey);
    gfxFlushBuffers();
    // Once the PICA renderer owns the top screen (citro3d swaps it), the status screen swaps only its own.
    if (Brawl3dsPicaActive()) {
        gfxScreenSwapBuffers(GFX_BOTTOM, false);
    } else {
        gfxSwapBuffers();
    }
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

// Ends the process without libctru's normal exit path. That path runs static destructors and then frees the heap
// while the runtime's own threads (the AX mix worker, watchdogs) are still running on stacks inside it: Azahar
// logged ~2,200 "unmapped" reads from the mix worker's stack right after the match ended, which a real 3DS turns
// into a data abort at exit. svcExitProcess stops every thread first.
// The service shutdown of that path still has to run: __appExit() is libctru's own (hidExit, which also closes the
// New 3DS's ir:rst, fsExit, aptExit, which tells the HOME Menu the application is closing, and srvExit). With only
// aptExit the HOME Menu still reported "An error has occurred" and asked for a restart after every real-hardware
// run; without any of it, likewise.
extern "C" void __appExit(void);
[[noreturn]] void ExitNow()
{
    std::fflush(stdout);
    std::fflush(stderr);
    if (g_log) std::fclose(g_log);
    g_log = nullptr;
    if (g_play) {
        // The playable build leaves through a restart of the console: this application runs in the New 3DS's
        // 178 MB mode, and ending the process (below) left the HOME Menu with "An error has occurred" on real
        // hardware whatever was shut down first. The restart takes a few seconds and shows no error. If the
        // request is refused the old path follows.
        AudioBackend::Instance().Shutdown();
        if (R_SUCCEEDED(APT_HardwareResetAsync())) {
            for (;;) svcSleepThread(100000000LL);
        }
    }
    gfxExit();
    __appExit();
    svcExitProcess();
    for (;;) {
    }
}

// The sound follows the game's speed (audio_backend_3ds.cpp): 1 at 60 game frames a second.
extern "C" float Brawl3dsAudioSpeed()
{
    return g_recentFrameMs > 0 ? static_cast<float>((1000.0 / 60.0) / g_recentFrameMs) : 1.0f;
}
bool g_audioOff = false;  // debug_flags.txt 1024

// Fingerprints of the game's memory, to prove on the PC that a change (a native function, a renderer shortcut) left
// the game's behaviour alone: sdmc:/3ds/brawl3ds/det_hash.txt holds "<every> [<dump frame> ...]". Every <every>
// frames one line goes to det_hash_out.txt: the frame, then one 32-bit hash per MiB of MEM1 (24), MEM2 (54) and the
// module homes (12). At a dump frame the three regions are written whole (mem_<frame>_<region>.bin).
// tools/det_compare.py compares two runs. Off (no file): one fopen at the first frame.
void DetHashFrame() {
    static int every = -1;
    static uint32_t dumps[8];
    static unsigned dumpCount = 0;
    static std::FILE* out = nullptr;
    if (every == -1) {
        every = 0;
        if (std::FILE* f = std::fopen("sdmc:/3ds/brawl3ds/det_hash.txt", "r")) {
            unsigned n = 0;
            if (std::fscanf(f, "%u", &n) == 1) every = static_cast<int>(n);
            while (dumpCount < 8 && std::fscanf(f, "%u", &n) == 1) dumps[dumpCount++] = n;
            std::fclose(f);
            if (every > 0) out = std::fopen("sdmc:/3ds/brawl3ds/det_hash_out.txt", "w");
            Log("det_hash.txt: memory hashes every %d frames, %u dump(s)\n", every, dumpCount);
        }
    }
    if (every <= 0) return;
    struct Region {
        const char* name;
        uint32_t base;
        size_t size;
    };
    static const Region regions[] = {{"mem1", Memory::kMem1CachedBase, Memory::kMem1Size},
                                     {"mem2", Memory::kMem2CachedBase, Memory::kMem2Size},
                                     {"homes", Memory::kModuleHomesBase, Memory::kModuleHomesSize}};
    if (out && g_frames % static_cast<uint32_t>(every) == 0) {
        std::fprintf(out, "%u", g_frames);
        for (const Region& r : regions) {
            const uint8_t* p = Memory::GetPointer(r.base, 4);
            for (size_t at = 0; at < r.size; at += 1u << 20) {
                std::fprintf(out, " %08x", static_cast<unsigned>(XXH3_64bits(p + at, 1u << 20)));
            }
        }
        std::fputc('\n', out);
        std::fflush(out);
    }
    for (unsigned i = 0; i < dumpCount; ++i) {
        if (dumps[i] != g_frames) continue;
        for (const Region& r : regions) {
            char path[64];
            std::snprintf(path, sizeof(path), "sdmc:/3ds/brawl3ds/mem_%u_%s.bin", g_frames, r.name);
            if (std::FILE* f = std::fopen(path, "wb")) {
                std::fwrite(Memory::GetPointer(r.base, 4), 1, r.size, f);
                std::fclose(f);
            }
        }
        Log("det_hash.txt: memory written at frame %u\n", g_frames);
    }
}
extern "C" int Brawl3dsAudioWanted() { return g_play && !g_audioOff; }

// Can this process run code it wrote itself? Translated code for one fighter or stage is 0.2-1.6 MiB and the whole
// game's does not fit (~95 MiB), so the plan is to load a module's translated code from the SD card when the game
// links that module: that needs memory made executable at run time (svcControlProcessMemory). One page, one
// function that returns 42. The steps are written to sdmc:/3ds/brawl3ds/exec_probe.txt as they happen, so a
// crash in the middle leaves "calling" there, and the probe is not repeated while that file exists.
void ExecProbe()
{
    const char* path = "sdmc:/3ds/brawl3ds/exec_probe.txt";
    if (std::FILE* old = std::fopen(path, "r")) {
        char line[160] = "";
        std::fgets(line, sizeof(line), old);
        std::fclose(old);
        Log("exec probe: done before, result: %s", line[0] ? line : "(empty)\n");
        return;
    }
    const auto state = [path](const char* text) {
        if (std::FILE* f = std::fopen(path, "w")) {
            std::fputs(text, f);
            std::fclose(f);
        }
    };
    state("started\n");
    auto* page = static_cast<u32*>(memalign(0x1000, 0x1000));
    if (!page) {
        state("failed: no memory\n");
        return;
    }
    page[0] = 0xE3A0002Au;  // mov r0, #42
    page[1] = 0xE12FFF1Eu;  // bx lr
    Handle self = CUR_PROCESS_HANDLE;
    const Result dup = svcDuplicateHandle(&self, CUR_PROCESS_HANDLE);
    if (R_FAILED(dup)) self = CUR_PROCESS_HANDLE;
    const Result flush = svcFlushProcessDataCache(self, reinterpret_cast<u32>(page), 0x1000);
    Result protect = svcControlProcessMemory(self, reinterpret_cast<u32>(page), 0, 0x1000, MEMOP_PROT,
                                             MEMPERM_READ | MEMPERM_EXECUTE);
    int perm = 5;
    if (R_FAILED(protect)) {
        protect = svcControlProcessMemory(self, reinterpret_cast<u32>(page), 0, 0x1000, MEMOP_PROT,
                                          MEMPERM_READ | MEMPERM_WRITE | MEMPERM_EXECUTE);
        perm = 7;
    }
    Log("exec probe: page %08lX, handle %08lX (duplicate %08lX), flush %08lX, protect r%sx %08lX\n",
        static_cast<unsigned long>(reinterpret_cast<u32>(page)), static_cast<unsigned long>(self),
        static_cast<unsigned long>(dup), static_cast<unsigned long>(flush), perm == 7 ? "w" : "-",
        static_cast<unsigned long>(protect));
    char text[120];
    if (R_FAILED(protect)) {
        std::snprintf(text, sizeof(text), "failed: protect %08lX (flush %08lX, duplicate %08lX)\n",
                      static_cast<unsigned long>(protect), static_cast<unsigned long>(flush),
                      static_cast<unsigned long>(dup));
        state(text);
        Log("exec probe: %s", text);
    } else {
        state("calling (if this is still here, the call crashed)\n");
        if (g_log) std::fflush(g_log);
        const int value = reinterpret_cast<int (*)()>(page)();
        std::snprintf(text, sizeof(text), "%s: the page returned %d (permissions %d)\n", value == 42 ? "ok" : "wrong",
                      value, perm);
        state(text);
        Log("exec probe: %s", text);
        svcControlProcessMemory(self, reinterpret_cast<u32>(page), 0, 0x1000, MEMOP_PROT, MEMPERM_READ | MEMPERM_WRITE);
    }
    if (self != CUR_PROCESS_HANDLE) svcCloseHandle(self);
    // The page is not freed: a block that changed protection and back is left alone.
}

// The end of a normal run, called from the game's own frame (main thread): results are written, then the process
// ends here. Asking the runtime to shut down instead (AURORA_EXIT) ends in libctru's exit path, which frees the heap
// under the still-running mix worker (see ExitNow).
[[noreturn]] void FinishRun(const char* why)
{
    Log("\nrun finished: %s, after %u frames\n", why, g_frames);
    LogStepSummary();
    DrawStatus("Finished - results in headless.log");
    ExitNow();
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
        // (The renderer had the top screen at 24 bits; consoleInit puts it back to the console's format.)
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
    ExitNow();
}

// CPU exceptions (data abort, undefined instruction, ...): libctru calls OnException on g_exceptionStack in the
// faulting thread, which reports from there and never returns. (Redirecting the thread's pc/sp into the report
// instead crashed a second time on a real New 3DS, losing the first fault's address.)
alignas(8) uint8_t g_exceptionStack[64 * 1024];
ERRF_ExceptionInfo g_exceptionInfo;
CpuRegisters g_exceptionRegs;

[[noreturn]] void CrashReport()
{
    const CpuRegisters& r = g_exceptionRegs;
    const auto u = [](u32 v) { return static_cast<unsigned long>(v); };
    char detail[512];
    std::snprintf(detail, sizeof(detail),
                  "exception type %d at pc=%08lX lr=%08lX sp=%08lX\nfault address %08lX (fsr %08lX) cpsr %08lX\n"
                  "r0 %08lX r1 %08lX r2 %08lX r3 %08lX\nr4 %08lX r5 %08lX r6 %08lX r7 %08lX\n"
                  "r8 %08lX r9 %08lX r10 %08lX r11 %08lX r12 %08lX\nguest lr %08lX sp %08lX (heartbeat sample)",
                  static_cast<int>(g_exceptionInfo.type), u(r.pc), u(r.lr), u(r.sp), u(g_exceptionInfo.far),
                  u(g_exceptionInfo.fsr), u(r.cpsr), u(r.r[0]), u(r.r[1]), u(r.r[2]), u(r.r[3]), u(r.r[4]), u(r.r[5]),
                  u(r.r[6]), u(r.r[7]), u(r.r[8]), u(r.r[9]), u(r.r[10]), u(r.r[11]), u(r.r[12]), u(g_sampleLr),
                  u(g_sampleSp));
    StopWithMessage("CPU exception", detail);
}

void OnException(ERRF_ExceptionInfo* info, CpuRegisters* regs)
{
    g_exceptionInfo = *info;
    g_exceptionRegs = *regs;
    CrashReport();  // on g_exceptionStack; locks this thread holds are recursive, so stdio and malloc still work
}

void WriteWindow()
{
    if (g_windowFrames == 0) return;
    const double avg = TicksToMs(g_windowSum) / g_windowFrames;
    double gpuBusy = 0, gpuWait = 0;
    uint32_t gpuWakeups = 0;
    Brawl3dsPicaTakeWindowStats(&gpuBusy, &gpuWait, &gpuWakeups);
    double cpMs = 0, cpSyncMs = 0;
    uint32_t cpSyncs = 0;
    Brawl3dsPicaTakeCpStats(&cpMs, &cpSyncs, &cpSyncMs);
    const struct mallinfo heap = mallinfo();
    static uint64_t lastInterpCalls = 0, lastInterpInstr = 0;
    const uint64_t interpInstr = PpcInterpreter::InstructionCount();
    if (g_interpretedCalls != lastInterpCalls) {
        Log("interpreted since the last line: %llu calls, %llu instructions\n",
            static_cast<unsigned long long>(g_interpretedCalls - lastInterpCalls),
            static_cast<unsigned long long>(interpInstr - lastInterpInstr));
        lastInterpCalls = g_interpretedCalls;
        lastInterpInstr = interpInstr;
        if (g_play) Brawl3dsInterpProfileWrite();  // with every window that interpreted something
    }
    Log("frame %6u  scene %-16s  avg %6.2f ms (game %6.2f)  min %6.2f  max %6.2f  ~%4.1f fps  malloc %d KiB"
        " (free %d)  draws %llu verts %llu  gpu worker %.1f ms (cp %.1f), wait %.1f ms, wakes %.0f, syncs %.1f (%.1f ms)\n",
        g_frames, g_scene.c_str(), avg, TicksToMs(g_windowWork) / g_windowFrames, TicksToMs(g_windowMin),
        TicksToMs(g_windowMax), avg > 0 ? 1000.0 / avg : 0.0, heap.uordblks >> 10, heap.fordblks >> 10,
        static_cast<unsigned long long>(g_windowDraws / g_windowFrames),
        static_cast<unsigned long long>(g_windowVerts / g_windowFrames), gpuBusy / g_windowFrames,
        cpMs / g_windowFrames, gpuWait / g_windowFrames, static_cast<double>(gpuWakeups) / g_windowFrames,
        static_cast<double>(cpSyncs) / g_windowFrames, cpSyncMs / g_windowFrames);
    g_windowDraws = g_windowVerts = 0;
    g_windowSum = g_windowWork = g_windowMax = 0;
    g_windowMin = UINT64_MAX;
    g_windowFrames = 0;
}

void WriteMatchSummary()
{
    if (g_matchFrames == 0) return;
    const double avg = TicksToMs(g_matchSum) / g_matchFrames;
    Brawl3dsProfilerWrite("sdmc:/3ds/brawl3ds/profile.txt", g_matchFrames, avg);
    Brawl3dsZonesWrite(g_log, g_matchFrames);
    Brawl3dsPicaWriteStats(g_log, g_matchFrames);
    Brawl3dsPicaWriteCaptures();
    Log("\n== VS demo match: %u frames measured ==\n"
        "average frame  %.2f ms (%.1f fps)\n"
        "average game   %.2f ms (time between aurora_begin_frame and aurora_end_frame)\n"
        "worst frame    %.2f ms\n"
        "frames > 16.7 ms (miss 60 fps): %u (%.1f%%)\n"
        "frames > 33.3 ms (miss 30 fps): %u (%.1f%%)\n"
        "renderer back end: %.0f draws and %.0f vertices per frame, %.2f ms per frame in GXCallDisplayList, "
        "%u pipeline configs\n",
        g_matchFrames, avg, avg > 0 ? 1000.0 / avg : 0.0, TicksToMs(g_matchWork) / g_matchFrames,
        TicksToMs(g_matchMax), g_matchOver16, 100.0 * g_matchOver16 / g_matchFrames, g_matchOver33,
        100.0 * g_matchOver33 / g_matchFrames, static_cast<double>(g_matchDraws) / g_matchFrames,
        static_cast<double>(g_matchVerts) / g_matchFrames, TicksToMs(g_matchDisplayListTicks) / g_matchFrames,
        g_pipelineConfigs);
}

}  // namespace

// The renderer back end's draw count (aurora3ds/backend_3ds.cpp), one call per GX draw that reaches it.
uint32_t g_frameDraws = 0;  // this frame's GX draws (logged around the last capture)
extern "C" void Brawl3dsRendererCountDraw(uint32_t vertices, uint32_t)
{
    ++g_windowDraws;
    ++g_frameDraws;
    g_windowVerts += vertices;
    if (g_scene == "scMelee") {
        ++g_matchDraws;
        g_matchVerts += vertices;
    }
}
extern "C" void Brawl3dsRendererEndFrame();

namespace {

// Latest non-empty line of stdout.txt, without colour escapes, for the status screen.
void ReadLastLine(long size)
{
    std::FILE* f = std::fopen("sdmc:/3ds/brawl3ds/stdout.txt", "rb");
    if (!f) return;
    char buf[512];
    const long from = size > static_cast<long>(sizeof(buf)) - 1 ? size - static_cast<long>(sizeof(buf)) + 1 : 0;
    std::fseek(f, from, SEEK_SET);
    const size_t n = std::fread(buf, 1, sizeof(buf) - 1, f);
    std::fclose(f);
    buf[n] = 0;
    size_t end = n;
    while (end > 0 && (buf[end - 1] == '\n' || buf[end - 1] == '\r' || buf[end - 1] == ' ')) --end;
    size_t start = end;
    while (start > 0 && buf[start - 1] != '\n') --start;
    char line[96];
    size_t o = 0;
    for (size_t i = start; i < end && o < 39; ++i) {  // one 39-column row on the bottom screen
        if (buf[i] == 0x1b) {  // ESC [ ... m
            while (i < end && buf[i] != 'm') ++i;
            continue;
        }
        line[o++] = (buf[i] >= 32 && buf[i] < 127) ? buf[i] : ' ';
    }
    line[o] = 0;
    if (o) std::memcpy(g_lastLine, line, o + 1);
}

// Runs beside RuntimeMain at a higher priority and sleeps most of the time. Before the first game frame (steps 1-4)
// nothing else redraws the screen, so this flushes the runtime's output, shows its last line, redraws once a second,
// logs a heartbeat every 15 s, and flags 10 minutes without any new output as a probable hang. START exits.
void HeartbeatThread(void*)
{
    tl_isHeartbeat = true;
    Log("heartbeat: thread running\n");
    uint32_t seconds = 0;
    for (;;) {
        svcSleepThread(1000000000LL);
        ++seconds;
        g_heartbeats = seconds;
        if (seconds <= 30) {
            Log("heartbeat %u: frames %u, step %d, wait %s\n", seconds, g_frames, g_step + 1,
                g_heartbeatWait ? g_heartbeatWait : "-");
        }
        if (g_frames > 0) {
            if (g_step >= kStepResults) return;
            // Frames are running: Brawl3dsFrameEnd redraws. A frame count that stops moving is a stall (seen in
            // Azahar on the safety screen; on the console once, 248 s in VIWaitForRetrace after a 3 s frame): say
            // where the guest and the renderer are, every 30 s while it lasts. (guest lr 80023B20 =
            // VIWaitForRetrace's call in the frame loop.)
            static uint32_t lastFrames = 0, stillSeconds = 0;
            if (g_frames != lastFrames) {
                if (stillSeconds >= 10) Log("stall over after %u s (frame %u)\n", stillSeconds, g_frames);
                lastFrames = g_frames;
                stillSeconds = 0;
            } else if (++stillSeconds >= 10 && (stillSeconds - 10) % 30 == 0) {
                SampleGuest();
                Log("STALL: no frame for %u s at frame %u (step %d, scene %s); guest lr %08lX sp %08lX r3 %08lX; "
                    "renderer: %s\n",
                    stillSeconds, g_frames, g_step + 1, g_scene.c_str(), static_cast<unsigned long>(g_sampleLr),
                    static_cast<unsigned long>(g_sampleSp), static_cast<unsigned long>(g_sampleR3),
                    g_brawl3dsRenderPhase ? g_brawl3dsRenderPhase : "-");
                if (g_log) std::fflush(g_log);
            }
            continue;
        }
        // Screen first: it needs no file access, so it keeps working even if the SD card service or a stdio lock is
        // stuck. g_heartbeatWait names the next operation, so a frozen screen shows which one never returned.
        // (No fflush(stdout) here: if the main thread hangs mid-write it holds that lock, and the heartbeat would
        // hang with it. stdout's 1 KiB buffer still reaches the card often enough to see progress.)
        if (!g_lastProgress) g_lastProgress = svcGetSystemTick();
        SampleGuest();
        g_heartbeatWait = "SD: size of stdout.txt";
        DrawStatus(g_hungReported ? "No output for 10 min: probably stuck. START exits; send stdout.txt" : nullptr);
        struct stat info {};
        if (stat("sdmc:/3ds/brawl3ds/stdout.txt", &info) == 0 && info.st_size != g_stdoutBytes) {
            g_stdoutBytes = info.st_size;
            g_lastProgress = svcGetSystemTick();
            g_heartbeatWait = "SD: reading stdout.txt";
            ReadLastLine(g_stdoutBytes);
        }
        const double idle = SecondsSince(g_lastProgress);
        g_heartbeatWait = "SD: writing headless.log";
        if (seconds % 15 == 0) {
            Log("  .. step %d of %d still running, %.0f s in; guest lr %08lX sp %08lX r3 %08lX; runtime log %ld "
                "bytes, last output %.0f s ago: %s\n",
                g_step + 1, kStepCount, SecondsSince(g_stepStart), static_cast<unsigned long>(g_sampleLr),
                static_cast<unsigned long>(g_sampleSp), static_cast<unsigned long>(g_sampleR3), g_stdoutBytes, idle,
                g_lastLine);
        }
        if (idle > 600 && !g_hungReported) {
            g_hungReported = true;
            Log("HUNG? no runtime output for 10 minutes in step %d of %d. Last line: %s\n", g_step + 1, kStepCount,
                g_lastLine);
            LogStepSummary();
        }
        hidScanInput();
        if (hidKeysDown() & KEY_START) {
            Log("stopped with START in step %d of %d (before the first game frame)\n", g_step + 1, kStepCount);
            LogStepSummary();
            if (g_log) std::fclose(g_log);
            g_log = nullptr;
            DrawStatus("Stopped with START. Closing...");
            svcSleepThread(1500000000LL);
            ExitNow();
        }
        g_heartbeatWait = nullptr;
        DrawStatus(g_hungReported ? "No output for 10 min: probably stuck. START exits; send stdout.txt"
                                  : nullptr);
    }
}

} // namespace

extern "C" void Brawl3dsFatal(const char* what)
{
    StopWithMessage("Fatal error", what);
}

// Every translated function the build left out (tools/select_translated_code.py) calls this.
//
// The whole game's translated code (~95 MiB) does not fit beside the Wii's RAM, so the build links the functions
// the VS demo runs and leaves the rest as stubs. Those run in the runtime's PowerPC interpreter
// (ppc_interpreter.cpp: the function's own guest code, still in guest memory, until it returns; its calls go back
// through the dispatch tables, into translated code where there is some). Slower by far, but everything the demo
// does not reach (menus, other fighters and stages) runs at all. The log names the functions and counts the
// instructions: the busiest are the ones to add to the translated set.
// The pre-linker's cross-module veneers (gen3ds/trampolines_3ds.cpp, tools/gen_trampoline_table.py): their bytes are
// not in guest memory, so the interpreter cannot run them; each is a jump to its target through r12 and ctr.
extern "C" const uint32_t g_brawl3dsTrampolineCount;
extern "C" const uint32_t g_brawl3dsTrampolines[][2];
static uint32_t TrampolineTarget(uint32_t address)
{
    uint32_t lo = 0, hi = g_brawl3dsTrampolineCount;
    while (lo < hi) {
        const uint32_t mid = (lo + hi) / 2;
        if (g_brawl3dsTrampolines[mid][0] < address) lo = mid + 1;
        else hi = mid;
    }
    return lo < g_brawl3dsTrampolineCount && g_brawl3dsTrampolines[lo][0] == address ? g_brawl3dsTrampolines[lo][1] : 0;
}

// The interpreted functions' calls and own instructions, by address (open addressing; a full table drops the rest).
extern "C" void (*Brawl3dsPackFunction(uint32_t guest))(CpuContext*);
extern "C" void Brawl3dsPacksInit();
extern "C" void Brawl3dsPacksSceneChange(int leavingMatch);

struct InterpProfileEntry {
    uint32_t address, calls;
    uint64_t instructions;
};
constexpr uint32_t kInterpProfileSlots = 8192;
static InterpProfileEntry g_interpProfile[kInterpProfileSlots];

static void InterpProfileAdd(uint32_t address, uint64_t instructions, uint32_t calls = 1)
{
    uint32_t slot = ((address >> 2) * 2654435761u) >> 19;  // 13 bits
    for (uint32_t probe = 0; probe < 64; ++probe, slot = (slot + 1) & (kInterpProfileSlots - 1)) {
        InterpProfileEntry& e = g_interpProfile[slot];
        if (e.address == address || e.address == 0) {
            e.address = address;
            e.calls += calls;
            e.instructions += instructions;
            return;
        }
    }
}

// sdmc:/3ds/brawl3ds/interp_profile.txt: "<address> <calls> <own instructions>", the costliest first
// (tools/promote_functions.py reads it). Written with the frame-time windows in play mode (every 600 frames and at
// scene changes). It adds up over the sessions: the
// file already there is read in first (delete it to start over, e.g. with a new build).
void Brawl3dsInterpProfileWrite()
{
    static bool merged = false;
    if (!merged) {
        merged = true;
        if (std::FILE* old = std::fopen("sdmc:/3ds/brawl3ds/interp_profile.txt", "r")) {
            unsigned long address = 0, calls = 0;
            unsigned long long instructions = 0;
            while (std::fscanf(old, "%lx %lu %llu", &address, &calls, &instructions) == 3)
                InterpProfileAdd(static_cast<uint32_t>(address), instructions, static_cast<uint32_t>(calls));
            std::fclose(old);
        }
    }
    static InterpProfileEntry* sorted[kInterpProfileSlots];
    uint32_t count = 0;
    for (auto& e : g_interpProfile)
        if (e.address) sorted[count++] = &e;
    if (count == 0) return;
    std::sort(sorted, sorted + count, [](const InterpProfileEntry* a, const InterpProfileEntry* b) {
        return a->instructions + 40ull * a->calls > b->instructions + 40ull * b->calls;
    });
    std::FILE* f = std::fopen("sdmc:/3ds/brawl3ds/interp_profile.txt", "w");
    if (!f) return;
    for (uint32_t i = 0; i < count; ++i)
        std::fprintf(f, "%08lX %lu %llu\n", static_cast<unsigned long>(sorted[i]->address),
                     static_cast<unsigned long>(sorted[i]->calls),
                     static_cast<unsigned long long>(sorted[i]->instructions));
    std::fclose(f);
}

extern "C" void Brawl3ds_NotLinked(uint32_t guestAddress, CpuContext* ctx)
{
    if (ctx) {
        if (const uint32_t target = TrampolineTarget(guestAddress)) {
            ctx->gpr[12] = target;
            ctx->ctr = target;
            InvokeIndirectJump(target, ctx);
            return;
        }
    }
    // A fighter's or stage's function: from its code pack, loaded now if this is the module's first call
    // (code_packs_3ds.cpp).
    if (ctx) {
        if (void (*packed)(CpuContext*) = Brawl3dsPackFunction(guestAddress)) {
            packed(ctx);
            return;
        }
    }
    if (ctx && PpcInterpreter::CanRun(guestAddress)) {
        ++g_interpretedCalls;
        // The host stack going lower than it ever has (the heap's first block is a stack: an overflow there writes
        // below the heap): each new low, a kilobyte at a time, names the guest call that made it.
        static uintptr_t lowestSp = ~static_cast<uintptr_t>(0);
        const uintptr_t sp = reinterpret_cast<uintptr_t>(__builtin_frame_address(0));
        if (sp + 1024 < lowestSp) {
            lowestSp = sp;
            std::fprintf(stderr, "[interp] host sp %08lX: guest function %08lX from lr=%08lX, guest sp %08lX\n",
                         static_cast<unsigned long>(sp), static_cast<unsigned long>(guestAddress),
                         static_cast<unsigned long>(ctx->lr), static_cast<unsigned long>(ctx->gpr[1]));
        }
        if (g_interpretedLogged < 400) {
            ++g_interpretedLogged;
            std::fprintf(stderr, "[interp] guest function %08lX (not linked) called from lr=%08lX\n",
                         static_cast<unsigned long>(guestAddress), static_cast<unsigned long>(ctx->lr));
        }
        // What each interpreted function costs (its own instructions: the interpreter counts a call's when it
        // returns, the calls it made first): interp_profile.txt names the ones to add to the translated set.
        static uint64_t childInstr = 0;
        const uint64_t before = PpcInterpreter::InstructionCount(), outerChildren = childInstr;
        childInstr = 0;
        PpcInterpreter::Run(ctx, guestAddress);
        const uint64_t inclusive = PpcInterpreter::InstructionCount() - before;
        InterpProfileAdd(guestAddress, inclusive > childInstr ? inclusive - childInstr : 0);
        childInstr = outerChildren + inclusive;
        return;
    }
    char detail[200];
    std::snprintf(detail, sizeof(detail),
                  "guest function %08lX is not in this build\n(called from guest lr=%08lX; add its module to --keep)",
                  static_cast<unsigned long>(guestAddress), static_cast<unsigned long>(ctx ? ctx->lr : 0));
    StopWithMessage("Code not linked", detail);
}

// Render frame skip (performance test, debug_flags.txt 128: draw 1 game frame in 2; 256: 1 in 4): in the match the
// game's render phase (gfTaskScheduler::renderPre and ::render, wrapped with -Wl,--wrap in port/headless/
// CMakeLists.txt) runs only every Nth game frame; the game logic runs every frame. The skipped frames show the last
// picture. The match average then reads as logic + render / N.
unsigned g_renderEvery = 1;
bool g_skipRender = false;
uint64_t g_skippedRenders = 0;
extern "C" void __real_func_8002E79C(CpuContext* ctx);  // renderPre__15gfTaskSchedulerFv
extern "C" void __real_func_8002E844(CpuContext* ctx);  // render__15gfTaskSchedulerFv
extern "C" void __wrap_func_8002E79C(CpuContext* ctx)
{
    unsigned every = g_scene == "scMelee" ? g_renderEvery : 1;
    if (g_play) {
        // Automatic: a drawn frame that takes over 60 ms gives up every second picture, over 150 ms two of three
        // (the game's logic, which sets its speed, then gets most of the time).
        every = g_renderEveryUser ? g_renderEveryUser : g_renderedFrameMs > 150 ? 3 : g_renderedFrameMs > 60 ? 2 : 1;
    }
    g_skipRender = every > 1 && (g_frames % every) != 0;
    if (g_skipRender) {
        ++g_skippedRenders;
        return;
    }
    __real_func_8002E79C(ctx);
}
extern "C" void __wrap_func_8002E844(CpuContext* ctx)
{
    if (g_skipRender) return;
    __real_func_8002E844(ctx);
}

// Play mode: is this the playable build (hle/input/pad.cpp, wii_remote_input.cpp and settings_overlay.cpp ask).
extern "C" bool Brawl3dsPlayMode()
{
    return g_play;
}

// Play mode: the 3DS buttons as a GameCube controller (hle/input/pad.cpp fills port 1 with it at every PADRead).
// A B X Y START and the D-pad are themselves, L and R the shoulder buttons (full analog), ZL or ZR is Z, the Circle
// Pad is the control stick and the New 3DS C-Stick the C-stick. SELECT is this port's own key: with START it ends
// the program, with D-pad up/down it changes the render frame skip (automatic, 1 in 1 ... 1 in 6).
// buttons: PADStatus bits. sticks: stick x, y, C-stick x, y. triggers: left, right.
//
// Testing aid: sdmc:/3ds/brawl3ds/play_script.txt, lines of "<game frame> <frames held> <PADStatus buttons, hex>
// [stick x] [stick y]", adds those presses to the controller (menus can be driven in an emulator without a
// keyboard). The scene line of headless.log shows where they led.
struct PlayScriptEvent {
    uint32_t start, frames;
    uint16_t buttons;
    int8_t stickX, stickY;
};
PlayScriptEvent g_playScript[96];
unsigned g_playScriptCount = 0;

void LoadPlayScript()
{
    std::FILE* f = std::fopen("sdmc:/3ds/brawl3ds/play_script.txt", "r");
    if (!f) return;
    char line[96];
    while (g_playScriptCount < 96 && std::fgets(line, sizeof(line), f)) {
        unsigned start = 0, frames = 0, buttons = 0;
        int x = 0, y = 0;
        unsigned skip = 0;  // "skip N": draw 1 game frame in N (a long test gets to the end sooner)
        if (std::sscanf(line, "skip %u", &skip) == 1) {
            g_renderEveryUser = skip;
            continue;
        }
        if (std::sscanf(line, "%u %u %x %d %d", &start, &frames, &buttons, &x, &y) >= 3) {
            g_playScript[g_playScriptCount++] = {start, frames, static_cast<uint16_t>(buttons),
                                                 static_cast<int8_t>(x), static_cast<int8_t>(y)};
        }
    }
    std::fclose(f);
    Log("play_script.txt: %u scripted press(es)\n", g_playScriptCount);
}

extern "C" bool Brawl3dsReadPad(uint16_t* buttons, int8_t* sticks, uint8_t* triggers)
{
    if (!g_play) return false;
    static bool scriptLoaded = false;
    if (!scriptLoaded) {
        scriptLoaded = true;
        LoadPlayScript();
    }
    hidScanInput();
    const u32 held = hidKeysHeld(), down = hidKeysDown();
    *buttons = 0;
    sticks[0] = sticks[1] = sticks[2] = sticks[3] = 0;
    triggers[0] = triggers[1] = 0;
    if (held & KEY_SELECT) {
        if (down & KEY_START) FinishRun("SELECT + START");
        if (down & KEY_DUP) g_renderEveryUser = g_renderEveryUser >= 6 ? 6 : g_renderEveryUser + 1;
        if (down & KEY_DDOWN) g_renderEveryUser = g_renderEveryUser ? g_renderEveryUser - 1 : 0;
        return true;  // the controller at rest while SELECT is held
    }
    uint16_t b = 0;
    if (held & KEY_A) b |= 0x0100;
    if (held & KEY_B) b |= 0x0200;
    if (held & KEY_X) b |= 0x0400;
    if (held & KEY_Y) b |= 0x0800;
    if (held & KEY_START) b |= 0x1000;
    if (held & KEY_DLEFT) b |= 0x0001;
    if (held & KEY_DRIGHT) b |= 0x0002;
    if (held & KEY_DDOWN) b |= 0x0004;
    if (held & KEY_DUP) b |= 0x0008;
    if (held & (KEY_ZL | KEY_ZR)) b |= 0x0010;
    if (held & KEY_R) b |= 0x0020, triggers[1] = 255;
    if (held & KEY_L) b |= 0x0040, triggers[0] = 255;
    *buttons = b;
    // The Circle Pad reports about +-155 at its edge; a GameCube stick about +-100.
    circlePosition circle{};
    hidCircleRead(&circle);
    auto axis = [](int v) {
        v = v * 100 / 140;
        if (v > -12 && v < 12) v = 0;  // dead zone
        return static_cast<int8_t>(v > 105 ? 105 : v < -105 ? -105 : v);
    };
    sticks[0] = axis(circle.dx);
    sticks[1] = axis(circle.dy);
    if (held & KEY_CSTICK_RIGHT) sticks[2] = 90;
    if (held & KEY_CSTICK_LEFT) sticks[2] = -90;
    if (held & KEY_CSTICK_UP) sticks[3] = 90;
    if (held & KEY_CSTICK_DOWN) sticks[3] = -90;
    for (unsigned i = 0; i < g_playScriptCount; ++i) {
        const PlayScriptEvent& e = g_playScript[i];
        if (e.buttons == 0xFFFF) continue;  // a screenshot request (Brawl3dsFrameEnd), not a press
        if (g_frames >= e.start && g_frames < e.start + e.frames) {
            *buttons |= e.buttons;
            if (e.stickX) sticks[0] = e.stickX;
            if (e.stickY) sticks[1] = e.stickY;
        }
    }
    return true;
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
    Brawl3dsRendererEndFrame();  // the frame's PICA draws are submitted; the back end's staging is done with
    g_droppedDraws += Brawl3dsPicaTakeDropped();
    // The frame before each capture also logs its first batches to stderr.txt (decoded vertex, where it lands).
    if (!g_play && (g_frames == 699 || g_frames == 1999 || g_frames == 2872)) Brawl3dsPicaDebugNextFrame();
    if (g_play) {
        // play_script.txt lines with buttons FFFF: a screenshot of that game frame (shot_<frame>.bmp), written three
        // frames later (the copy is taken at the next frame's end).
        for (unsigned i = 0; i < g_playScriptCount; ++i) {
            const PlayScriptEvent& e = g_playScript[i];
            if (e.buttons != 0xFFFF) continue;
            if (g_frames == e.start) {
                char path[64];
                std::snprintf(path, sizeof(path), "sdmc:/3ds/brawl3ds/shot_%u.bmp", g_frames);
                Brawl3dsPicaCaptureNextFrame(path);
            } else if (g_frames == e.start + 3) {
                Brawl3dsPicaWriteCaptures();
            }
        }
    }
    if (!g_skipRender) {
        const double ms = TicksToMs(total);
        g_renderedFrameMs = g_renderedFrameMs == 0 ? ms : g_renderedFrameMs * 0.9 + ms * 0.1;
    }
    // Top-screen captures for checking the picture: the safety screen, the title screen, two match frames.
    // Frames around the last capture: their draw counts (does the HUD come every other frame?).
    if (!g_play && g_frames >= 3370 && g_frames <= 3376) Log("frame %u: %u GX draws\n", g_frames, g_frameDraws);
    g_frameDraws = 0;
    // Two captures in a row at the end: the HUD showed on the screen but not in single captures.
    // Testing aid: sdmc:/3ds/brawl3ds/stop_after.txt holding a capture frame ends the run once that capture is in
    // memory (two frames later), with its screenshots written: a title-screen check without the whole match.
    {
        static int stopAfter = -1;
        if (stopAfter == -1) {
            stopAfter = 0;
            if (std::FILE* f = std::fopen("sdmc:/3ds/brawl3ds/stop_after.txt", "r")) {
                unsigned n = 0;
                if (std::fscanf(f, "%u", &n) == 1) stopAfter = static_cast<int>(n);
                std::fclose(f);
                if (stopAfter > 0) Log("stop_after.txt: the run ends after the capture at frame %d\n", stopAfter);
            }
            if (std::FILE* f = std::fopen("sdmc:/3ds/brawl3ds/debug_flags.txt", "r")) {
                unsigned flags = 0;
                if (std::fscanf(f, "%u", &flags) == 1) {
                    if (Brawl3dsPicaSetDebugFlags) Brawl3dsPicaSetDebugFlags(flags);
                    Log("debug_flags.txt: renderer testing switches %u\n", flags);
                    g_renderEvery = (flags & 256) ? 4 : (flags & 128) ? 2 : 1;
                    g_brawl3dsGxTiming = (flags & 512) ? 1 : 0;  // the game thread's GX timers (headless.log)
                    g_audioOff = (flags & 1024) != 0;  // no sound (the DSP is not opened)
                    if (g_renderEvery > 1) Log("render frame skip: the match draws 1 game frame in %u\n", g_renderEvery);
                }
                std::fclose(f);
            }
        }
        if (stopAfter > 0 && g_frames == static_cast<uint32_t>(stopAfter) + 2) {
            Brawl3dsPicaWriteCaptures();
            FinishRun("stop_after.txt");
        }
    }
    if (!g_play &&
        (g_frames == 700 || g_frames == 2000 || g_frames == 2873 || g_frames == 3373 || g_frames == 3374)) {
        char path[64];
        std::snprintf(path, sizeof(path), "sdmc:/3ds/brawl3ds/shot_%u.bmp", g_frames);
        Brawl3dsPicaCaptureNextFrame(path);  // kept in memory, written with the results
        Log("screenshot: %s (renderer %s, %llu draws dropped so far)\n", path,
            Brawl3dsPicaActive() ? "active" : "not started", static_cast<unsigned long long>(g_droppedDraws));
    }
    {
        uint64_t displayListTicks = 0;
        Brawl3dsRendererTakeStats(&displayListTicks, &g_pipelineConfigs);
        if (g_scene == "scMelee") g_matchDisplayListTicks += displayListTicks;
    }
    // Track allocations from frame 10 (past boot) and report what is still held at frame 60: per-frame leaks.
    if (g_frames == 1) {
        // Where guest memory and the heap sit in the 3DS address space, to place addresses from crash reports and
        // Azahar's "unmapped" log lines.
        Log("memory map: MEM1 host %p, MEM2 host %p (+%u MiB), module homes host %p, heap %08lX-%08lX (sbrk %p), "
            "linear %08lX-%08lX\n",
            static_cast<void*>(Memory::GetPointer(Memory::kMem1CachedBase, 4)),
            static_cast<void*>(Memory::GetPointer(Memory::kMem2CachedBase, 4)),
            static_cast<unsigned>(Memory::kMem2Size >> 20),
            static_cast<void*>(Memory::GetPointer(Memory::kModuleHomesBase, 4)),
            static_cast<unsigned long>(__ctru_heap), static_cast<unsigned long>(__ctru_heap + __ctru_heap_size),
            sbrk(0), static_cast<unsigned long>(__ctru_linear_heap),
            static_cast<unsigned long>(__ctru_linear_heap + __ctru_linear_heap_size));
    }
    DetHashFrame();
    // Guest-thread stacks set aside while the heap still has room (the title movie's reader thread once found
    // no 256 KiB block and never ran): past the boot peak, so the disc index and the renderer's ring came first.
    if (g_frames == 2) Log("fiber stacks set aside: %d of 4 (256 KiB each)\n", Brawl3dsReserveFiberStacks(4));
    if (g_frames == 60) Brawl3dsPacksInit();
    if (g_frames == 3 && g_play) ExecProbe();
    if (g_frames == 10) Brawl3dsAllocTrackStart();
    if (g_frames == 60) Brawl3dsAllocTrackDump("sdmc:/3ds/brawl3ds/leaks.txt", g_frames);

    const std::string scene = SaveStates::SceneName();
    if (scene != g_scene) {
        WriteWindow();
        Log("-- scene %s -> %s at frame %u\n", g_scene.empty() ? "(none)" : g_scene.c_str(), scene.c_str(), g_frames);
        // The demo match is ~1,141 frames (it ends before the 1,800-frame cap): leaving scMelee ends the measurement.
        Brawl3dsPacksSceneChange(g_scene == "scMelee" ? 1 : 0);
        const bool matchEnded = !g_play && g_scene == "scMelee" && g_matchFrames > 0 && g_step == kStepMatch;
        g_scene = scene;
        if (g_scene == "scMelee") Brawl3dsPicaWriteStats(nullptr, 0);  // the match statistics start here
        if (matchEnded) {
            WriteMatchSummary();
            EnterStep(kStepResults);
            FinishRun("the demo match ended");
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
    if (!g_play && kSteps[g_step].pcFrames && g_step != kStepMatch) {
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
    if (++g_windowFrames >= (g_play ? 600u : g_scene == "scMelee" ? 60u : 10u)) WriteWindow();  // finer before the match: memory growth

    if (g_scene == "scMelee" && !g_play) {
        if (g_matchFrames == 0) {
            Brawl3dsProfilerSetRecording(true);  // profile.txt: the match only
            Brawl3dsZonesReset();                // and Aurora's zone timers
        }
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
            FinishRun("the measurement cap was reached");
        }
    }

    // Keep the console responsive (HOME menu, sleep, power button) and let START end the run.
    if ((g_frames & 7u) == 0) {
        if (!aptMainLoop()) FinishRun("the system asked the program to close");
        if (!g_play) {  // play mode reads the buttons at the game's own controller polls (Brawl3dsReadPad)
            hidScanInput();
            if (hidKeysDown() & KEY_START) {
                WriteWindow();
                WriteMatchSummary();
                FinishRun("START was pressed");
            }
        }
        DrawStatus();
    }
}

int main(int, char**)
{
    LightLock_Init(&g_drawLock);
    gfxInitDefault();
    consoleInit(GFX_TOP, nullptr);
    g_consoleActive = true;
    // Exit test: SELECT held at launch returns from main at once, through libctru's ordinary exit, before anything
    // of this build runs (no speedup, no threads, no runtime). Every real-hardware run so far has ended with the
    // HOME Menu's "An error has occurred" (hold POWER, restart): if this ends the same way, the cause is the
    // 178 MB memory mode or the exheader, not this build's exit path.
    bool selectHeld = false;
    for (int i = 0; i < 4 && !selectHeld; ++i) {  // HID may not have sampled the buttons yet on the first scan
        svcSleepThread(20000000LL);
        hidScanInput();
        selectHeld = (hidKeysHeld() & KEY_SELECT) != 0;
    }
    if (selectHeld) {
        std::printf("Exit test (SELECT held at launch):\nreturning from main through libctru's normal exit.\n");
        svcSleepThread(2000000000LL);
        gfxExit();
        return 0;
    }
    osSetSpeedupEnable(true);
    threadOnException(OnException, g_exceptionStack + sizeof(g_exceptionStack), WRITE_DATA_TO_HANDLER_STACK);

    mkdir("sdmc:/3ds", 0777);
    mkdir(kRoot, 0777);
    g_log = std::fopen("sdmc:/3ds/brawl3ds/headless.log", "w");
    std::remove("sdmc:/3ds/brawl3ds/profile.txt");
    std::remove("sdmc:/3ds/brawl3ds/crash.txt");  // a crash.txt left by an earlier run would look like this run's

    bool isNew = false;
    APT_CheckNew3DS(&isNew);
    {
        u64 programId = 0;
        struct stat playFile {};
        g_play = (R_SUCCEEDED(APT_GetProgramID(&programId)) && ((programId >> 8) & 0xFFFFFu) == 0xBB3D3u) ||
                 stat("sdmc:/3ds/brawl3ds/play.txt", &playFile) == 0;
    }
    if (g_play) {
        std::printf("Brawl3DS (playable test build)\n\n"
                    "Starting: about 20 seconds of loading\n"
                    "before the first picture.\n\n"
                    "Controls (GameCube controller, port 1):\n"
                    " A B X Y START D-pad as printed\n"
                    " L R, ZL or ZR = Z\n"
                    " Circle Pad = stick, C-Stick = C-stick\n\n"
                    "SELECT + D-pad up/down: draw fewer or\n"
                    " more pictures (fewer = faster game)\n"
                    "SELECT + START: quit\n\n"
                    "Log: sdmc:/3ds/brawl3ds/headless.log\n\n");
    }
    if (!g_play) std::printf("brawl3ds headless timing build\n\n");
    Log("brawl3ds %s build\nconsole: %s 3DS, APPLICATION region %lu MiB, free at start %lu MiB\n",
        g_play ? "playable" : "headless timing", isNew ? "New" : "Old",
        static_cast<unsigned long>(osGetMemRegionSize(MEMREGION_APPLICATION) >> 20),
        static_cast<unsigned long>(osGetMemRegionFree(MEMREGION_APPLICATION) >> 20));
    Log("heaps: regular %lu MiB, linear %lu MiB (linear free %lu KiB)\n",
        static_cast<unsigned long>(__ctru_heap_size >> 20), static_cast<unsigned long>(__ctru_linear_heap_size >> 20),
        static_cast<unsigned long>(linearSpaceFree() >> 10));
    if (!g_play) std::printf("This is a diagnostic build, not the game:\n"
                "no graphics, no input. It boots Brawl,\n"
                "lets the attract demo start a VS match\n"
                "and measures how fast the 3DS runs it.\n\n"
                "The bottom screen shows the step (1-%d),\n"
                "time spent and an estimate of time left.\n"
                "Steps 5 and 7 are Brawl's own idle\n"
                "timers: a player pressing A skips them.\n\n"
                "Stops by itself when the demo match\n"
                "ends (or press START). Results:\n"
                "  sdmc:/3ds/brawl3ds/headless.log\n", kStepCount);

    EnterStep(0);
    struct stat info {};
    if (stat("sdmc:/3ds/brawl3ds/game/sys/boot.bin", &info) != 0) {
        StopWithMessage("Game files missing", "copy the extracted disc to sdmc:/3ds/brawl3ds/game/ (sys/ and files/)");
    }
    // A first boot (no save) stops at Brawl's "create save data?" prompt, which this build cannot answer.
    if (!g_play && stat("sdmc:/3ds/brawl3ds/UserData/NAND/title/00010000/52534245/data/autosv0.bin", &info) != 0) {
        StopWithMessage("No Brawl save data",
                        "Brawl would stop at its 'create save data?'\nprompt and this build has no input.\n"
                        "Copy your save (the PC build's\nUserData/NAND/title/00010000/52534245/data)\n"
                        "to sdmc:/3ds/brawl3ds/UserData/NAND/title/\n00010000/52534245/data");
    }

    // The runtime indexes the whole disc folder at boot (5,314 files for Brawl), twice. A stat per file takes ~50 ms
    // on a real New 3DS (a std::filesystem walk took 4m38s here), so the patched runtime asks Brawl3dsListTree,
    // which reads each folder with FSDIR_Read (names and sizes, 64 per call). List the game folder now: it times
    // the card and fills the cache the runtime's two walks are answered from.
    EnterStep(1);
    {
        const uint64_t start = svcGetSystemTick();
        const auto* tree = Brawl3dsListTree("/3ds/brawl3ds/game");
        uint64_t bytes = 0;
        if (tree) {
            for (const Brawl3dsTreeFile& file : *tree) bytes += file.size;
        }
        Log("game folder listing (FSDIR_Read): %u files, %llu MiB in %.0f ms%s\n", tree ? unsigned(tree->size()) : 0u,
            static_cast<unsigned long long>(bytes >> 20), TicksToMs(svcGetSystemTick() - start),
            tree ? "" : ", could not open the folder");
        if (!tree || tree->empty()) {
            StopWithMessage("Game folder unreadable", "could not list sdmc:/3ds/brawl3ds/game");
        }
    }

    // Deterministic mode: guest clocks follow frames, so the attract demo is the same match on every run.
    setenv("NSMBW_DETERMINISTIC", "1", 1);
    // No sound output in this CPU timing build: SDL's dummy driver instead of the 3DS DSP (ndsp). The game still
    // mixes its audio. On a real New 3DS, opening the ndsp device during start-up raised a CPU exception
    // (registers pointing into ndspVars, 44100 Hz); that is for the audio phase, not this measurement.
    setenv("SDL_AUDIO_DRIVER", "dummy", 1);

    // From here on the runtime's own output goes to a file; the screen keeps the instructions above.
    std::freopen("sdmc:/3ds/brawl3ds/stdout.txt", "w", stdout);
    std::freopen("sdmc:/3ds/brawl3ds/stderr.txt", "w", stderr);
    g_consoleActive = false;
    {
        // The CIA's exheader starts the main thread at priority 16, the highest an application may use, so nothing
        // could preempt it: on real hardware the heartbeat (then created below it) never ran while start-up kept
        // the CPU busy, and a worker thread the main thread waits for could starve the same way. Drop the main
        // thread to the usual application priority and put the heartbeat clearly above it.
        s32 before = 0, after = 0;
        svcGetThreadPriority(&before, CUR_THREAD_HANDLE);
        svcSetThreadPriority(CUR_THREAD_HANDLE, 0x30);
        svcGetThreadPriority(&after, CUR_THREAD_HANDLE);
        const bool started = threadCreate(HeartbeatThread, nullptr, 64 * 1024, 0x18, -2, true) != nullptr;
        Log("thread priorities: main %ld -> %ld, heartbeat 24 (%s)\n", static_cast<long>(before),
            static_cast<long>(after), started ? "started" : "could not start");
    }

    // Guest RAM first, before the runtime's own allocations fragment the regular heap.
    EnterStep(2);
    Log("before reserving guest RAM: malloc in use %d KiB, linear free %lu KiB\n", mallinfo().uordblks >> 10,
        static_cast<unsigned long>(linearSpaceFree() >> 10));
    Brawl3dsReserveGuestMemory();
    Log("after reserving: malloc in use %d KiB, linear free %lu KiB\n", mallinfo().uordblks >> 10,
        static_cast<unsigned long>(linearSpaceFree() >> 10));

    EnterStep(3);
    g_mainGuestContextSlot = &g_currentCpuContext;  // this thread's instance, for the heartbeat's guest sampler
    if (!g_play) Brawl3dsProfilerStart(g_mainGuestContextSlot);  // the sampler costs game-thread time
    // The runtime's overlay code still calls ImGui on every presented frame (cursor auto-hide, ImGui::GetTime, ...)
    // even though the 3DS never renders an ImGui frame. Without a context those are null-pointer reads: Azahar
    // logs them as "unmapped Read32" and returns 0, a real New 3DS raises a data abort (settings_overlay::Draw
    // +0x120, ~30 s into step 4). An empty context makes them ordinary reads of the same zero values.
    ImGui::CreateContext();
    SDL_SetMainReady();
    char program[] = "sdmc:/3ds/brawl3ds/brawl3ds.3dsx";
    char* argv[] = {program, nullptr};
    const int result = RuntimeMain(1, argv);

    WriteWindow();
    if (g_step != kStepResults) WriteMatchSummary();  // already written when the match measurement completed
    Log("RuntimeMain returned %d after %u frames\n", result, g_frames);
    LogStepSummary();
    (void)result;
    ExitNow();
}
