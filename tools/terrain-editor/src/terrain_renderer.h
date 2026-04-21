#pragma once
#include "heightmap.h"
#include <glad/glad.h>
#include <glm/glm.hpp>
#include <vector>

class TerrainRenderer {
public:
    static constexpr int kCells = 64; // cells per chunk side
    static constexpr int kVerts = kCells + 1; // 65×65 vertices per chunk

    struct Chunk {
        GLuint vao = 0, vbo = 0;
        int    cx = 0, cz = 0;
        bool   dirty = true;
    };

    void Init(const Heightmap& hmap);
    void Destroy();

    void MarkDirtyAll();
    void MarkDirtyRegion(float wx, float wz, float radius, const Heightmap& hmap);
    void Update(const Heightmap& hmap);
    void Render() const;

private:
    std::vector<Chunk> chunks_;
    GLuint             ebo_ = 0;
    int                idx_count_ = 0;
    int                cx_ = 0, cz_ = 0;

    void BuildChunk(Chunk& c, const Heightmap& hmap);
};
