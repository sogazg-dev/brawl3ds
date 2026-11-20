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

// rename() over an existing file: POSIX and std::filesystem::rename replace the destination, the 3DS SD driver
// (FSUSER_RenameFile) refuses. The runtime saves NAND files, settings and the SD image by writing a temp file and
// renaming it over the old one, so without this every save after the first silently failed (and Brawl took error
// paths the PC never runs). Linked with -Wl,--wrap=rename, which also covers libstdc++'s filesystem code.
extern "C" int __real_rename(const char* from, const char* to);
extern "C" int __wrap_rename(const char* from, const char* to)
{
    if (__real_rename(from, to) == 0) return 0;
    const int saved = errno;
    struct stat source {}, target {};
    if (stat(from, &source) == 0 && stat(to, &target) == 0 && !S_ISDIR(target.st_mode) && unlink(to) == 0) {
        return __real_rename(from, to);
    }
    errno = saved;
    return -1;
}
