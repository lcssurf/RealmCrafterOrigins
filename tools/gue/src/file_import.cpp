#include "file_import.h"

#include <filesystem>
#include <string>

#ifdef _WIN32
  #define WIN32_LEAN_AND_MEAN
  #define NOMINMAX
  #include <windows.h>
  #include <commdlg.h>
#endif

namespace gue {

#ifdef _WIN32
static std::wstring Widen(const char* s) {
    if (!s || !*s) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
    if (n <= 0) return {};
    std::wstring w(static_cast<size_t>(n - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s, -1, w.data(), n);
    return w;
}

// Build a double-null-terminated filter string for OPENFILENAMEW:
//   "Label\0*.glb;*.fbx\0All Files\0*.*\0\0"
static std::wstring BuildFilter(const char* label, const char* exts) {
    std::wstring out;
    std::wstring labelW = Widen(label && *label ? label : "Files");

    std::wstring pattern;
    if (exts && *exts) {
        std::string in = exts;
        size_t start = 0;
        while (start <= in.size()) {
            size_t p = in.find(',', start);
            if (p == std::string::npos) p = in.size();
            if (p > start) {
                if (!pattern.empty()) pattern.push_back(L';');
                pattern += L"*.";
                pattern += Widen(in.substr(start, p - start).c_str());
            }
            start = p + 1;
        }
    }
    if (pattern.empty()) pattern = L"*.*";

    out += labelW;   out.push_back(L'\0');
    out += pattern;  out.push_back(L'\0');
    out += L"All Files"; out.push_back(L'\0');
    out += L"*.*";   out.push_back(L'\0');
    out.push_back(L'\0');
    return out;
}
#endif

std::string PickAndImportAsset(const char* filter_label,
                               const char* filter_exts,
                               const char* target_subdir) {
#ifndef _WIN32
    (void)filter_label; (void)filter_exts; (void)target_subdir;
    return {};
#else
    std::wstring filter = BuildFilter(filter_label, filter_exts);

    wchar_t buf[MAX_PATH * 2] = {};
    OPENFILENAMEW ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.lpstrFilter = filter.c_str();
    ofn.lpstrFile   = buf;
    ofn.nMaxFile    = MAX_PATH * 2;
    // OFN_NOCHANGEDIR: the dialog must NOT change our cwd — we rely on it to
    // resolve paths relative to dist/tools/ throughout the session.
    ofn.Flags       = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;

    if (!GetOpenFileNameW(&ofn)) return {};

    std::error_code ec;
    std::filesystem::path picked = buf;
    picked = std::filesystem::canonical(picked, ec);
    if (ec) return {};

    // dist/tools/ (cwd after SetCwdToExeDir) → ../client/assets/
    std::filesystem::path cwd = std::filesystem::current_path();
    std::filesystem::path assetsDir = cwd.parent_path() / "client" / "assets";
    std::filesystem::create_directories(assetsDir, ec);
    assetsDir = std::filesystem::canonical(assetsDir, ec);
    if (ec) return {};

    // If the picked file is already under assets/, reuse it.
    auto rel = std::filesystem::relative(picked, assetsDir, ec);
    bool inside = !ec
                  && !rel.empty()
                  && rel.native().front() != L'.'
                  && !rel.is_absolute();
    if (inside) {
        return std::string("assets/") + rel.generic_string();
    }

    // Otherwise copy into assets/<target_subdir>/<basename>.
    std::string sub = (target_subdir && *target_subdir) ? target_subdir : "imported";
    std::filesystem::path dstDir = assetsDir / sub;
    std::filesystem::create_directories(dstDir, ec);
    std::filesystem::path dstFile = dstDir / picked.filename();

    std::filesystem::copy_file(picked, dstFile,
        std::filesystem::copy_options::overwrite_existing, ec);
    if (ec) return {};

    std::string out = "assets/";
    out += sub;
    out += "/";
    out += picked.filename().generic_string();
    return out;
#endif
}

} // namespace gue
