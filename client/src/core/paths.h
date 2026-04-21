#pragma once

namespace rco {

// Changes the process's current working directory to the directory containing
// the current executable. All relative paths (shaders/, assets/, data/...)
// then resolve relative to the exe's install location, independent of how
// the exe was launched (double-click, command line, task scheduler, etc.).
//
// Call this ONCE at the very top of main() before any file I/O.
void SetCwdToExeDir();

} // namespace rco
