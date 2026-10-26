// Where RetroDeck lives: the folder with library.conf, found upwards from
// the running binary, so the folder can be anywhere.
#pragma once

#include <filesystem>
#include <system_error>

namespace rd {

inline std::filesystem::path executableDir() {
    std::error_code error;
    return std::filesystem::read_symlink("/proc/self/exe", error).parent_path();
}

inline std::filesystem::path findRoot() {
    std::error_code error;
    for (std::filesystem::path dir = executableDir(); !dir.empty(); dir = dir.parent_path()) {
        if (std::filesystem::exists(dir / "library.conf", error)) return dir;
        if (dir == dir.root_path()) break;
    }
    return std::filesystem::current_path(error);
}

} // namespace rd
