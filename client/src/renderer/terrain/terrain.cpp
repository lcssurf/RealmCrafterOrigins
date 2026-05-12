#include "renderer/terrain/terrain.h"
#include "rco/renderer/pipeline.h"
#include <stb_image.h>
#include <glm/gtc/type_ptr.hpp>
#include <cstdio>
#include <cmath>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <cctype>
#include <filesystem>

namespace rco::renderer {

// ---------------------------------------------------------------------------
// Collision data — load from coldata.bin
// ---------------------------------------------------------------------------

ColData LoadColData(const std::string& area_name) {
    char path[512];
    std::snprintf(path, sizeof(path), "data/areas/%s/coldata.bin", area_name.c_str());
    FILE* f = std::fopen(path, "rb");
    if (!f) return {};

    auto r32 = [&](uint32_t& v) { return std::fread(&v, 4, 1, f) == 1; };
    auto rf  = [&](float&    v) { return std::fread(&v, 4, 1, f) == 1; };

    uint32_t magic, version;
    if (!r32(magic) || magic != 0x444C4F43u) { std::fclose(f); return {}; }
    if (!r32(version) || version > 2)        { std::fclose(f); return {}; }

    ColData out;
    uint32_t n;
    if (!r32(n)) { std::fclose(f); return {}; }
    out.boxes.reserve(n);
    for (uint32_t i = 0; i < n; ++i) {
        ColBox b;
        if (!rf(b.pos.x)||!rf(b.pos.y)||!rf(b.pos.z)||
            !rf(b.half.x)||!rf(b.half.y)||!rf(b.half.z)) break;
        out.boxes.push_back(b);
    }
    if (!r32(n)) { std::fclose(f); out.loaded = true; return out; }
    out.spheres.reserve(n);
    for (uint32_t i = 0; i < n; ++i) {
        ColSphere s;
        if (!rf(s.pos.x)||!rf(s.pos.y)||!rf(s.pos.z)||!rf(s.radius)) break;
        out.spheres.push_back(s);
    }
    if (version >= 2) {
        if (!r32(n)) { std::fclose(f); out.loaded = true; return out; }
        out.tris.reserve(n);
        for (uint32_t i = 0; i < n; ++i) {
            ColTri t;
            bool ok = rf(t.v[0].x)&&rf(t.v[0].y)&&rf(t.v[0].z)
                    &&rf(t.v[1].x)&&rf(t.v[1].y)&&rf(t.v[1].z)
                    &&rf(t.v[2].x)&&rf(t.v[2].y)&&rf(t.v[2].z);
            if (!ok) break;
            out.tris.push_back(t);
        }
    }
    std::fclose(f);
    out.loaded = true;
    return out;
}

// ---------------------------------------------------------------------------
// Collision resolution — push player out of boxes and spheres
// ---------------------------------------------------------------------------

// Closest point on a 2D line segment to point p (Ericson).
static glm::vec2 ClosestPtSeg2D(glm::vec2 p, glm::vec2 a, glm::vec2 b) {
    glm::vec2 ab = b - a, ap = p - a;
    float len2 = glm::dot(ab, ab);
    if (len2 < 1e-10f) return a;
    float t = std::max(0.f, std::min(1.f, glm::dot(ap, ab) / len2));
    return a + t * ab;
}

// Closest point on 2D triangle (a,b,c) to point p — Ericson barycentric method.
// Handles degenerate (collinear) triangles gracefully.
static glm::vec2 ClosestPtTri2D(glm::vec2 p, glm::vec2 a, glm::vec2 b, glm::vec2 c) {
    glm::vec2 ab = b-a, ac = c-a, ap = p-a;
    float d1 = glm::dot(ab,ap), d2 = glm::dot(ac,ap);
    if (d1 <= 0.f && d2 <= 0.f) return a;
    glm::vec2 bp = p-b;
    float d3 = glm::dot(ab,bp), d4 = glm::dot(ac,bp);
    if (d3 >= 0.f && d4 <= d3) return b;
    float vc = d1*d4 - d3*d2;
    if (vc <= 0.f && d1 >= 0.f && d3 <= 0.f) {
        float v = d1/(d1-d3); return a + v*ab;
    }
    glm::vec2 cp = p-c;
    float d5 = glm::dot(ab,cp), d6 = glm::dot(ac,cp);
    if (d6 >= 0.f && d5 <= d6) return c;
    float vb = d5*d2 - d1*d6;
    if (vb <= 0.f && d2 >= 0.f && d6 <= 0.f) {
        float w = d2/(d2-d6); return a + w*ac;
    }
    float va = d3*d6 - d5*d4;
    if (va <= 0.f && (d4-d3) >= 0.f && (d5-d6) >= 0.f) {
        float w = (d4-d3)/((d4-d3)+(d5-d6)); return b + w*(c-b);
    }
    float denom = va+vb+vc;
    if (std::abs(denom) < 1e-10f) {
        // Degenerate triangle — fall back to closest edge
        glm::vec2 p1 = ClosestPtSeg2D(p,a,b);
        glm::vec2 p2 = ClosestPtSeg2D(p,b,c);
        glm::vec2 p3 = ClosestPtSeg2D(p,c,a);
        float l1=glm::length(p-p1), l2=glm::length(p-p2), l3=glm::length(p-p3);
        if (l1<=l2&&l1<=l3) return p1;
        return l2<=l3 ? p2 : p3;
    }
    float inv = 1.f/denom, v = vb*inv, w = vc*inv;
    return a + v*ab + w*ac;
}

void ColData::Resolve(float& px, float pz_in, float py, float& out_pz) const {
    constexpr float R = 0.45f;   // player capsule radius
    constexpr float H = 1.8f;    // player capsule height
    float pz = pz_in;
    float pYmin = py, pYmax = py + H;

    for (int iter = 0; iter < 3; ++iter) {
        for (const auto& b : boxes) {
            if (pYmax < b.pos.y - b.half.y || pYmin > b.pos.y + b.half.y) continue;
            float cx = std::max(b.pos.x - b.half.x, std::min(px, b.pos.x + b.half.x));
            float cz = std::max(b.pos.z - b.half.z, std::min(pz, b.pos.z + b.half.z));
            float dx = px - cx, dz = pz - cz;
            float d2 = dx*dx + dz*dz;
            if (d2 < R * R) {
                if (d2 < 1e-7f) { dx = 1.f; dz = 0.f; d2 = 1.f; }
                float d = std::sqrt(d2);
                float push = R - d;
                px += (dx / d) * push;
                pz += (dz / d) * push;
            }
        }
        for (const auto& s : spheres) {
            if (pYmax < s.pos.y - s.radius || pYmin > s.pos.y + s.radius) continue;
            float dx = px - s.pos.x, dz = pz - s.pos.z;
            float d2 = dx*dx + dz*dz;
            float minD = R + s.radius;
            if (d2 < minD * minD) {
                if (d2 < 1e-7f) { dx = 1.f; dz = 0.f; d2 = 1.f; }
                float d = std::sqrt(d2);
                px += (dx / d) * (minD - d);
                pz += (dz / d) * (minD - d);
            }
        }
        for (const auto& tri : tris) {
            float tYmin = std::min({tri.v[0].y, tri.v[1].y, tri.v[2].y});
            float tYmax = std::max({tri.v[0].y, tri.v[1].y, tri.v[2].y});
            if (pYmax < tYmin || pYmin > tYmax) continue;
            glm::vec2 a{tri.v[0].x, tri.v[0].z};
            glm::vec2 b{tri.v[1].x, tri.v[1].z};
            glm::vec2 c{tri.v[2].x, tri.v[2].z};
            glm::vec2 pp{px, pz};
            glm::vec2 closest = ClosestPtTri2D(pp, a, b, c);
            glm::vec2 diff = pp - closest;
            float d2 = glm::dot(diff, diff);
            if (d2 < R * R) {
                if (d2 < 1e-7f) { diff = {1.f, 0.f}; d2 = 1.f; }
                float d = std::sqrt(d2);
                px += (diff.x / d) * (R - d);
                pz += (diff.y / d) * (R - d);
            }
        }
    }
    out_pz = pz;
}

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
enum class TexRole { Albedo, Normal, Roughness, AO, Height, ORM, Unknown };

static TexRole GuessRole(const std::string& stem) {
    std::string s = stem;
    std::transform(s.begin(), s.end(), s.begin(), ::tolower);

    // Normal must be tested BEFORE ORM/ARM, otherwise "_normal-ogl" matches
    // the "orm" substring inside "n[orm]al" and gets miscategorised.
    bool isDX = s.find("_dx") != std::string::npos || s.find("dx_") != std::string::npos;
    if (!isDX && (s.find("nor") != std::string::npos ||
                  s.find("nrm") != std::string::npos ||
                  s.find("normal") != std::string::npos))
        return TexRole::Normal;

    // ORM/ARM must be matched on word-ish boundaries to avoid the "normal"
    // collision above. We require a separator (_/-/.) before or after.
    auto isMatchedToken = [&](const std::string& tok) {
        size_t pos = 0;
        while ((pos = s.find(tok, pos)) != std::string::npos) {
            bool leftOk  = (pos == 0) || s[pos-1] == '_' || s[pos-1] == '-' || s[pos-1] == '.';
            size_t end   = pos + tok.size();
            bool rightOk = (end == s.size()) || s[end] == '_' || s[end] == '-' || s[end] == '.';
            if (leftOk && rightOk) return true;
            pos = end;
        }
        return false;
    };
    if (isMatchedToken("orm") || isMatchedToken("arm"))
        return TexRole::ORM;
    if (s.find("col") != std::string::npos || s.find("albedo") != std::string::npos ||
        s.find("diffuse") != std::string::npos || s.find("diff") != std::string::npos ||
        s.find("basecolor") != std::string::npos || s.find("base_color") != std::string::npos)
        return TexRole::Albedo;
    if (s.find("rough") != std::string::npos) return TexRole::Roughness;
    if (s == "ao" || s.find("_ao") != std::string::npos || s.find("ao_") != std::string::npos ||
        s.find("ambient") != std::string::npos || s.find("occlusion") != std::string::npos)
        return TexRole::AO;
    if (s.find("height") != std::string::npos || s.find("disp") != std::string::npos ||
        s.find("bump") != std::string::npos)
        return TexRole::Height;
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
    def_ao_        = MakeSolidTex(255, 255, 255);   // no occlusion
    def_height_    = MakeSolidTex(128, 128, 128);   // mid-height (neutral for height-blend)
    def_macro_     = MakeSolidTex(128, 128, 128);   // 0.5 gray = no overlay change

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

    // Upload heightmap as R32F texture (read by vertex shader)
    {
        if (!hmap_tex_) glGenTextures(1, &hmap_tex_);
        glBindTexture(GL_TEXTURE_2D, hmap_tex_);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_R32F, hmap_w_, hmap_h_, 0,
                     GL_RED, GL_FLOAT, hmap_data_.data());
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glBindTexture(GL_TEXTURE_2D, 0);
    }

    // Rebuild chunks (static XZ grid — no heights in VBO)
    RebuildChunksFromHmap();

    // --- Load splatmap ---
    {
        std::ifstream f(smap_path, std::ios::binary);
        if (f) {
            uint32_t magic = 0;
            f.read(reinterpret_cast<char*>(&magic), 4);
            int sw, sh;
            f.read(reinterpret_cast<char*>(&sw), 4);
            f.read(reinterpret_cast<char*>(&sh), 4);

            std::vector<uint8_t> sdata;
            bool ok = false;

            if (magic == 0x32505352) {
                // RSP2 — native uint8 format
                sdata.resize((size_t)sw * sh * 4);
                f.read(reinterpret_cast<char*>(sdata.data()), (std::streamsize)sdata.size());
                ok = (bool)f;
            } else if (magic == 0x4D505352) {
                // RSPM — legacy float format; convert on load
                std::vector<float> ftmp((size_t)sw * sh * 4);
                f.read(reinterpret_cast<char*>(ftmp.data()), (std::streamsize)(ftmp.size() * 4));
                if (f) {
                    sdata.resize(ftmp.size());
                    for (size_t i = 0; i < ftmp.size(); i++)
                        sdata[i] = static_cast<uint8_t>(std::clamp(ftmp[i] * 255.f + 0.5f, 0.f, 255.f));
                    ok = true;
                }
            }

            if (ok) {
                if (splatmap_tex_ == 0) glGenTextures(1, &splatmap_tex_);
                glBindTexture(GL_TEXTURE_2D, splatmap_tex_);
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, sw, sh, 0,
                             GL_RGBA, GL_UNSIGNED_BYTE, sdata.data());
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            }
        }
    }

    // --- Load macro variation ---
    {
        std::string macro_path = base + "macro.png";
        int mw, mh, mch;
        stbi_set_flip_vertically_on_load(false);
        unsigned char* px = stbi_load(macro_path.c_str(), &mw, &mh, &mch, 1);
        if (px) {
            if (macro_tex_) glDeleteTextures(1, &macro_tex_);
            glGenTextures(1, &macro_tex_);
            glBindTexture(GL_TEXTURE_2D, macro_tex_);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, mw, mh, 0, GL_RED, GL_UNSIGNED_BYTE, px);
            glGenerateMipmap(GL_TEXTURE_2D);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            macro_strength_ = 0.3f;
            stbi_image_free(px);
            std::fprintf(stderr, "[terrain] macro variation loaded: %dx%d\n", mw, mh);
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

                // Two formats:
                //   Old: "foldername [tiling]"   — first token is a non-numeric name
                //   New: "id tiling albedo normal orm"  — first token is a digit (media_materials id)
                std::istringstream is(line);
                std::string first;
                float tiling = 4.f;
                if (!(is >> first)) continue;
                is >> tiling;

                bool isNewFormat = !first.empty() && std::isdigit((unsigned char)first[0]);

                if (isNewFormat) {
                    // Extended format written by GUE — direct texture paths relative to dist/client/
                    // tokens: albedo normal orm normal_strength
                    std::string albedo_rel, normal_rel, orm_rel, ns_str;
                    is >> albedo_rel >> normal_rel >> orm_rel >> ns_str;

                    MatTex m;
                    m.tiling          = tiling;
                    m.normal_strength = ns_str.empty() ? 2.5f : [&]{
                        try { return std::stof(ns_str); } catch (...) { return 2.5f; }
                    }();
                    m.normal    = def_normal_;
                    m.roughness = def_roughness_;
                    m.ao        = def_ao_;
                    m.height    = def_height_;

                    if (!albedo_rel.empty() && albedo_rel != "-")
                        m.albedo = LoadSRGBTex(albedo_rel);
                    if (!normal_rel.empty() && normal_rel != "-")
                        m.normal = LoadLinearTex(normal_rel);
                    if (!orm_rel.empty() && orm_rel != "-") {
                        m.roughness = LoadLinearTex(orm_rel);
                        m.ao = m.roughness;  // same ORM: R=AO, G=Roughness, B=Metallic
                    }

                    if (!m.albedo) m.albedo = MakeSolidTex(200, 200, 200);
                    materials_.push_back(m);
                    continue;
                }

                // Old format: resolve by folder under data/terrain/materials/<name>/
                std::string mat_dir = "data/terrain/materials/" + first;
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
                m.ao        = def_ao_;
                m.height    = def_height_;

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
                    case TexRole::AO:
                        if (m.ao == def_ao_)
                            m.ao = LoadLinearTex(entry.path().string());
                        break;
                    case TexRole::Height:
                        if (m.height == def_height_)
                            m.height = LoadLinearTex(entry.path().string());
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
            // Heights now come from the GPU texture — no per-chunk data needed.
            chunks_.push_back(std::move(ch));
        }
    }
}

// ---------------------------------------------------------------------------
// GenerateProcedural
// ---------------------------------------------------------------------------
void Terrain::GenerateProcedural() {
    chunks_.clear();

    float ox = -(grid_w_ * kChunkSize) * 0.5f;
    float oz = -(grid_h_ * kChunkSize) * 0.5f;

    // Build hmap_data_ so the heightmap GPU texture covers the full terrain.
    const int stride = TerrainChunk::kSize - 1;
    hmap_w_       = grid_w_ * stride + 1;
    hmap_h_       = grid_h_ * stride + 1;
    hmap_cell_    = kCellSize;
    hmap_origin_x_ = ox;
    hmap_origin_z_ = oz;
    hmap_size_x_   = (hmap_w_ - 1) * hmap_cell_;
    hmap_size_z_   = (hmap_h_ - 1) * hmap_cell_;
    has_hmap_      = true;

    hmap_data_.resize(hmap_w_ * hmap_h_);
    for (int z = 0; z < hmap_h_; ++z)
        for (int x = 0; x < hmap_w_; ++x)
            hmap_data_[z * hmap_w_ + x] = ProceduralHeight(
                hmap_origin_x_ + x * hmap_cell_,
                hmap_origin_z_ + z * hmap_cell_);

    if (!hmap_tex_) glGenTextures(1, &hmap_tex_);
    glBindTexture(GL_TEXTURE_2D, hmap_tex_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R32F, hmap_w_, hmap_h_, 0,
                 GL_RED, GL_FLOAT, hmap_data_.data());
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);

    chunks_.reserve(grid_w_ * grid_h_);
    for (int cz = 0; cz < grid_h_; ++cz) {
        for (int cx = 0; cx < grid_w_; ++cx) {
            auto ch = std::make_unique<TerrainChunk>();
            float wx = ox + cx * kChunkSize;
            float wz = oz + cz * kChunkSize;
            ch->Init(wx, wz, kCellSize);
            chunks_.push_back(std::move(ch));
        }
    }
}

// ---------------------------------------------------------------------------
// Render
// ---------------------------------------------------------------------------
void Terrain::SetRenderTuning(const TerrainRenderTuning& tuning) {
    render_tuning_.tiling_mul = glm::clamp(tuning.tiling_mul, 0.50f, 2.50f);
    render_tuning_.macro_strength_mul = glm::clamp(tuning.macro_strength_mul, 0.00f, 3.00f);
    render_tuning_.height_blend_slop = glm::clamp(tuning.height_blend_slop, 0.02f, 0.70f);
}

void Terrain::Submit(Pipeline& pipeline, const glm::vec3& cam_pos) const {
    TerrainChunkSubmission base{};
    for (int i = 0; i < 4; ++i) {
        if (i < (int)materials_.size()) {
            base.mat_albedo[i]          = materials_[i].albedo;
            base.mat_normal[i]          = materials_[i].normal    ? materials_[i].normal    : def_normal_;
            base.mat_roughness[i]       = materials_[i].roughness ? materials_[i].roughness : def_roughness_;
            base.mat_ao[i]              = materials_[i].ao        ? materials_[i].ao        : def_ao_;
            base.mat_height[i]          = materials_[i].height    ? materials_[i].height    : def_height_;
            base.mat_normal_strength[i] = materials_[i].normal_strength;
        } else {
            base.mat_albedo[i]          = 0;
            base.mat_normal[i]          = def_normal_;
            base.mat_roughness[i]       = def_roughness_;
            base.mat_ao[i]              = def_ao_;
            base.mat_height[i]          = def_height_;
            base.mat_normal_strength[i] = 2.5f;
        }
    }
    base.tilings        = glm::vec4(
        materials_.size() > 0 ? materials_[0].tiling : 4.0f,
        materials_.size() > 1 ? materials_[1].tiling : 4.0f,
        materials_.size() > 2 ? materials_[2].tiling : 4.0f,
        materials_.size() > 3 ? materials_[3].tiling : 4.0f) * render_tuning_.tiling_mul;
    base.macro_variation = macro_tex_ ? macro_tex_ : def_macro_;
    base.macro_strength  = glm::clamp(
        macro_strength_ * render_tuning_.macro_strength_mul,
        0.0f, 1.0f);
    base.height_blend_slop = render_tuning_.height_blend_slop;
    base.splatmap        = splatmap_tex_;
    base.terrain_origin  = { hmap_origin_x_, hmap_origin_z_ };
    base.terrain_size    = { hmap_size_x_,   hmap_size_z_   };
    base.heightmap_tex   = hmap_tex_;
    base.cell_size       = hmap_cell_;

    constexpr float kLodBase = 128.f;  // distance at which LOD 0→1 transition begins

    for (const auto& ch : chunks_) {
        TerrainChunkSubmission c = base;
        c.vao         = ch->vao();
        c.vbo         = ch->vbo();
        c.ebo         = ch->ebo();
        c.index_count = ch->idx_count();
        c.model       = glm::mat4(1.0f);

        // Fractional LOD based on XZ distance from camera to chunk centre
        glm::vec3 origin = ch->Origin();
        float cx   = origin.x + kChunkSize * 0.5f;
        float cz   = origin.z + kChunkSize * 0.5f;
        float dist = glm::length(glm::vec2(cx - cam_pos.x, cz - cam_pos.z));
        c.lod_level = glm::clamp(std::log2(std::max(dist / kLodBase, 1.f)), 0.f, 3.f);

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
// SampleNormal / SlopeAngle
// ---------------------------------------------------------------------------
glm::vec3 Terrain::SampleNormal(float wx, float wz) const {
    const float eps = (hmap_cell_ > 0.f ? hmap_cell_ : 2.f);
    float hr = SampleHeight(wx + eps, wz);
    float hl = SampleHeight(wx - eps, wz);
    float hu = SampleHeight(wx, wz + eps);
    float hd = SampleHeight(wx, wz - eps);
    // Central-difference gradient → upward-pointing surface normal
    return glm::normalize(glm::vec3(-(hr - hl), 2.f * eps, -(hu - hd)));
}

float Terrain::SlopeAngle(float wx, float wz) const {
    return glm::degrees(std::acos(glm::clamp(SampleNormal(wx, wz).y, 0.f, 1.f)));
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
        if (m.ao        != def_ao_)        del(m.ao);
        if (m.height    != def_height_)    del(m.height);
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
    if (def_ao_)        { glDeleteTextures(1, &def_ao_);        def_ao_        = 0; }
    if (def_height_)    { glDeleteTextures(1, &def_height_);    def_height_    = 0; }
    if (def_macro_)     { glDeleteTextures(1, &def_macro_);     def_macro_     = 0; }
    if (macro_tex_)     { glDeleteTextures(1, &macro_tex_);     macro_tex_     = 0; }
    if (splatmap_tex_)  { glDeleteTextures(1, &splatmap_tex_);  splatmap_tex_  = 0; }
    if (hmap_tex_)      { glDeleteTextures(1, &hmap_tex_);      hmap_tex_      = 0; }
}

} // namespace rco::renderer
