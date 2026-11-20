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

extern "C" {

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
