#include "heightmap.h"
#include <fstream>
#include <glad/glad.h>

static constexpr uint32_t kMagic = 0x4D484352; // "RCHM"

void Heightmap::InitGPU() {
    if (W == 0 || H == 0 || heights.empty()) return;
    if (!tex) glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R32F, W, H, 0, GL_RED, GL_FLOAT, heights.data());
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);
}

void Heightmap::UploadRegion(int x0, int z0, int x1, int z1) {
    if (!tex || W == 0) return;
    x0 = std::max(0, x0); z0 = std::max(0, z0);
    x1 = std::min(W - 1, x1); z1 = std::min(H - 1, z1);
    if (x0 > x1 || z0 > z1) return;
    int rw = x1 - x0 + 1, rh = z1 - z0 + 1;
    std::vector<float> tmp(rw * rh);
    for (int z = 0; z < rh; z++)
        for (int x = 0; x < rw; x++)
            tmp[z * rw + x] = Get(x0 + x, z0 + z);
    glBindTexture(GL_TEXTURE_2D, tex);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    glTexSubImage2D(GL_TEXTURE_2D, 0, x0, z0, rw, rh, GL_RED, GL_FLOAT, tmp.data());
    glBindTexture(GL_TEXTURE_2D, 0);
}

void Heightmap::DestroyGPU() {
    if (tex) { glDeleteTextures(1, &tex); tex = 0; }
}

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
