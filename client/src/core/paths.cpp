#include "paths.h"

#include <filesystem>
#include <cstdio>

#ifdef _WIN32
  #define WIN32_LEAN_AND_MEAN
  #define NOMINMAX
  #include <windows.h>
#else
  #include <unistd.h>
  #include <limits.h>
#endif

namespace rco {

void SetCwdToExeDir() {
    std::filesystem::path exe_path;

#ifdef _WIN32
    wchar_t buf[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    if (n == 0 || n == MAX_PATH) {
        std::fprintf(stderr, "[paths] GetModuleFileNameW failed\n");
        return;
    }
    exe_path = buf;
#else
    char buf[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n <= 0) {
        std::fprintf(stderr, "[paths] readlink /proc/self/exe failed\n");
        return;
    }
    buf[n] = 0;
    exe_path = buf;
#endif

    std::error_code ec;
    std::filesystem::current_path(exe_path.parent_path(), ec);
    if (ec) {
        std::fprintf(stderr, "[paths] current_path failed: %s\n", ec.message().c_str());
    }
}

} // namespace rco
