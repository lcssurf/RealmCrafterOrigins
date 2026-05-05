#pragma once
#include <glad/glad.h>
#include <stb_image.h>
#include <string>
#include <vector>
#include <filesystem>
#include <algorithm>
#include <cctype>

// ---------------------------------------------------------------------------
// Texture-role auto-detection from filenames
// Supports: PolyHaven (_col_, _nor_gl_, _rough_, _ao_)
//           Substance Painter (_BaseColor, _Normal, _Roughness)
//           Simple (albedo.png, normal.png, roughness.png)
// ---------------------------------------------------------------------------
enum class TexRole { Albedo, Normal, Roughness, AO, ORM, Height, Unknown };

inline TexRole GuessRole(const std::string& stem) {
    std::string s = stem;
    std::transform(s.begin(), s.end(), s.begin(), ::tolower);

    // ORM / ARM packed — check first to avoid false matches
    if (s.find("orm") != std::string::npos ||
        s.find("arm") != std::string::npos) return TexRole::ORM;

    // Normal map — OpenGL convention (green = up)
    // Reject DirectX normal maps (_nor_dx, normdx)
    bool isDX = s.find("_dx") != std::string::npos || s.find("dx_") != std::string::npos;
    if (!isDX && (s.find("nor")    != std::string::npos ||
                  s.find("nrm")    != std::string::npos ||
                  s.find("normal") != std::string::npos)) return TexRole::Normal;

    // Albedo / Base Color
    if (s.find("col")       != std::string::npos ||
        s.find("albedo")    != std::string::npos ||
        s.find("diffuse")   != std::string::npos ||
        s.find("diff")      != std::string::npos ||
        s.find("basecolor") != std::string::npos ||
        s.find("base_color")!= std::string::npos) return TexRole::Albedo;

    // Roughness
    if (s.find("rough") != std::string::npos) return TexRole::Roughness;

    // AO
    if (s == "ao" ||
        s.find("_ao")      != std::string::npos ||
        s.find("ao_")      != std::string::npos ||
        s.find("ambient")  != std::string::npos ||
        s.find("occlusion")!= std::string::npos) return TexRole::AO;

    // Height / displacement
    if (s.find("height")  != std::string::npos ||
        s.find("disp")    != std::string::npos ||
        s.find("_h_")     != std::string::npos ||
        s.find("bump")    != std::string::npos) return TexRole::Height;

    return TexRole::Unknown;
}

// ---------------------------------------------------------------------------
// GL texture helpers
// ---------------------------------------------------------------------------
static GLuint LoadTex(const std::string& path, bool srgb = true) {
    int w, h, ch;
    stbi_set_flip_vertically_on_load(true);
    unsigned char* px = stbi_load(path.c_str(), &w, &h, &ch, 4);
    if (!px) return 0;

    GLuint tex;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    // Albedo → sRGB; normal/roughness → linear
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

// 1×1 solid-color placeholder textures
inline GLuint MakeSolidTex(uint8_t r, uint8_t g, uint8_t b, uint8_t a = 255) {
    GLuint t;
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    const uint8_t px[4] = {r, g, b, a};
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    return t;
}

// ---------------------------------------------------------------------------
// Material — one folder = one PBR material
// ---------------------------------------------------------------------------
struct Material {
    std::string name;
    float       tiling    = 4.f;

    GLuint albedo    = 0; // sRGB
    GLuint normal    = 0; // linear — flat (128,128,255) when missing
    GLuint roughness = 0; // linear — grey (180) when missing → ~0.7 rough
    GLuint ao        = 0; // linear — white (255) when missing → no occlusion
    GLuint height    = 0; // linear — white (255) when missing → flat height-blend

    void Unload() {
        auto del = [](GLuint& t){ if (t) { glDeleteTextures(1, &t); t = 0; } };
        del(albedo); del(normal); del(roughness); del(ao); del(height);
    }
};

// ---------------------------------------------------------------------------
// Scanner — one subfolder = one material, auto-detects texture roles
// Flat .png/.jpg in root = one material each (albedo only)
// ---------------------------------------------------------------------------
inline std::vector<Material> ScanMaterials(const std::string& dir,
                                            GLuint defaultNormal,
                                            GLuint defaultRoughness,
                                            GLuint defaultAO     = 0,
                                            GLuint defaultHeight  = 0)
{
    namespace fs = std::filesystem;
    fs::path root(dir);
    std::vector<Material> out;
    if (!fs::exists(root)) return out;

    static const auto isImg = [](const fs::path& p) {
        auto e = p.extension().string();
        std::transform(e.begin(), e.end(), e.begin(), ::tolower);
        return e == ".png" || e == ".jpg" || e == ".jpeg" || e == ".tga";
    };

    // --- Subfolders: each is a material ---
    for (auto& sub : fs::directory_iterator(root)) {
        if (!sub.is_directory()) continue;

        Material m;
        m.name      = sub.path().filename().string();
        m.normal    = defaultNormal;
        m.roughness = defaultRoughness;
        m.ao        = defaultAO;
        m.height    = defaultHeight;

        for (auto& f : fs::directory_iterator(sub.path())) {
            if (!f.is_regular_file() || !isImg(f.path())) continue;
            TexRole role = GuessRole(f.path().stem().string());
            switch (role) {
            case TexRole::Albedo:
                if (!m.albedo) m.albedo = LoadTex(f.path().string(), true);
                break;
            case TexRole::Normal:
                if (m.normal == defaultNormal)
                    m.normal = LoadTex(f.path().string(), false);
                break;
            case TexRole::Roughness:
            case TexRole::ORM:
                if (m.roughness == defaultRoughness)
                    m.roughness = LoadTex(f.path().string(), false);
                break;
            case TexRole::AO:
                if (m.ao == defaultAO)
                    m.ao = LoadTex(f.path().string(), false);
                break;
            case TexRole::Height:
                if (m.height == defaultHeight)
                    m.height = LoadTex(f.path().string(), false);
                break;
            default: break;
            }
        }

        if (m.albedo) out.push_back(std::move(m));
    }

    // Sort subfolder-derived materials by name before inserting the flat entry.
    std::sort(out.begin(), out.end(),
              [](const Material& a, const Material& b){ return a.name < b.name; });

    // --- Root-level images: aggregate ALL flat files into ONE material ---
    // Handles the common "flat" layout where albedo/normal/roughness sit
    // directly inside the material folder rather than in a sub-folder.
    // Inserted at front so it wins over any subfolder variants when present.
    {
        Material flat;
        flat.name      = root.filename().string();
        flat.normal    = defaultNormal;
        flat.roughness = defaultRoughness;
        flat.ao        = defaultAO;
        flat.height    = defaultHeight;
        for (auto& f : fs::directory_iterator(root)) {
            if (!f.is_regular_file() || !isImg(f.path())) continue;
            TexRole role = GuessRole(f.path().stem().string());
            switch (role) {
            case TexRole::Albedo:
                if (!flat.albedo)
                    flat.albedo = LoadTex(f.path().string(), true);
                break;
            case TexRole::Normal:
                if (flat.normal == defaultNormal)
                    flat.normal = LoadTex(f.path().string(), false);
                break;
            case TexRole::Roughness:
            case TexRole::ORM:
                if (flat.roughness == defaultRoughness)
                    flat.roughness = LoadTex(f.path().string(), false);
                break;
            case TexRole::AO:
                if (flat.ao == defaultAO)
                    flat.ao = LoadTex(f.path().string(), false);
                break;
            case TexRole::Height:
                if (flat.height == defaultHeight)
                    flat.height = LoadTex(f.path().string(), false);
                break;
            default: break;
            }
        }
        if (flat.albedo) out.insert(out.begin(), std::move(flat));
    }

    return out;
}
