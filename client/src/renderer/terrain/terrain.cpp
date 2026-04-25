#include "renderer/terrain/terrain.h"
#include "rco/renderer/pipeline.h"
#include <stb_image.h>
#include <cstdio>
#include <cmath>
#include <fstream>
#include <algorithm>
#include <cctype>
#include <filesystem>

namespace rco::renderer {

// ---------------------------------------------------------------------------
// Procedural fallback height
// ---------------------------------------------------------------------------
static float ProceduralHeight(float x, float z) {
    float h = 0.f;
    h += std::sin(x * 0.05f) * std::cos(z * 0.04f) * 4.f;
    h += std::sin(x * 0.12f + 1.3f) * std::cos(z * 0.11f) * 2.f;
    h += std::sin(x * 0.31f) * std::cos(z * 0.28f + 0.7f) * 0.8f;
    h += std::sin(x * 0.70f + 2.1f) * std::cos(z * 0.65f) * 0.3f;
    return h;
}

// ---------------------------------------------------------------------------
// Texture helpers
// ---------------------------------------------------------------------------
GLuint Terrain::MakeSolidTex(uint8_t r, uint8_t g, uint8_t b) {
    GLuint t;
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    const uint8_t px[4] = {r, g, b, 255};
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    return t;
}

static GLuint LoadTexInternal(const std::string& path, bool srgb) {
    int w, h, ch;
    stbi_set_flip_vertically_on_load(true);
    unsigned char* px = stbi_load(path.c_str(), &w, &h, &ch, 4);
    if (!px) return 0;
    GLuint tex;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    GLenum internal = srgb ? GL_SRGB8_ALPHA8 : GL_RGBA8;
    glTexImage2D(GL_TEXTURE_2D, 0, internal, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
    glGenerateMipmap(GL_TEXTURE_2D);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    stbi_image_free(px);
    return tex;
}

GLuint Terrain::LoadSRGBTex(const std::string& path)   { return LoadTexInternal(path, true); }
GLuint Terrain::LoadLinearTex(const std::string& path) { return LoadTexInternal(path, false); }

// ---------------------------------------------------------------------------
// Texture role detection (mirrors terrain-editor material.h)
// ---------------------------------------------------------------------------
enum class TexRole { Albedo, Normal, Roughness, AO, ORM, Unknown };

static TexRole GuessRole(const std::string& stem) {
    std::string s = stem;
    std::transform(s.begin(), s.end(), s.begin(), ::tolower);
    if (s.find("orm") != std::string::npos || s.find("arm") != std::string::npos)
        return TexRole::ORM;
    bool isDX = s.find("_dx") != std::string::npos || s.find("dx_") != std::string::npos;
    if (!isDX && (s.find("nor") != std::string::npos ||
                  s.find("nrm") != std::string::npos ||
                  s.find("normal") != std::string::npos))
        return TexRole::Normal;
    if (s.find("col") != std::string::npos || s.find("albedo") != std::string::npos ||
        s.find("diffuse") != std::string::npos || s.find("diff") != std::string::npos ||
        s.find("basecolor") != std::string::npos || s.find("base_color") != std::string::npos)
        return TexRole::Albedo;
    if (s.find("rough") != std::string::npos) return TexRole::Roughness;
    if (s == "ao" || s.find("_ao") != std::string::npos || s.find("ao_") != std::string::npos ||
        s.find("ambient") != std::string::npos || s.find("occlusion") != std::string::npos)
        return TexRole::AO;
    return TexRole::Unknown;
}

static bool IsImg(const std::filesystem::path& p) {
    auto e = p.extension().string();
    std::transform(e.begin(), e.end(), e.begin(), ::tolower);
    return e == ".png" || e == ".jpg" || e == ".jpeg" || e == ".tga";
}

// ---------------------------------------------------------------------------
// Init
// ---------------------------------------------------------------------------
bool Terrain::Init(int gw, int gh) {
    grid_w_ = gw;
    grid_h_ = gh;

    def_normal_    = MakeSolidTex(128, 128, 255);
    def_roughness_ = MakeSolidTex(180, 180, 180);

    GenerateProcedural();
    return true;
}

// ---------------------------------------------------------------------------
// LoadFromEditor
// ---------------------------------------------------------------------------
bool Terrain::LoadFromEditor(const std::string& area_name) {
    namespace fs = std::filesystem;

    std::string base = "data/areas/" + area_name + "/";
    std::string hmap_path  = base + "heightmap.bin";
    std::string smap_path  = base + "splatmap.bin";
    std::string mats_path  = base + "materials.txt";

    std::fprintf(stderr, "[terrain] LoadFromEditor: '%s' -> %s\n",
                 area_name.c_str(), hmap_path.c_str());

    // --- Load heightmap ---
    {
        std::ifstream f(hmap_path, std::ios::binary);
        if (!f) {
            std::fprintf(stderr, "[terrain] heightmap not found: %s\n", hmap_path.c_str());
            return false;
        }

        uint32_t magic = 0;
        f.read(reinterpret_cast<char*>(&magic), 4);
        if (magic != 0x4D484352) {
            std::fprintf(stderr, "[terrain] bad magic in %s\n", hmap_path.c_str());
            return false;
        }

        int lw, lh;
        float cs;
        f.read(reinterpret_cast<char*>(&lw), 4);
        f.read(reinterpret_cast<char*>(&lh), 4);
        f.read(reinterpret_cast<char*>(&cs), 4);

        std::vector<float> tmp(lw * lh);
        f.read(reinterpret_cast<char*>(tmp.data()), tmp.size() * 4);
        if (!f) return false;

        hmap_data_    = std::move(tmp);
        hmap_w_       = lw;
        hmap_h_       = lh;
        hmap_cell_    = cs;
        has_hmap_     = true;
        std::fprintf(stderr, "[terrain] heightmap loaded: %dx%d cells, cell=%.1f\n", lw, lh, cs);

        // Each chunk covers (kSize-1) cells, so N chunks need N*(kSize-1)+1 verts.
        const int stride = TerrainChunk::kSize - 1;
        int cw = std::max(1, (lw - 1) / stride);
        int ch = std::max(1, (lh - 1) / stride);
        grid_w_ = cw;
        grid_h_ = ch;

        // World coords are 0-indexed to match the GUE Zone editor and the
        // npc_spawns / scenery tables (which store positions in [0, W*cs]).
        // Previously this was centered around the origin, causing every
        // placed object to render off-map on the client.
        hmap_origin_x_ = 0.f;
        hmap_origin_z_ = 0.f;
        hmap_size_x_   = (lw - 1) * cs;
        hmap_size_z_   = (lh - 1) * cs;
    }

    // Rebuild chunks with loaded heights
    RebuildChunksFromHmap();

    // --- Load splatmap ---
    {
        std::ifstream f(smap_path, std::ios::binary);
        if (f) {
            uint32_t magic = 0;
            f.read(reinterpret_cast<char*>(&magic), 4);
            if (magic == 0x4D505352) {
                int sw, sh;
                f.read(reinterpret_cast<char*>(&sw), 4);
                f.read(reinterpret_cast<char*>(&sh), 4);
                std::vector<float> sdata(sw * sh * 4);
                f.read(reinterpret_cast<char*>(sdata.data()), sdata.size() * 4);
                if (f) {
                    if (splatmap_tex_ == 0) glGenTextures(1, &splatmap_tex_);
                    glBindTexture(GL_TEXTURE_2D, splatmap_tex_);
                    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32F, sw, sh, 0,
                                 GL_RGBA, GL_FLOAT, sdata.data());
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
                }
            }
        }
    }

    // --- Load materials ---
    UnloadMaterials();
    {
        std::ifstream f(mats_path);
        if (f) {
            std::string line;
            while (std::getline(f, line) && (int)materials_.size() < 4) {
                line.erase(0, line.find_first_not_of(" \t\r\n"));
                line.erase(line.find_last_not_of(" \t\r\n") + 1);
                if (line.empty()) continue;

                // Format: "name [tiling]"
                std::string mat_name = line;
                float tiling = 4.f;
                auto sp = line.find(' ');
                if (sp != std::string::npos) {
                    mat_name = line.substr(0, sp);
                    try { tiling = std::stof(line.substr(sp + 1)); } catch (...) {}
                }

                std::string mat_dir = "data/terrain/materials/" + mat_name;
                if (!fs::exists(mat_dir) || !fs::is_directory(mat_dir)) {
                    std::fprintf(stderr, "[terrain] material dir not found: %s\n", mat_dir.c_str());
                    // Push placeholder so slot indices match
                    MatTex m;
                    m.albedo    = MakeSolidTex(200, 200, 200);
                    m.normal    = def_normal_;
                    m.roughness = def_roughness_;
                    materials_.push_back(m);
                    continue;
                }

                MatTex m;
                m.tiling    = tiling;
                m.normal    = def_normal_;
                m.roughness = def_roughness_;

                for (auto& entry : fs::directory_iterator(mat_dir)) {
                    if (!entry.is_regular_file() || !IsImg(entry.path())) continue;
                    TexRole role = GuessRole(entry.path().stem().string());
                    switch (role) {
                    case TexRole::Albedo:
                        if (!m.albedo) m.albedo = LoadSRGBTex(entry.path().string());
                        break;
                    case TexRole::Normal:
                        if (m.normal == def_normal_)
                            m.normal = LoadLinearTex(entry.path().string());
                        break;
                    case TexRole::Roughness:
                    case TexRole::ORM:
                        if (m.roughness == def_roughness_)
                            m.roughness = LoadLinearTex(entry.path().string());
                        break;
                    default: break;
                    }
                }

                if (!m.albedo) m.albedo = MakeSolidTex(200, 200, 200);
                materials_.push_back(m);
            }
            has_materials_ = !materials_.empty();
        }
    }

    return true;
}

// ---------------------------------------------------------------------------
// RebuildChunksFromHmap
// ---------------------------------------------------------------------------
void Terrain::RebuildChunksFromHmap() {
    chunks_.clear();
    chunks_.reserve(grid_w_ * grid_h_);

    for (int cz = 0; cz < grid_h_; ++cz) {
        for (int cx = 0; cx < grid_w_; ++cx) {
            auto ch = std::make_unique<TerrainChunk>();
            float wx = hmap_origin_x_ + cx * kChunkSize;
            float wz = hmap_origin_z_ + cz * kChunkSize;
            ch->Init(wx, wz, hmap_cell_);

            // Sample from loaded heightmap.
            // stride = kSize-1 so adjacent chunks share their border vertex,
            // eliminating the one-cell gap that caused visible seams.
            const int N      = TerrainChunk::kSize;
            const int stride = N - 1;
            std::vector<float> h(N * N);
            for (int z = 0; z < N; ++z) {
                for (int x = 0; x < N; ++x) {
                    int gx = cx * stride + x;
                    int gz = cz * stride + z;
                    float val = 0.f;
                    if (gx < hmap_w_ && gz < hmap_h_)
                        val = hmap_data_[gz * hmap_w_ + gx];
                    h[z * N + x] = val;
                }
            }
            ch->SetHeights(h);
            chunks_.push_back(std::move(ch));
        }
    }
}

// ---------------------------------------------------------------------------
// GenerateProcedural
// ---------------------------------------------------------------------------
void Terrain::GenerateProcedural() {
    has_hmap_ = false;
    chunks_.clear();

    float ox = -(grid_w_ * kChunkSize) * 0.5f;
    float oz = -(grid_h_ * kChunkSize) * 0.5f;

    chunks_.reserve(grid_w_ * grid_h_);
    for (int cz = 0; cz < grid_h_; ++cz) {
        for (int cx = 0; cx < grid_w_; ++cx) {
            auto ch = std::make_unique<TerrainChunk>();
            // kChunkSize = (kSize-1)*kCellSize → adjacent chunks share border vertex
            float wx = ox + cx * kChunkSize;
            float wz = oz + cz * kChunkSize;
            ch->Init(wx, wz, kCellSize);

            const int N = TerrainChunk::kSize;
            std::vector<float> h(N * N);
            for (int z = 0; z < N; ++z)
                for (int x = 0; x < N; ++x)
                    h[z * N + x] = ProceduralHeight(wx + x * kCellSize, wz + z * kCellSize);
            ch->SetHeights(h);
            chunks_.push_back(std::move(ch));
        }
    }
}

// ---------------------------------------------------------------------------
// Render
// ---------------------------------------------------------------------------
void Terrain::Submit(Pipeline& pipeline) const {
    TerrainChunkSubmission base{};
    for (int i = 0; i < 4; ++i) {
        if (i < (int)materials_.size()) {
            base.mat_albedo[i]    = materials_[i].albedo;
            base.mat_normal[i]    = materials_[i].normal    ? materials_[i].normal    : def_normal_;
            base.mat_roughness[i] = materials_[i].roughness ? materials_[i].roughness : def_roughness_;
        } else {
            base.mat_albedo[i]    = 0;
            base.mat_normal[i]    = def_normal_;
            base.mat_roughness[i] = def_roughness_;
        }
    }
    base.tiling         = materials_.empty() ? 4.0f : materials_[0].tiling;
    base.splatmap       = splatmap_tex_;
    base.terrain_origin = { hmap_origin_x_, hmap_origin_z_ };
    base.terrain_size   = { hmap_size_x_,   hmap_size_z_   };

    for (const auto& ch : chunks_) {
        TerrainChunkSubmission c = base;
        c.vao         = ch->vao();
        c.vbo         = ch->vbo();
        c.ebo         = ch->ebo();
        c.index_count = ch->idx_count();
        c.model       = glm::mat4(1.0f);
        pipeline.SubmitTerrainChunk(c);
    }
}

// ---------------------------------------------------------------------------
// SampleHeight
// ---------------------------------------------------------------------------
float Terrain::SampleHeight(float wx, float wz) const {
    if (!has_hmap_) return ProceduralHeight(wx, wz);

    // Map world position to heightmap grid coords
    float lx = (wx - hmap_origin_x_) / hmap_cell_;
    float lz = (wz - hmap_origin_z_) / hmap_cell_;

    int x0 = static_cast<int>(std::floor(lx));
    int z0 = static_cast<int>(std::floor(lz));
    float fx = lx - x0;
    float fz = lz - z0;

    auto safe = [&](int x, int z) -> float {
        if (x < 0) x = 0; if (x >= hmap_w_) x = hmap_w_ - 1;
        if (z < 0) z = 0; if (z >= hmap_h_) z = hmap_h_ - 1;
        return hmap_data_[z * hmap_w_ + x];
    };

    float h00 = safe(x0, z0);
    float h10 = safe(x0 + 1, z0);
    float h01 = safe(x0, z0 + 1);
    float h11 = safe(x0 + 1, z0 + 1);

    return h00 * (1 - fx) * (1 - fz)
         + h10 * fx       * (1 - fz)
         + h01 * (1 - fx) * fz
         + h11 * fx       * fz;
}

// ---------------------------------------------------------------------------
// UnloadMaterials
// ---------------------------------------------------------------------------
void Terrain::UnloadMaterials() {
    for (auto& m : materials_) {
        auto del = [](GLuint t){ if (t) glDeleteTextures(1, &t); };
        del(m.albedo);
        if (m.normal    != def_normal_)    del(m.normal);
        if (m.roughness != def_roughness_) del(m.roughness);
    }
    materials_.clear();
    has_materials_ = false;
}

// ---------------------------------------------------------------------------
// Destroy
// ---------------------------------------------------------------------------
void Terrain::Destroy() {
    chunks_.clear();
    UnloadMaterials();
    if (def_normal_)    { glDeleteTextures(1, &def_normal_);    def_normal_    = 0; }
    if (def_roughness_) { glDeleteTextures(1, &def_roughness_); def_roughness_ = 0; }
    if (splatmap_tex_)  { glDeleteTextures(1, &splatmap_tex_);  splatmap_tex_  = 0; }
}

} // namespace rco::renderer
