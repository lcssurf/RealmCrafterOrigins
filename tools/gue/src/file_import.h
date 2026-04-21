#pragma once
#include <string>

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

} // namespace gue
