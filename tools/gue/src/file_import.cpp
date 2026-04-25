#include "file_import.h"

#include <filesystem>
#include <string>

#ifdef _WIN32
  #define WIN32_LEAN_AND_MEAN
  #define NOMINMAX
  #include <windows.h>
  #include <commdlg.h>
  #include <shobjidl.h>
  #include <objbase.h>
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

std::string ImportAbsolutePath(const std::string& src_abs,
                               const char* target_subdir) {
    if (src_abs.empty()) return {};

    std::error_code ec;
#ifdef _WIN32
    std::filesystem::path picked = Widen(src_abs.c_str());
#else
    std::filesystem::path picked = src_abs;
#endif
    picked = std::filesystem::canonical(picked, ec);
    if (ec) return {};

    // dist/tools/ (cwd after SetCwdToExeDir) → ../client/assets/
    std::filesystem::path cwd = std::filesystem::current_path();
    std::filesystem::path assetsDir = cwd.parent_path() / "client" / "assets";
    std::filesystem::create_directories(assetsDir, ec);
    assetsDir = std::filesystem::canonical(assetsDir, ec);
    if (ec) return {};

    // If the picked file is already under assets/, reuse it in-place.
    auto rel = std::filesystem::relative(picked, assetsDir, ec);
    bool inside = !ec
                  && !rel.empty()
#ifdef _WIN32
                  && rel.native().front() != L'.'
#else
                  && rel.native().front() != '.'
#endif
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
}

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

    // Convert UTF-16 buf to UTF-8 and delegate to ImportAbsolutePath.
    int n = WideCharToMultiByte(CP_UTF8, 0, buf, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string utf8((size_t)(n - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, buf, -1, utf8.data(), n, nullptr, nullptr);
    return ImportAbsolutePath(utf8, target_subdir);
#endif
}

// ---------------------------------------------------------------------------
// Folder picker — modern Win32 IFileDialog (FOS_PICKFOLDERS)
// ---------------------------------------------------------------------------

std::vector<std::string> PickMultipleFiles(const char* filter_label,
                                           const char* filter_exts) {
#ifndef _WIN32
    (void)filter_label; (void)filter_exts;
    return {};
#else
    // Use a generous buffer — OFN_ALLOWMULTISELECT returns the folder
    // followed by every filename, each separated by NUL. 32 KB handles
    // dozens of long paths.
    std::vector<wchar_t> buf(32 * 1024, L'\0');

    std::wstring filter = BuildFilter(filter_label, filter_exts);

    OPENFILENAMEW ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.lpstrFilter = filter.c_str();
    ofn.lpstrFile   = buf.data();
    ofn.nMaxFile    = (DWORD)buf.size();
    ofn.Flags       = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR
                    | OFN_ALLOWMULTISELECT | OFN_EXPLORER;

    if (!GetOpenFileNameW(&ofn)) return {};

    // Parse the double-null-terminated result.
    //   Single file selected: buf holds the full path, one NUL at the end.
    //   Multi-select:         buf holds <folder>\0<name1>\0<name2>\0...\0\0
    std::vector<std::string> out;
    auto toUtf8 = [](const std::wstring& w) {
        if (w.empty()) return std::string();
        int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1,
                                    nullptr, 0, nullptr, nullptr);
        if (n <= 0) return std::string();
        std::string s(static_cast<size_t>(n - 1), '\0');
        WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1,
                            s.data(), n, nullptr, nullptr);
        return s;
    };

    const wchar_t* p = buf.data();
    std::wstring first = p;
    p += first.size() + 1;

    if (*p == L'\0') {
        // Single file: `first` IS the full path.
        out.push_back(toUtf8(first));
    } else {
        // Multi: `first` is the folder; subsequent strings are filenames.
        std::filesystem::path folder = first;
        while (*p) {
            std::wstring name = p;
            out.push_back(toUtf8((folder / name).wstring()));
            p += name.size() + 1;
        }
    }
    return out;
#endif
}

std::string PickFolder(const char* title) {
#ifdef _WIN32
    // COM init is per-thread. Idempotent when already initialised.
    HRESULT hrInit = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    bool needsUninit = SUCCEEDED(hrInit);

    IFileDialog* dialog = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_ALL,
                                  IID_PPV_ARGS(&dialog));
    std::string result;
    if (SUCCEEDED(hr) && dialog) {
        DWORD flags = 0;
        dialog->GetOptions(&flags);
        dialog->SetOptions(flags | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
        if (title && *title) {
            std::wstring t = Widen(title);
            dialog->SetTitle(t.c_str());
        }
        if (SUCCEEDED(dialog->Show(nullptr))) {
            IShellItem* item = nullptr;
            if (SUCCEEDED(dialog->GetResult(&item)) && item) {
                PWSTR pszPath = nullptr;
                if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &pszPath)) && pszPath) {
                    int n = WideCharToMultiByte(CP_UTF8, 0, pszPath, -1,
                                                nullptr, 0, nullptr, nullptr);
                    if (n > 0) {
                        result.resize((size_t)n - 1);
                        WideCharToMultiByte(CP_UTF8, 0, pszPath, -1,
                                            result.data(), n, nullptr, nullptr);
                    }
                    CoTaskMemFree(pszPath);
                }
                item->Release();
            }
        }
        dialog->Release();
    }
    if (needsUninit) CoUninitialize();
    return result;
#else
    (void)title;
    return {};
#endif
}

} // namespace gue
