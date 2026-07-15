#pragma once
#include <string>
#include <vector>

namespace gue {

// Opens the native "Open File" dialog (Win32 OPENFILENAMEW).
//
// If the user picks a file and it is already inside dist/client/assets/,
// returns the relative path (e.g. "assets/models/foo.glb") directly.
//
// Otherwise, copies the file into dist/client/assets/<target_subdir>/ and
// returns the relative path of the copy.
//
// Returns "" if the user cancels or an error occurs.
//
// filter_label:  UI label for the filter (e.g., "3D Model")
// filter_exts:   comma-separated extensions (e.g., "glb,fbx,obj,dae,b3d").
//                Pass "" or nullptr to accept any file.
// target_subdir: where to copy if the file isn't already under assets/.
//                No leading/trailing slash. Examples: "models", "textures", "anims".
std::string PickAndImportAsset(const char* filter_label,
                               const char* filter_exts,
                               const char* target_subdir);

// Opens the native folder picker (IFileDialog with FOS_PICKFOLDERS).
// Returns the absolute path the user selected, or "" if cancelled.
std::string PickFolder(const char* title = "Select folder");

// Opens the native multi-select file dialog. Returns absolute paths of
// every file the user picked. Empty vector if cancelled.
//
// filter_label / filter_exts — same semantics as PickAndImportAsset.
// Pass "" to accept any file.
std::vector<std::string> PickMultipleFiles(const char* filter_label,
                                           const char* filter_exts);

// Copies `src_abs` (absolute path on disk) into dist/client/assets/<subdir>/
// and returns the asset-relative path (e.g. "assets/models/knight.glb").
// If the file is already inside assets/, returns its relative path without
// copying. Returns "" on failure.
//
// Shared between the single-file PickAndImportAsset flow and the batch
// importer so both produce identical on-disk layouts.
std::string ImportAbsolutePath(const std::string& src_abs,
                               const char* target_subdir);

// Lists every image file (png/jpg/jpeg/bmp/tga) anywhere under
// dist/client/assets/, recursively — the whole texture tree, not scoped to
// any one subfolder. Each entry is an "assets/..." relative path, same
// convention as MediaModel::file_path / ImportAbsolutePath's return value.
// Shared by any picker that lets the user reuse an already-imported texture
// as-is (e.g. item/ability icon pickers) instead of re-importing a copy.
std::vector<std::string> ListTextureAssets();

} // namespace gue
