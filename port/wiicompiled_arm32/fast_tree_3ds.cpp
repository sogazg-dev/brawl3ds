// brawl3ds: list a folder tree on the SD card with the 3DS file system service directly.
//
// newlib's stat() on the 3DS opens the file, asks its size and closes it: three FS service calls plus a FAT
// directory search, about 50 ms per file on a real New 3DS. The runtime walked Brawl's 5,314 disc files that way
// at start-up, twice (the save key and the DVD index), which kept real hardware in "Starting the runtime" for
// over an hour. FSDIR_Read instead returns each entry's name, attributes and size, up to 64 per call, so the
// whole tree costs one directory read per folder. The result is kept, and a later request for a folder inside an
// already listed tree is answered from memory: the game folder is read from the card once.
#include <3ds.h>

#include <algorithm>
#include <map>
#include <string>
#include <vector>

#include "brawl3ds_fast_tree.h"

namespace {

std::string Normalize(std::string path)
{
    if (path.rfind("sdmc:", 0) == 0) path.erase(0, 5);
    std::replace(path.begin(), path.end(), '\\', '/');
    while (path.size() > 1 && path.back() == '/') path.pop_back();
    if (path.empty() || path[0] != '/') path.insert(path.begin(), '/');
    return path;
}

bool ListInto(FS_Archive sdmc, const std::string& absolute, const std::string& relative,
              std::vector<Brawl3dsTreeFile>& out)
{
    Handle dir = 0;
    if (R_FAILED(FSUSER_OpenDirectory(&dir, sdmc, fsMakePath(PATH_ASCII, absolute.c_str())))) return false;
    std::vector<std::string> subdirs;
    static FS_DirectoryEntry entries[64];  // only the main thread lists trees; keeps 35 KiB off the stack
    for (;;) {
        u32 count = 0;
        if (R_FAILED(FSDIR_Read(dir, &count, 64, entries)) || count == 0) break;
        for (u32 i = 0; i < count; ++i) {
            char name[0x106 * 3 + 1] = {};
            const ssize_t n = utf16_to_utf8(reinterpret_cast<uint8_t*>(name), entries[i].name, sizeof(name) - 1);
            if (n <= 0) continue;
            name[n] = 0;
            const std::string child = relative.empty() ? std::string(name) : relative + "/" + name;
            if (entries[i].attributes & FS_ATTRIBUTE_DIRECTORY) {
                subdirs.push_back(child);
            } else {
                out.push_back({child, entries[i].fileSize});
            }
        }
    }
    FSDIR_Close(dir);
    for (const std::string& sub : subdirs) ListInto(sdmc, absolute + "/" + sub.substr(sub.rfind('/') + 1), sub, out);
    return true;
}

std::map<std::string, std::vector<Brawl3dsTreeFile>> g_trees;  // normalized root -> files

} // namespace

const std::vector<Brawl3dsTreeFile>* Brawl3dsListTree(const std::string& rootPath)
{
    const std::string root = Normalize(rootPath);
    if (auto it = g_trees.find(root); it != g_trees.end()) return &it->second;

    // A folder inside a tree already listed: filter that list instead of reading the card again.
    for (const auto& [listed, files] : g_trees) {
        if (root.size() > listed.size() && root.compare(0, listed.size(), listed) == 0 && root[listed.size()] == '/') {
            const std::string prefix = root.substr(listed.size() + 1) + "/";
            std::vector<Brawl3dsTreeFile> sub;
            for (const Brawl3dsTreeFile& file : files) {
                if (file.relative.compare(0, prefix.size(), prefix) == 0) {
                    sub.push_back({file.relative.substr(prefix.size()), file.size});
                }
            }
            return &(g_trees[root] = std::move(sub));
        }
    }

    FS_Archive sdmc = 0;
    if (R_FAILED(FSUSER_OpenArchive(&sdmc, ARCHIVE_SDMC, fsMakePath(PATH_EMPTY, "")))) return nullptr;
    std::vector<Brawl3dsTreeFile> files;
    const bool ok = ListInto(sdmc, root, "", files);
    FSUSER_CloseArchive(sdmc);
    if (!ok) return nullptr;
    return &(g_trees[root] = std::move(files));
}
