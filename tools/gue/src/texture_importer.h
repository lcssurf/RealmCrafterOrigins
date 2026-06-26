#pragma once
#include <string>
#include <vector>

namespace gue {

// ---------------------------------------------------------------------------
// Detected PBR role for an individual texture file on disk.
// ---------------------------------------------------------------------------
enum class TexRole {
    Unknown,
    Albedo,
    Normal,       // OpenGL convention (Y up)
    NormalDX,     // DirectX convention (Y down) — importer flips at write time
    Roughness,
    Metallic,
    AO,
    Alpha,
};

// A single source texture file with its detected role.
struct TexFile {
    std::string abs_path;   // absolute path on disk
    std::string filename;   // basename (without path)
    TexRole     role = TexRole::Unknown;
};

// A group of textures sharing a prefix (typically one PBR material).
struct TextureGroup {
    std::string prefix;         // e.g. "ID01", "M_Body", or the folder name

    // One entry per role — picks the first file found for each role.
    // Empty string means "not present".
    std::string albedo_src;
    std::string normal_src;
    std::string roughness_src;
    std::string metallic_src;
    std::string ao_src;
    std::string alpha_src;
    bool        normal_is_dx = false;

    // Filled in by ImportTextureGroup after files are copied/processed, as
    // paths RELATIVE to dist/client/ (e.g. "assets/textures/dwarf/ID01_Albedo.png").
    // These go straight into media_materials.albedo_path etc.
    std::string albedo_rel;
    std::string normal_rel;
    std::string orm_rel;        // when pack_orm=true (filled); otherwise empty
    std::string roughness_rel;  // when pack_orm=false, kept as separate files
    std::string metallic_rel;
    std::string ao_rel;
    std::string alpha_rel;

    // Material name to use when inserting into media_materials.
    std::string material_name;

    // Per-group toggle that the user can override in the confirm dialog.
    bool enabled = true;
};

struct TextureImportOptions {
    // Collapse AO+Roughness+Metallic into a single ORM texture with
    // R=AO, G=Roughness, B=Metallic (glTF 2.0 convention). Default on —
    // matches what the deferred shader expects.
    bool pack_orm = true;

    // Source Normal_DirectX textures have Y inverted vs OpenGL/glTF.
    // When true (default), the importer writes out a flipped copy.
    bool flip_normal_dx = true;

    // Subfolder under dist/client/assets/materials/ where output lands.
    // If empty, uses the basename of the scanned folder.
    std::string target_subdir;

    // When non-empty, folder structure relative to this path is preserved
    // in the output (e.g. scanned subfolder "chest/" → materials/sub/chest/).
    std::string source_root;
};

// Scan a folder (recursively) for PBR textures. Detects role by filename
// keyword, groups files by common prefix (everything before the role keyword),
// and populates `out_groups`. Returns true if any groups were found.
bool ScanTextureFolder(const std::string& folder,
                       std::vector<TextureGroup>& out_groups);

// Import a scanned group into dist/client/assets/materials/<subdir>/.
// Copies files, optionally packs AO/Rough/Metal into a single ORM PNG,
// and flips Normal_DirectX when requested. Writes relative output paths
// back into the group (*_rel fields). Returns true on success.
//
// Caller must have chdir'd to dist/tools/ (standard for GUE) so that
// "../client/assets/..." resolves correctly.
bool ImportTextureGroup(TextureGroup& g, const TextureImportOptions& opts);

} // namespace gue
