// The game library: which folders hold which system's games (library.conf),
// and what is found in them.
#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace rd {

namespace fs = std::filesystem;

enum class System { plus4, c64, amiga, mega65 };
constexpr std::array<System, 4> allSystems{System::plus4, System::c64, System::amiga, System::mega65};

const char *systemName(System system);   // "Plus/4"
const char *systemId(System system);     // "plus4", as in library.conf
bool systemFromId(const std::string &id, System &system);

struct Game {
    System system{};
    std::string title;    // "Lotus Esprit Turbo Challenge"
    std::string details;  // "1990 · Gremlin"
    std::string format;   // "ADF", "D81", ...
    std::string folder;   // where it lies, for display
    std::string key;      // stable identity (favorites, last played)
    std::vector<fs::path> files; // the media in order (disk 1, 2, ...); names inside `archive` if set
    fs::path archive;     // the ZIP the files are in, or empty
    std::uintmax_t bytes = 0;
    fs::path artwork;     // a screenshot, or empty
};

struct Library {
    std::vector<Game> games;
    std::vector<std::string> notes; // problems found in library.conf
};

Library scanLibrary(const fs::path &root, const fs::path &config);
// The folders library.conf names for a system (the Kickstart lies in the Amiga's)
std::vector<fs::path> libraryFolders(const fs::path &root, const fs::path &config, System system);

// Where RetroDeck keeps the game's title screenshot:
// $XDG_DATA_HOME/retrodeck/screenshots/<system>/<name of the game's file>.png
fs::path screenshotPath(const Game &game);

// The files to hand to the emulator; games from a ZIP are extracted into
// cacheDir first. Empty (and error set) on failure.
std::vector<fs::path> mediaFiles(const Game &game, const fs::path &cacheDir, std::string &error);

} // namespace rd
