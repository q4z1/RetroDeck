// What the launcher remembers between runs, in
// $XDG_CONFIG_HOME/retrodeck/settings.conf (key = value lines).
#pragma once

#include <filesystem>
#include <map>
#include <set>
#include <string>

namespace rd {

// What one game wants instead of the settings (games.conf); empty, 0 or -1
// leaves it to them
struct GameOptions {
    int joyPort = 0;            // 1 or 2
    std::string plus4Emulator;  // "vice", "yape" or "plus4emu"
    std::string amigaModel;     // "A500_OCS_1MB" or "A500_ECS_1MB"
    int loadTurbo = -1;         // 0 or 1
    int screenshotSeconds = 0;  // when its title shows (--make-screenshots)

    bool empty() const;
};

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
    std::map<std::string, GameOptions> games; // Game::key

    void load(const std::filesystem::path &file);
    void save(const std::filesystem::path &file) const;
    // games.conf: [Game::key] and key = value lines below it
    void loadGames(const std::filesystem::path &file);
    void saveGames(const std::filesystem::path &file) const;
    // These settings with that game's choices in place of them
    Settings forGame(const std::string &key) const;
};

// $XDG_CONFIG_HOME/retrodeck, ~/.config/retrodeck without it
std::filesystem::path configDir();
// $XDG_CACHE_HOME/retrodeck
std::filesystem::path cacheDir();
// $XDG_DATA_HOME/retrodeck (title screenshots)
std::filesystem::path dataDir();
// The videos folder (XDG_VIDEOS_DIR in ~/.config/user-dirs.dirs, else
// ~/Videos) / RetroDeck: recordings of games
std::filesystem::path videosDir();

} // namespace rd
