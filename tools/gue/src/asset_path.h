#pragma once
#include <string>

namespace gue {

// Resolve a media-relative path (as stored in the DB, e.g. "assets/models/x.glb")
// into a path usable from dist/tools/ (GUE's runtime cwd). Absolute paths and
// explicitly-rooted relatives ("./x", "../x") are returned unchanged.
inline std::string ResolveClientAsset(const std::string& p) {
    if (p.empty()) return p;
    // Absolute path: "C:\...", "/usr/...", "D:..."
    if (p[0] == '/' || (p.size() > 1 && p[1] == ':')) return p;
    // Already rooted relative to cwd
    if (p.rfind("../", 0) == 0 || p.rfind("./", 0) == 0) return p;
    return std::string("../client/") + p;
}

} // namespace gue
