// What the launcher remembers between runs, in
// $XDG_CONFIG_HOME/retrodeck/settings.conf (key = value lines).
#pragma once

#include <filesystem>
#include <set>
#include <string>

namespace rd {

struct Settings {
    std::string pad = "auto";            // name of the controller to play with, or "auto" (the last one used)
    std::string display = "auto";        // connector of the screen to open on (DP-3), or "auto"
    std::string plus4Emulator = "vice";  // "vice", "yape" or "plus4emu"
    std::string amigaModel = "A500_OCS_1MB";
    int joyPort = 2;                     // C64 and Plus/4: the port the joystick is in
    bool loadTurbo = true;               // warp while a disk drive runs (all but Xemu)
    bool favoritesFirst = true;
    std::set<std::string> favorites;     // Game::key
    std::string lastGame;
    int lastSystem = 0;

    void load(const std::filesystem::path &file);
    void save(const std::filesystem::path &file) const;
};

// $XDG_CONFIG_HOME/retrodeck, ~/.config/retrodeck without it
std::filesystem::path configDir();
// $XDG_CACHE_HOME/retrodeck
std::filesystem::path cacheDir();
// $XDG_DATA_HOME/retrodeck (title screenshots)
std::filesystem::path dataDir();

} // namespace rd
