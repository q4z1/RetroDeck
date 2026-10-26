#include "settings.h"

#include <algorithm>
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

bool GameOptions::empty() const {
    return joyPort == 0 && plus4Emulator.empty() && amigaModel.empty() && loadTurbo < 0 && screenshotSeconds == 0;
}

void Settings::loadGames(const std::filesystem::path &file) {
    std::ifstream in(file);
    std::string line, game;
    while (std::getline(in, line)) {
        if (line.size() > 2 && line.front() == '[' && line.back() == ']') {
            game = line.substr(1, line.size() - 2);
            continue;
        }
        const auto equals = line.find(" = ");
        if (game.empty() || equals == std::string::npos) continue;
        const std::string key = line.substr(0, equals), value = line.substr(equals + 3);
        GameOptions &options = games[game];
        if (key == "joy-port") options.joyPort = value == "1" ? 1 : value == "2" ? 2 : 0;
        else if (key == "plus4-emulator") options.plus4Emulator = value;
        else if (key == "amiga-model") options.amigaModel = value;
        else if (key == "load-turbo") options.loadTurbo = value == "yes" ? 1 : value == "no" ? 0 : -1;
        else if (key == "screenshot-seconds") options.screenshotSeconds = std::max(0, std::atoi(value.c_str()));
    }
}

void Settings::saveGames(const std::filesystem::path &file) const {
    std::error_code error;
    std::filesystem::create_directories(file.parent_path(), error);
    const std::filesystem::path partial = file.string() + ".part";
    {
        std::ofstream out(partial);
        out << "# What single games want instead of RetroDeck's settings, written by the\n";
        out << "# launcher (X on a game). [the game's file, ZIP#file for one in a ZIP]\n";
        for (const auto &[game, options] : games) {
            if (options.empty()) continue;
            out << "\n[" << game << "]\n";
            if (options.joyPort) out << "joy-port = " << options.joyPort << "\n";
            if (!options.plus4Emulator.empty()) out << "plus4-emulator = " << options.plus4Emulator << "\n";
            if (!options.amigaModel.empty()) out << "amiga-model = " << options.amigaModel << "\n";
            if (options.loadTurbo >= 0) out << "load-turbo = " << (options.loadTurbo ? "yes" : "no") << "\n";
            if (options.screenshotSeconds) out << "screenshot-seconds = " << options.screenshotSeconds << "\n";
        }
    }
    std::filesystem::rename(partial, file, error);
}

Settings Settings::forGame(const std::string &key) const {
    Settings settings = *this;
    const auto found = games.find(key);
    if (found == games.end()) return settings;
    const GameOptions &options = found->second;
    if (options.joyPort) settings.joyPort = options.joyPort;
    if (!options.plus4Emulator.empty()) settings.plus4Emulator = options.plus4Emulator;
    if (!options.amigaModel.empty()) settings.amigaModel = options.amigaModel;
    if (options.loadTurbo >= 0) settings.loadTurbo = options.loadTurbo != 0;
    return settings;
}

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
        else if (key == "load-turbo") loadTurbo = value != "no";
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
        out << "load-turbo = " << (loadTurbo ? "yes" : "no") << "\n";
        out << "favorites-first = " << (favoritesFirst ? "yes" : "no") << "\n";
        out << "last-system = " << lastSystem << "\n";
        out << "last-game = " << lastGame << "\n";
        for (const std::string &favorite : favorites) out << "favorite = " << favorite << "\n";
    }
    std::filesystem::rename(partial, file, error);
}

} // namespace rd
