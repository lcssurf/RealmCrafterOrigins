#include "renderer/terrain/terrain.h"
#include <cstdio>
#include <cmath>

namespace rco::renderer {

static float ProceduralHeight(float x, float z) {
    float h = 0.f;
    h += std::sin(x * 0.05f) * std::cos(z * 0.04f) * 4.f;
    h += std::sin(x * 0.12f + 1.3f) * std::cos(z * 0.11f) * 2.f;
    h += std::sin(x * 0.31f) * std::cos(z * 0.28f + 0.7f) * 0.8f;
    h += std::sin(x * 0.70f + 2.1f) * std::cos(z * 0.65f) * 0.3f;
    return h;
}

bool Terrain::Init(const char* shader_dir, int gw, int gh) {
    grid_w_ = gw;
    grid_h_ = gh;

    char vert[256], frag[256];
    std::snprintf(vert, sizeof(vert), "%s/terrain.vert", shader_dir);
    std::snprintf(frag, sizeof(frag), "%s/terrain.frag", shader_dir);
    if (!shader_.Load(vert, frag)) return false;

    float ox = -(gw * kChunkSize) * 0.5f;
    float oz = -(gh * kChunkSize) * 0.5f;

    chunks_.reserve(gw * gh);
    for (int cz = 0; cz < gh; ++cz) {
        for (int cx = 0; cx < gw; ++cx) {
            auto ch = std::make_unique<TerrainChunk>();
            float wx = ox + cx * kChunkSize;
            float wz = oz + cz * kChunkSize;
            ch->Init(wx, wz, kCellSize);

            std::vector<float> h(TerrainChunk::kSize * TerrainChunk::kSize);
            for (int z = 0; z < TerrainChunk::kSize; ++z)
                for (int x = 0; x < TerrainChunk::kSize; ++x)
                    h[z * TerrainChunk::kSize + x] =
                        ProceduralHeight(wx + x * kCellSize, wz + z * kCellSize);
            ch->SetHeights(h);
            chunks_.push_back(std::move(ch));
        }
    }
    return true;
}

void Terrain::Render(const glm::mat4& view, const glm::mat4& proj,
                     const glm::vec3& cam_pos, const glm::vec3& sun_dir) {
    shader_.Use();
    shader_.SetMat4("uView",     view);
    shader_.SetMat4("uProj",     proj);
    shader_.SetMat4("uModel",    glm::mat4(1.f));
    shader_.SetVec3("uCamPos",   cam_pos);
    shader_.SetVec3("uSunDir",   glm::normalize(sun_dir));
    shader_.SetVec3("uSunColor", {1.0f, 0.95f, 0.80f});
    shader_.SetVec3("uAmbient",  {0.15f, 0.18f, 0.22f});
    for (auto& ch : chunks_) ch->Draw();
}

float Terrain::SampleHeight(float wx, float wz) const {
    return ProceduralHeight(wx, wz);
}

void Terrain::Destroy() { chunks_.clear(); }

} // namespace rco::renderer
