#include "settings.h"

#include <cstdlib>
#include <fstream>

namespace rd {

namespace {

std::filesystem::path xdg(const char *variable, const char *fallback) {
    if (const char *value = std::getenv(variable); value && *value) return std::filesystem::path(value) / "retrodeck";
    const char *home = std::getenv("HOME");
    return std::filesystem::path(home ? home : "/tmp") / fallback / "retrodeck";
}

} // namespace

std::filesystem::path configDir() { return xdg("XDG_CONFIG_HOME", ".config"); }
std::filesystem::path cacheDir() { return xdg("XDG_CACHE_HOME", ".cache"); }
std::filesystem::path dataDir() { return xdg("XDG_DATA_HOME", ".local/share"); }

void Settings::load(const std::filesystem::path &file) {
    std::ifstream in(file);
    std::string line;
    while (std::getline(in, line)) {
        const auto equals = line.find(" = ");
        if (equals == std::string::npos) continue;
        const std::string key = line.substr(0, equals), value = line.substr(equals + 3);
        if (key == "pad") pad = value;
        else if (key == "display") display = value;
        else if (key == "plus4-emulator") plus4Emulator = value;
        else if (key == "amiga-model") amigaModel = value;
        else if (key == "joy-port") joyPort = value == "1" ? 1 : 2;
        else if (key == "favorites-first") favoritesFirst = value != "no";
        else if (key == "favorite") favorites.insert(value);
        else if (key == "last-game") lastGame = value;
        else if (key == "last-system") lastSystem = std::atoi(value.c_str());
    }
}

void Settings::save(const std::filesystem::path &file) const {
    std::error_code error;
    std::filesystem::create_directories(file.parent_path(), error);
    const std::filesystem::path partial = file.string() + ".part";
    {
        std::ofstream out(partial);
        out << "# RetroDeck settings, written by the launcher\n";
        out << "pad = " << pad << "\n";
        out << "display = " << display << "\n";
        out << "plus4-emulator = " << plus4Emulator << "\n";
        out << "amiga-model = " << amigaModel << "\n";
        out << "joy-port = " << joyPort << "\n";
        out << "favorites-first = " << (favoritesFirst ? "yes" : "no") << "\n";
        out << "last-system = " << lastSystem << "\n";
        out << "last-game = " << lastGame << "\n";
        for (const std::string &favorite : favorites) out << "favorite = " << favorite << "\n";
    }
    std::filesystem::rename(partial, file, error);
}

} // namespace rd
