#include "heightmap.h"
#include <fstream>

static constexpr uint32_t kMagic = 0x4D484352; // "RCHM"

bool Heightmap::Save(const std::string& path) const {
    std::ofstream f(path, std::ios::binary);
    if (!f) return false;
    f.write(reinterpret_cast<const char*>(&kMagic),     4);
    f.write(reinterpret_cast<const char*>(&W),          4);
    f.write(reinterpret_cast<const char*>(&H),          4);
    f.write(reinterpret_cast<const char*>(&cell_size),  4);
    f.write(reinterpret_cast<const char*>(heights.data()), heights.size() * 4);
    return true;
}

bool Heightmap::Load(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    uint32_t magic = 0;
    f.read(reinterpret_cast<char*>(&magic), 4);
    if (magic != kMagic) return false;
    f.read(reinterpret_cast<char*>(&W),         4);
    f.read(reinterpret_cast<char*>(&H),         4);
    f.read(reinterpret_cast<char*>(&cell_size), 4);
    heights.resize(W * H);
    f.read(reinterpret_cast<char*>(heights.data()), heights.size() * 4);
    return !!f;
}
