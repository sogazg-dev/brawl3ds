// Symbols the 3DS build needs that neither newlib/libctru nor the generated Aurora stubs provide.
//
//  * POSIX calls newlib on the 3DS lacks: the runtime only uses them for host features the 3DS doesn't have
//    (signal-based fault handling, the restart-into-netplay exec, a self-pipe), so they fail cleanly with ENOSYS.
//    fdatasync is used when saves are flushed: it maps to a real fsync.
//  * Aurora's SPM (Super Paper Mario) diagnostics globals, which the runtime's GX layer reads and writes.
//  * Two Aurora-internal GX functions the runtime declares locally (no public header to generate stubs from).
#include <cerrno>
#include <csignal>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <fcntl.h>

extern "C" {

// Large-allocation log (linked with -Wl,--wrap=malloc,--wrap=calloc,--wrap=realloc): every request of 64 KiB or
// more is appended to sdmc:/3ds/brawl3ds/bigalloc.txt with its size and caller, to see where the heap goes.
// Writes with a raw fd from a stack buffer and a re-entrancy guard, so the logger itself never allocates.
void* __real_malloc(size_t);
void* __real_calloc(size_t, size_t);
void* __real_realloc(void*, size_t);

static int g_bigAllocFd = -2;
static bool g_inBigAllocLog = false;
static unsigned long long g_bigAllocTotal = 0;

static void LogBigAlloc(const char* kind, size_t size, void* result, void* caller)
{
    if (size < 64u * 1024u || g_inBigAllocLog) return;
    g_inBigAllocLog = true;
    if (g_bigAllocFd == -2) g_bigAllocFd = open("sdmc:/3ds/brawl3ds/bigalloc.txt", O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (g_bigAllocFd >= 0) {
        g_bigAllocTotal += size;
        char line[128];
        const int n = std::snprintf(line, sizeof(line), "%s %8u KiB  total %8llu KiB  caller %08lx  %s\n", kind,
                                    static_cast<unsigned>(size >> 10), g_bigAllocTotal >> 10,
                                    static_cast<unsigned long>(reinterpret_cast<uintptr_t>(caller)),
                                    result ? "ok" : "FAILED");
        if (n > 0) write(g_bigAllocFd, line, static_cast<size_t>(n));
    }
    g_inBigAllocLog = false;
}

void* __wrap_malloc(size_t size)
{
    void* p = __real_malloc(size);
    LogBigAlloc("malloc ", size, p, __builtin_return_address(0));
    return p;
}

void* __wrap_calloc(size_t count, size_t size)
{
    void* p = __real_calloc(count, size);
    LogBigAlloc("calloc ", count * size, p, __builtin_return_address(0));
    return p;
}

void* __wrap_realloc(void* old, size_t size)
{
    void* p = __real_realloc(old, size);
    LogBigAlloc("realloc", size, p, __builtin_return_address(0));
    return p;
}

// operator new / new[] (unsigned int size on 32-bit ARM): logged here so the caller is the C++ code itself.
void* __real__Znwj(size_t);
void* __real__Znaj(size_t);

void* __wrap__Znwj(size_t size)
{
    // new -> malloc logs the same request again with operator new as the caller; this line names the real one.
    LogBigAlloc("new    ", size, reinterpret_cast<void*>(1), __builtin_return_address(0));
    return __real__Znwj(size);
}

void* __wrap__Znaj(size_t size)
{
    LogBigAlloc("new[]  ", size, reinterpret_cast<void*>(1), __builtin_return_address(0));
    return __real__Znaj(size);
}

int sigaction(int, const struct sigaction*, struct sigaction*)
{
    errno = ENOSYS;
    return -1;
}

int pipe(int*)
{
    errno = ENOSYS;
    return -1;
}

int execv(const char*, char* const*)
{
    errno = ENOSYS;
    return -1;
}

int fdatasync(int fd)
{
    return fsync(fd);
}

// The SD card's FAT file system has no Unix permissions. newlib reports chmod/fchmod as failures, which makes
// libstdc++'s std::filesystem::copy_file give up after creating an empty destination file (it sets the
// permissions before copying): report success instead.
int fchmod(int, mode_t)
{
    return 0;
}

int chmod(const char*, mode_t)
{
    return 0;
}

// Aurora command_processor's SPM bring-up diagnostics (all off).
int g_spmDrawDump = 0;
int g_spmCullOff = 0;
int g_spmSweepDump = 0;
unsigned g_spmDrawIdx = 0;
unsigned g_spmDrawLimit = 0;
unsigned g_spmAudit[8] = {};

// Aurora-internal GX entry points (runtime/src/hle/gx/gx_stubs.cpp, gx_init.cpp declare them locally).
void __GXSetSUTexRegs() {}
bool GXDisplayListIsEmpty() { return true; }
// PNG decoding for the PC settings overlay's button art; nothing to show on the 3DS timing build.
unsigned char* aurora_load_png_rgba(const char*, unsigned* width, unsigned* height)
{
    if (width) *width = 0;
    if (height) *height = 0;
    return nullptr;
}

} // extern "C"
