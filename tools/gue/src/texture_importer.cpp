#include "texture_importer.h"

#include <stb_image.h>
#include <stb_image_write.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <unordered_map>

namespace gue {
namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// Role detection — patterns tested in the given order so more specific markers
// ("Normal_DirectX") match before generic ones ("Normal"). All comparisons are
// case-insensitive.
// ---------------------------------------------------------------------------

struct RolePattern { TexRole role; const char* needle; };

static const RolePattern kRolePatterns[] = {
    // Specific / composite markers first
    { TexRole::NormalDX,   "Normal_DirectX" },
    { TexRole::NormalDX,   "Normal_DX"      },
    { TexRole::NormalDX,   "NormalDX"       },
    { TexRole::NormalDX,   "_nor_dx"        },
    { TexRole::Normal,     "Normal_OpenGL"  },
    { TexRole::Normal,     "Normal_GL"      },
    { TexRole::Normal,     "NormalGL"       },
    { TexRole::Normal,     "_nor_gl"        },
    { TexRole::Albedo,     "Base_color"     },
    { TexRole::Albedo,     "BaseColor"      },
    { TexRole::Albedo,     "base_color"     },
    { TexRole::AO,         "Mixed_AO"       },
    { TexRole::AO,         "AmbientOcclusion" },
    { TexRole::AO,         "Ambient_Occlusion" },

    // Generic keywords
    { TexRole::Normal,     "Normal"         },
    { TexRole::Normal,     "_nrm"           },
    { TexRole::Normal,     "_nor"           },
    { TexRole::Albedo,     "Albedo"         },
    { TexRole::Albedo,     "Diffuse"        },
    { TexRole::Albedo,     "_diff"          },
    { TexRole::Albedo,     "_col"           },
    { TexRole::Roughness,  "Roughness"      },
    { TexRole::Roughness,  "_rough"         },
    { TexRole::Metallic,   "Metalness"      },
    { TexRole::Metallic,   "Metallic"       },
    { TexRole::Metallic,   "_metal"         },
    { TexRole::AO,         "_ao"            },
    { TexRole::AO,         "AO"             },
    { TexRole::Alpha,      "Opacity"        },
    { TexRole::Alpha,      "Alpha"          },
    { TexRole::Alpha,      "_alpha"         },
};

// Case-insensitive substring search. Returns the index of the first match, or
// std::string::npos if none.
static size_t IFindIn(const std::string& hay, const char* needle) {
    if (!needle || !*needle) return std::string::npos;
    const size_t nlen = std::strlen(needle);
    if (nlen > hay.size()) return std::string::npos;
    for (size_t i = 0; i + nlen <= hay.size(); ++i) {
        bool ok = true;
        for (size_t j = 0; j < nlen; ++j) {
            char a = (char)std::tolower((unsigned char)hay[i + j]);
            char b = (char)std::tolower((unsigned char)needle[j]);
            if (a != b) { ok = false; break; }
        }
        if (ok) return i;
    }
    return std::string::npos;
}

// Strip the file extension from a name. Returns the stem portion.
static std::string StripExt(const std::string& name) {
    size_t dot = name.find_last_of('.');
    return (dot == std::string::npos) ? name : name.substr(0, dot);
}

static bool IsImageExt(const std::string& ext) {
    std::string e;
    e.reserve(ext.size());
    for (char c : ext) e.push_back((char)std::tolower((unsigned char)c));
    return e == ".png" || e == ".jpg" || e == ".jpeg" || e == ".tga" || e == ".bmp";
}

// Detect role and return the start position of the role keyword in the
// filename stem (everything before that position is the group prefix).
// Returns {Unknown, npos} if no role keyword matched.
struct RoleHit { TexRole role; size_t start; };

static RoleHit DetectRole(const std::string& stem) {
    for (const auto& pat : kRolePatterns) {
        size_t pos = IFindIn(stem, pat.needle);
        if (pos != std::string::npos) return { pat.role, pos };
    }
    return { TexRole::Unknown, std::string::npos };
}

// Extract the group prefix from the filename stem given the role-start
// position. Strips a trailing '_' if present so "ID01_" → "ID01".
static std::string ExtractPrefix(const std::string& stem, size_t roleStart) {
    std::string p = stem.substr(0, roleStart);
    while (!p.empty() && (p.back() == '_' || p.back() == ' ' || p.back() == '-'))
        p.pop_back();
    return p;
}

// ---------------------------------------------------------------------------
// Scan a folder recursively.
// ---------------------------------------------------------------------------

bool ScanTextureFolder(const std::string& folder,
                       std::vector<TextureGroup>& out_groups) {
    out_groups.clear();

    fs::path root(folder);
    std::error_code ec;
    if (!fs::exists(root, ec) || !fs::is_directory(root, ec)) return false;

    // First pass: walk files, detect role, bucket by prefix.
    std::unordered_map<std::string, std::vector<TexFile>> by_prefix;

    for (fs::recursive_directory_iterator it(root, ec), end; it != end; it.increment(ec)) {
        if (ec) break;
        if (!it->is_regular_file(ec)) continue;
        const fs::path p = it->path();
        if (!IsImageExt(p.extension().string())) continue;

        std::string filename = p.filename().string();
        std::string stem     = StripExt(filename);

        RoleHit hit = DetectRole(stem);
        if (hit.role == TexRole::Unknown) continue;  // skip non-PBR files

        std::string prefix = ExtractPrefix(stem, hit.start);
        // Fallback: if no prefix (filename is JUST a role keyword), use the
        // containing folder's name.
        if (prefix.empty()) prefix = p.parent_path().filename().string();
        if (prefix.empty()) prefix = "default";

        TexFile tf;
        tf.abs_path = p.string();
        tf.filename = filename;
        tf.role     = hit.role;
        by_prefix[prefix].push_back(std::move(tf));
    }

    // Second pass: materialise each bucket as a TextureGroup.
    for (auto& [prefix, files] : by_prefix) {
        TextureGroup g;
        g.prefix        = prefix;
        g.material_name = prefix;

        auto assign = [&](TexRole role, const std::string& path) {
            switch (role) {
            case TexRole::Albedo:    if (g.albedo_src.empty())    g.albedo_src    = path; break;
            case TexRole::Normal:    if (g.normal_src.empty()) {  g.normal_src    = path; g.normal_is_dx = false; } break;
            case TexRole::NormalDX:  if (g.normal_src.empty()) {  g.normal_src    = path; g.normal_is_dx = true;  } break;
            case TexRole::Roughness: if (g.roughness_src.empty()) g.roughness_src = path; break;
            case TexRole::Metallic:  if (g.metallic_src.empty())  g.metallic_src  = path; break;
            case TexRole::AO:        if (g.ao_src.empty())        g.ao_src        = path; break;
            case TexRole::Alpha:     if (g.alpha_src.empty())     g.alpha_src     = path; break;
            default: break;
            }
        };
        for (const auto& f : files) assign(f.role, f.abs_path);

        // Skip groups without any recognised albedo — otherwise it's noise.
        if (!g.albedo_src.empty()) out_groups.push_back(std::move(g));
    }

    // Sort by prefix for a stable UI order (ID01, ID02, ID03…).
    std::sort(out_groups.begin(), out_groups.end(),
              [](const TextureGroup& a, const TextureGroup& b) {
                  return a.prefix < b.prefix;
              });
    return !out_groups.empty();
}

// ---------------------------------------------------------------------------
// File helpers
// ---------------------------------------------------------------------------

// Copy a file byte-for-byte. Creates the parent directory if missing.
static bool CopyFileTo(const std::string& src, const std::string& dst) {
    std::error_code ec;
    fs::create_directories(fs::path(dst).parent_path(), ec);
    fs::copy_file(src, dst, fs::copy_options::overwrite_existing, ec);
    return !ec;
}

// Load an 8-bit image. Caller frees with stbi_image_free.
static unsigned char* LoadImage8(const std::string& path, int& w, int& h, int want_ch = 4) {
    int ch = 0;
    return stbi_load(path.c_str(), &w, &h, &ch, want_ch);
}

// Save a PNG. Pass channels=1/2/3/4.
static bool SavePNG(const std::string& path, int w, int h, int ch,
                    const unsigned char* px) {
    std::error_code ec;
    fs::create_directories(fs::path(path).parent_path(), ec);
    return stbi_write_png(path.c_str(), w, h, ch, px, w * ch) != 0;
}

// Flip the G channel of a loaded RGBA image (OpenGL/DX normal convention swap).
static void FlipNormalGChannel(unsigned char* px, int w, int h) {
    const int count = w * h;
    for (int i = 0; i < count; ++i) {
        px[i * 4 + 1] = (unsigned char)(255 - px[i * 4 + 1]);
    }
}

// Build an ORM image from three grayscale sources. Missing sources fall back
// to sensible defaults (AO=255, Roughness=180, Metallic=0). `ref_w`/`ref_h`
// is the target output size — inputs are resampled by nearest-neighbour when
// they don't match (good enough for PBR editor tooling).
static std::vector<unsigned char>
BuildORM(const std::string& ao_src,
         const std::string& rough_src,
         const std::string& metal_src,
         int& out_w, int& out_h)
{
    // Use the albedo resolution if available; else pick the first non-empty channel.
    auto pickSize = [&](int& w, int& h) {
        const std::string* pick[] = { &rough_src, &metal_src, &ao_src };
        for (auto* p : pick) {
            if (p->empty()) continue;
            int cw, ch;
            if (stbi_info(p->c_str(), &cw, &ch, nullptr) != 0) {
                w = cw; h = ch; return true;
            }
        }
        return false;
    };
    int w = 0, h = 0;
    if (!pickSize(w, h)) { out_w = out_h = 0; return {}; }

    out_w = w; out_h = h;
    std::vector<unsigned char> rgba((size_t)w * h * 4, 0);
    // Initial defaults: AO=full (255), Roughness=mid (180), Metallic=0, Alpha=255
    for (int i = 0; i < w * h; ++i) {
        rgba[i * 4 + 0] = 255;
        rgba[i * 4 + 1] = 180;
        rgba[i * 4 + 2] = 0;
        rgba[i * 4 + 3] = 255;
    }

    auto fill = [&](const std::string& src, int channel) {
        if (src.empty()) return;
        int sw, sh;
        unsigned char* px = LoadImage8(src, sw, sh, 1); // grayscale
        if (!px) return;
        for (int y = 0; y < h; ++y) {
            int sy = (sh == h) ? y : (y * sh) / h;
            for (int x = 0; x < w; ++x) {
                int sx = (sw == w) ? x : (x * sw) / w;
                rgba[((size_t)y * w + x) * 4 + channel] = px[sy * sw + sx];
            }
        }
        stbi_image_free(px);
    };
    fill(ao_src,    0);  // R = AO
    fill(rough_src, 1);  // G = Roughness
    fill(metal_src, 2);  // B = Metallic

    return rgba;
}

// ---------------------------------------------------------------------------
// Import a single group
// ---------------------------------------------------------------------------

bool ImportTextureGroup(TextureGroup& g, const TextureImportOptions& opts) {
    const std::string sub = opts.target_subdir.empty() ? std::string("imported")
                                                       : opts.target_subdir;
    const std::string rel_base = "assets/textures/" + sub + "/";
    // When run from dist/tools/, assets live under ../client/.
    const std::string abs_base = "../client/" + rel_base;

    auto destPath = [&](const std::string& srcPath, const char* suffix,
                        const char* ext = ".png") -> std::pair<std::string,std::string> {
        // Output filename: "<prefix>_<suffix>.<ext>"
        std::string fname = g.prefix + "_" + suffix + ext;
        return { abs_base + fname, rel_base + fname };
    };

    // --- Albedo: straight copy (preserve format when possible) ---
    if (!g.albedo_src.empty()) {
        fs::path sp(g.albedo_src);
        std::string ext = sp.extension().string();
        auto [abs, rel] = destPath(g.albedo_src, "Albedo", ext.c_str());
        if (!CopyFileTo(g.albedo_src, abs)) {
            std::fprintf(stderr, "[tex-import] copy failed: %s\n", g.albedo_src.c_str());
            return false;
        }
        g.albedo_rel = rel;
    }

    // --- Normal: copy, optionally flip G channel ---
    if (!g.normal_src.empty()) {
        if (g.normal_is_dx && opts.flip_normal_dx) {
            int w = 0, h = 0;
            unsigned char* px = LoadImage8(g.normal_src, w, h, 4);
            if (!px) { std::fprintf(stderr, "[tex-import] can't read normal: %s\n", g.normal_src.c_str()); return false; }
            FlipNormalGChannel(px, w, h);
            auto [abs, rel] = destPath(g.normal_src, "Normal", ".png");
            if (!SavePNG(abs, w, h, 4, px)) { stbi_image_free(px); return false; }
            stbi_image_free(px);
            g.normal_rel = rel;
        } else {
            fs::path sp(g.normal_src);
            std::string ext = sp.extension().string();
            auto [abs, rel] = destPath(g.normal_src, "Normal", ext.c_str());
            if (!CopyFileTo(g.normal_src, abs)) return false;
            g.normal_rel = rel;
        }
    }

    // --- ORM pack / separate R+M+AO ---
    if (opts.pack_orm && (!g.roughness_src.empty() || !g.metallic_src.empty() || !g.ao_src.empty())) {
        int w = 0, h = 0;
        auto px = BuildORM(g.ao_src, g.roughness_src, g.metallic_src, w, h);
        if (w > 0 && h > 0) {
            auto [abs, rel] = destPath(g.albedo_src /*any*/, "ORM", ".png");
            if (!SavePNG(abs, w, h, 4, px.data())) return false;
            g.orm_rel = rel;
        }
    } else {
        auto copyIf = [&](const std::string& src, const char* suffix, std::string& out_rel) {
            if (src.empty()) return;
            fs::path sp(src);
            std::string ext = sp.extension().string();
            auto [abs, rel] = destPath(src, suffix, ext.c_str());
            if (CopyFileTo(src, abs)) out_rel = rel;
        };
        copyIf(g.roughness_src, "Roughness", g.roughness_rel);
        copyIf(g.metallic_src,  "Metallic",  g.metallic_rel);
        copyIf(g.ao_src,        "AO",        g.ao_rel);
    }

    // --- Alpha: straight copy ---
    if (!g.alpha_src.empty()) {
        fs::path sp(g.alpha_src);
        std::string ext = sp.extension().string();
        auto [abs, rel] = destPath(g.alpha_src, "Alpha", ext.c_str());
        if (CopyFileTo(g.alpha_src, abs)) g.alpha_rel = rel;
    }

    return true;
}

} // namespace gue
