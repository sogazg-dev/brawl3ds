// brawl3ds memory/CPU probe.
//
// Answers the questions that gate a Brawl port before any game code runs on the console:
//   1. How much memory does the system mode in this CIA's exheader really give us (124 MB / 178 MB)?
//   2. How much of it can be allocated as ordinary heap and as GPU-visible linear memory?
//   3. What clock / L2 state does the app get, and how fast are the float operations the recompiled
//      PowerPC code leans on (single vs double, and software fmaf for Gekko's fused ps_madd)?
//
// Results go to the top screen and to sdmc:/3ds/brawl3ds/probe.txt.
#include <3ds.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifndef PROBE_MODE_NAME
#define PROBE_MODE_NAME "unknown"
#endif

// Keep the linear heap small so almost all application memory lands in the regular heap;
// the probe measures both pools separately.
u32 __ctru_linear_heap_size = 16 * 1024 * 1024;

static FILE* g_out;

static void say(const char* fmt, ...)
{
    char line[256];
    va_list args;
    va_start(args, fmt);
    vsnprintf(line, sizeof(line), fmt, args);
    va_end(args);
    fputs(line, stdout);
    if (g_out) fputs(line, g_out);
}

static double ms_since(u64 start) { return (double)(svcGetSystemTick() - start) * 1000.0 / SYSCLOCK_ARM11; }

// Allocate 1 MiB blocks until the allocator refuses, touching each so the pages are really backed.
static unsigned probe_heap(void* (*alloc)(size_t), void (*release)(void*))
{
    enum { kBlock = 1024 * 1024, kMax = 512 };
    static void* blocks[kMax];
    unsigned n = 0;
    while (n < kMax && (blocks[n] = alloc(kBlock)) != NULL) {
        memset(blocks[n], (int)n, kBlock);
        ++n;
    }
    for (unsigned i = 0; i < n; ++i) release(blocks[i]);
    return n;
}

static void* linear_alloc_mb(size_t n) { return linearAlloc(n); }

// Float microbenchmarks. volatile sinks stop the compiler from deleting the loops.
static volatile float g_sinkF;
static volatile double g_sinkD;

static void bench(void)
{
    enum { kIters = 4000000 };
    float a = 1.0001f, b = 0.9999f, c = 0.5f;
    double da = 1.0001, db = 0.9999, dc = 0.5;

    u64 t = svcGetSystemTick();
    for (int i = 0; i < kIters; ++i) c = a * c + b;
    g_sinkF = c;
    const double single_madd = ms_since(t);

    t = svcGetSystemTick();
    for (int i = 0; i < kIters; ++i) dc = da * dc + db;
    g_sinkD = dc;
    const double double_madd = ms_since(t);

    c = 0.5f;
    t = svcGetSystemTick();
    for (int i = 0; i < kIters; ++i) c = fmaf(a, c, b);
    g_sinkF = c;
    const double fused_madd = ms_since(t);

    t = svcGetSystemTick();
    for (int i = 0; i < kIters; ++i) dc = dc / da + db;
    g_sinkD = dc;
    const double double_div = ms_since(t);

    const double ns = 1e6 / kIters;
    say("float a*b+c      %6.1f ns/op\n", single_madd * ns);
    say("double a*b+c     %6.1f ns/op\n", double_madd * ns);
    say("fmaf (software)  %6.1f ns/op\n", fused_madd * ns);
    say("double divide    %6.1f ns/op\n", double_div * ns);
}

int main(void)
{
    gfxInitDefault();
    consoleInit(GFX_TOP, NULL);
    osSetSpeedupEnable(true);

    mkdir("sdmc:/3ds", 0777);
    mkdir("sdmc:/3ds/brawl3ds", 0777);
    char path[64];
    snprintf(path, sizeof(path), "sdmc:/3ds/brawl3ds/probe-%s.txt", PROBE_MODE_NAME);
    g_out = fopen(path, "w");

    bool isNew = false;
    APT_CheckNew3DS(&isNew);
    say("brawl3ds probe, CIA mode: %s\n", PROBE_MODE_NAME);
    say("console: %s 3DS\n", isNew ? "New" : "Old");
    say("APPLICATION region: %lu MB\n", (unsigned long)(osGetMemRegionSize(MEMREGION_APPLICATION) >> 20));
    say("  in use at start:  %lu MB\n", (unsigned long)(osGetMemRegionUsed(MEMREGION_APPLICATION) >> 20));
    say("  free at start:    %lu MB\n", (unsigned long)(osGetMemRegionFree(MEMREGION_APPLICATION) >> 20));

    s64 clock = -1, l2 = -1;
    if (R_SUCCEEDED(svcGetSystemInfo(&clock, 0x10001, 0)) && R_SUCCEEDED(svcGetSystemInfo(&l2, 0x10001, 2)))
        say("CPU: %lld MHz, L2 %s (Luma)\n", clock, l2 ? "on" : "off");
    else
        say("CPU: speed query unavailable\n");

    say("allocating...\n");
    gfxFlushBuffers(); gfxSwapBuffers(); gspWaitForVBlank();
    const unsigned heapMb = probe_heap(malloc, free);
    const unsigned linearMb = probe_heap(linear_alloc_mb, linearFree);
    say("usable heap:   %u MB\n", heapMb);
    say("usable linear: %u MB\n", linearMb);
    say("total usable:  %u MB\n", heapMb + linearMb);

    say("float benchmarks...\n");
    gfxFlushBuffers(); gfxSwapBuffers(); gspWaitForVBlank();
    bench();

    if (g_out) {
        fclose(g_out);
        g_out = NULL;
        printf("\nsaved %s\n", path);
    }
    printf("press START to exit\n");
    while (aptMainLoop()) {
        hidScanInput();
        if (hidKeysDown() & KEY_START) break;
        gfxFlushBuffers();
        gfxSwapBuffers();
        gspWaitForVBlank();
    }
    gfxExit();
    return 0;
}
