#include "terrain_renderer.h"
#include <glm/glm.hpp>
#include <cmath>
#include <algorithm>

static constexpr int CV = TerrainRenderer::kVerts;
static constexpr int CC = TerrainRenderer::kCells;

void TerrainRenderer::Init(const Heightmap& hmap) {
    cx_ = (hmap.W + CC - 1) / CC;
    cz_ = (hmap.H + CC - 1) / CC;

    // Shared index buffer — same topology for every chunk
    std::vector<uint32_t> idx;
    idx.reserve(CC * CC * 6);
    for (int z = 0; z < CC; z++) {
        for (int x = 0; x < CC; x++) {
            uint32_t tl = z * CV + x;
            uint32_t tr = tl + 1;
            uint32_t bl = (z + 1) * CV + x;
            uint32_t br = bl + 1;
            idx.push_back(tl); idx.push_back(bl); idx.push_back(tr);
            idx.push_back(tr); idx.push_back(bl); idx.push_back(br);
        }
    }
    idx_count_ = (int)idx.size();

    glGenBuffers(1, &ebo_);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo_);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, idx.size() * 4, idx.data(), GL_STATIC_DRAW);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);

    chunks_.resize(cx_ * cz_);
    for (int z = 0; z < cz_; z++) {
        for (int x = 0; x < cx_; x++) {
            Chunk& c = chunks_[z * cx_ + x];
            c.cx = x; c.cz = z; c.dirty = true;

            glGenVertexArrays(1, &c.vao);
            glGenBuffers(1, &c.vbo);

            glBindVertexArray(c.vao);
            glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo_); // stored in VAO state
            glBindBuffer(GL_ARRAY_BUFFER, c.vbo);
            glBufferData(GL_ARRAY_BUFFER, CV * CV * 6 * sizeof(float), nullptr, GL_DYNAMIC_DRAW);

            // layout(location=0) vec3 aPos
            glEnableVertexAttribArray(0);
            glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void*)0);
            // layout(location=1) vec3 aNormal
            glEnableVertexAttribArray(1);
            glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void*)(3 * sizeof(float)));

            glBindVertexArray(0);
        }
    }
}

void TerrainRenderer::BuildChunk(Chunk& c, const Heightmap& hmap) {
    const int   baseX = c.cx * CC;
    const int   baseZ = c.cz * CC;
    const float cs    = hmap.cell_size;

    std::vector<float> verts;
    verts.reserve(CV * CV * 6);

    for (int z = 0; z < CV; z++) {
        for (int x = 0; x < CV; x++) {
            int   gx = baseX + x;
            int   gz = baseZ + z;
            float h  = hmap.Get(gx, gz);

            // Finite-difference normal — matches client terrain shader exactly
            float hl = hmap.Get(gx - 1, gz);
            float hr = hmap.Get(gx + 1, gz);
            float hd = hmap.Get(gx, gz - 1);
            float hu = hmap.Get(gx, gz + 1);
            glm::vec3 n = glm::normalize(glm::vec3(hl - hr, 2.f * cs, hd - hu));

            verts.push_back(gx * cs); verts.push_back(h);    verts.push_back(gz * cs);
            verts.push_back(n.x);     verts.push_back(n.y);  verts.push_back(n.z);
        }
    }

    glBindVertexArray(c.vao);
    glBindBuffer(GL_ARRAY_BUFFER, c.vbo);
    glBufferSubData(GL_ARRAY_BUFFER, 0, verts.size() * sizeof(float), verts.data());
    glBindVertexArray(0);

    c.dirty = false;
}

void TerrainRenderer::MarkDirtyAll() {
    for (Chunk& c : chunks_) c.dirty = true;
}

void TerrainRenderer::MarkDirtyRegion(float wx, float wz, float radius, const Heightmap& hmap) {
    float chunkWorld = CC * hmap.cell_size;
    int x0 = std::max(0,      (int)std::floor((wx - radius) / chunkWorld));
    int z0 = std::max(0,      (int)std::floor((wz - radius) / chunkWorld));
    int x1 = std::min(cx_-1,  (int)std::floor((wx + radius) / chunkWorld));
    int z1 = std::min(cz_-1,  (int)std::floor((wz + radius) / chunkWorld));
    for (int z = z0; z <= z1; z++)
        for (int x = x0; x <= x1; x++)
            chunks_[z * cx_ + x].dirty = true;
}

void TerrainRenderer::Update(const Heightmap& hmap) {
    for (Chunk& c : chunks_)
        if (c.dirty) BuildChunk(c, hmap);
}

void TerrainRenderer::Render() const {
    for (const Chunk& c : chunks_) {
        glBindVertexArray(c.vao);
        glDrawElements(GL_TRIANGLES, idx_count_, GL_UNSIGNED_INT, 0);
    }
    glBindVertexArray(0);
}

void TerrainRenderer::Destroy() {
    for (Chunk& c : chunks_) {
        if (c.vao) glDeleteVertexArrays(1, &c.vao);
        if (c.vbo) glDeleteBuffers(1, &c.vbo);
        c.vao = c.vbo = 0;
    }
    if (ebo_) { glDeleteBuffers(1, &ebo_); ebo_ = 0; }
    chunks_.clear();
    cx_ = cz_ = 0;
}
