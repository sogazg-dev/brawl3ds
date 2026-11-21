// brawl3ds: fast SD folder-tree listing (fast_tree_3ds.cpp). 3DS only.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

struct Brawl3dsTreeFile {
    std::string relative;  // path below the root, '/' separated
    uint64_t size;
};

// Every regular file below rootPath ("/3ds/..." or "sdmc:/3ds/..."), or nullptr if the folder cannot be opened.
// Cached: the first call reads the card, later calls for the same tree or a folder inside it do not.
const std::vector<Brawl3dsTreeFile>* Brawl3dsListTree(const std::string& rootPath);
