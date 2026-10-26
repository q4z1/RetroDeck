#include "library.h"
#include "settings.h"
#include "zip.h"

#include <fnmatch.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <map>
#include <regex>
#include <set>
#include <sstream>

namespace rd {

namespace {

struct Root {
    System system{};
    fs::path path;
    int depth = 3;
    std::vector<std::string> exclude;
};

std::string lower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

std::string upper(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return text;
}

std::string trim(const std::string &text) {
    const auto first = text.find_first_not_of(" \t\r");
    if (first == std::string::npos) return {};
    return text.substr(first, text.find_last_not_of(" \t\r") - first + 1);
}

const std::vector<std::string> &extensions(System system) {
    static const std::vector<std::string> plus4{".prg", ".d64", ".d81", ".t64", ".tap"};
    static const std::vector<std::string> c64{".prg", ".d64", ".d71", ".d81", ".g64", ".t64", ".tap", ".crt"};
    static const std::vector<std::string> amiga{".adf", ".adz", ".dms"};
    static const std::vector<std::string> mega65{".prg", ".d81"};
    switch (system) {
    case System::plus4: return plus4;
    case System::c64: return c64;
    case System::amiga: return amiga;
    case System::mega65: return mega65;
    }
    return c64;
}

bool supported(System system, const fs::path &path) {
    const auto &list = extensions(system);
    return std::find(list.begin(), list.end(), lower(path.extension().string())) != list.end();
}

fs::path expandHome(const std::string &text, const fs::path &root) {
    if (text == "~" || text.rfind("~/", 0) == 0) {
        const char *home = std::getenv("HOME");
        return (fs::path(home ? home : "/") / text.substr(text.size() > 1 ? 2 : 1)).lexically_normal();
    }
    const fs::path path(text);
    return (path.is_absolute() ? path : root / path).lexically_normal();
}

// Expands '*' and '?' in the path's components (one folder level each)
std::vector<fs::path> expandGlob(const fs::path &pattern) {
    std::vector<fs::path> current{pattern.root_path()};
    for (const fs::path &part : pattern.relative_path()) {
        const std::string piece = part.string();
        std::vector<fs::path> next;
        if (piece.find_first_of("*?[") == std::string::npos) {
            for (const fs::path &base : current) next.push_back(base / part);
        } else {
            for (const fs::path &base : current) {
                std::error_code error;
                std::vector<fs::path> matches;
                for (const auto &entry : fs::directory_iterator(base, error)) {
                    if (fnmatch(piece.c_str(), entry.path().filename().c_str(), FNM_PERIOD) == 0) matches.push_back(entry.path());
                }
                std::sort(matches.begin(), matches.end());
                next.insert(next.end(), matches.begin(), matches.end());
            }
        }
        current = std::move(next);
    }
    return current;
}

std::vector<Root> readConfig(const fs::path &root, const fs::path &config, std::vector<std::string> &notes) {
    std::vector<Root> roots;
    std::ifstream file(config);
    if (!file) {
        notes.push_back("library.conf fehlt: " + config.string());
        return roots;
    }
    std::string line;
    for (int number = 1; std::getline(file, line); ++number) {
        if (const auto hash = line.find('#'); hash != std::string::npos) line.erase(hash);
        std::istringstream words(line);
        std::string id, pathText, option;
        if (!(words >> id)) continue;
        // The path may contain spaces when quoted
        words >> std::ws;
        if (words.peek() == '"') {
            words.get();
            std::getline(words, pathText, '"');
        } else {
            words >> pathText;
        }
        Root entry;
        if (!systemFromId(id, entry.system) || pathText.empty()) {
            notes.push_back("library.conf, Zeile " + std::to_string(number) + ": nicht verstanden");
            continue;
        }
        while (words >> option) {
            if (option.rfind("depth=", 0) == 0) {
                entry.depth = std::atoi(option.c_str() + 6);
            } else if (option.rfind("exclude=", 0) == 0) {
                std::istringstream patterns(option.substr(8));
                for (std::string pattern; std::getline(patterns, pattern, ',');) entry.exclude.push_back(pattern);
            }
        }
        for (const fs::path &path : expandGlob(expandHome(pathText, root))) {
            entry.path = path;
            roots.push_back(entry);
        }
    }
    return roots;
}

bool excluded(const Root &root, const std::string &name) {
    for (const std::string &pattern : root.exclude) {
        if (fnmatch(pattern.c_str(), name.c_str(), FNM_CASEFOLD) == 0) return true;
    }
    return false;
}

// Whether a file from the ZIP also lies unpacked next to it (beside the
// ZIP, or in a folder named like it), so the ZIP need not be listed too
bool unpackedBeside(const fs::path &archive, const fs::path &member, std::uintmax_t size) {
    const fs::path dir = archive.parent_path(), named = dir / archive.stem();
    for (const fs::path &candidate : {dir / member, named / member, dir / member.filename(), named / member.filename()}) {
        std::error_code error;
        if (fs::is_regular_file(candidate, error) && fs::file_size(candidate, error) == size) return true;
    }
    return false;
}

// Lower case letters and digits only: "AnotherWorld" -> "anotherworld"
std::string compact(const std::string &text) {
    std::string result;
    for (unsigned char c : text) if (std::isalnum(c)) result += static_cast<char>(std::tolower(c));
    return result;
}

void walk(const fs::path &dir, int depth, const std::function<void(const fs::path &)> &visit) {
    std::error_code error;
    std::vector<fs::path> entries;
    for (const auto &entry : fs::directory_iterator(dir, fs::directory_options::skip_permission_denied, error)) {
        entries.push_back(entry.path());
    }
    std::sort(entries.begin(), entries.end());
    for (const fs::path &path : entries) {
        if (path.filename().string().front() == '.') continue;
        if (fs::is_directory(path, error)) {
            if (depth > 0) walk(path, depth - 1, visit);
        } else if (fs::is_regular_file(path, error)) {
            visit(path);
        }
    }
}

// "Game (Disk 2 of 3)" -> disk 2; the rest of the name is the group
const std::regex diskTag(R"(\s*[\(\[_\- ]\s*(disk|disc|side)\s*_?([0-9]+|[a-d])(\s*of\s*[0-9]+)?\s*[\)\]]?)",
                         std::regex::icase);

int diskNumber(const std::string &stem, std::string &rest) {
    std::smatch match;
    if (!std::regex_search(stem, match, diskTag)) {
        rest = stem;
        return 0;
    }
    rest = match.prefix().str() + match.suffix().str();
    const std::string number = match[2].str();
    if (std::isdigit(static_cast<unsigned char>(number[0]))) return std::atoi(number.c_str());
    return std::tolower(static_cast<unsigned char>(number[0])) - 'a' + 1;
}

// "Lotus Esprit Turbo Challenge (1990)(Gremlin)[a]" -> title and details,
// "smaragd-dash-1.3" -> "Smaragd Dash 1.3"
void describe(const std::string &stem, std::string &title, std::string &details) {
    const auto open = stem.find_first_of("([");
    title = trim(stem.substr(0, open));
    details.clear();
    if (open != std::string::npos) {
        static const std::regex tag(R"([\(\[]([^\)\]]*)[\)\]])");
        for (std::sregex_iterator it(stem.begin() + static_cast<long>(open), stem.end(), tag), end; it != end; ++it) {
            const std::string text = trim((*it)[1].str());
            if (text.empty()) continue;
            if (!details.empty()) details += " · ";
            details += text;
        }
    }
    if (title.empty()) title = stem;
    if (title.find(' ') == std::string::npos) {
        std::replace(title.begin(), title.end(), '_', ' ');
        // Hyphens become spaces, except between digits ("BD1-3")
        for (std::size_t i = 0; i < title.size(); ++i) {
            const bool digits = i > 0 && i + 1 < title.size() && std::isdigit(static_cast<unsigned char>(title[i - 1])) &&
                                std::isdigit(static_cast<unsigned char>(title[i + 1]));
            if (title[i] == '-' && !digits) title[i] = ' ';
        }
    }
    // "AnotherWorld" -> "Another World"
    if (title.find(' ') == std::string::npos) {
        for (std::size_t i = 1; i < title.size(); ++i) {
            if (std::islower(static_cast<unsigned char>(title[i - 1])) && std::isupper(static_cast<unsigned char>(title[i])))
                title.insert(i++, " ");
        }
    }
    bool wordStart = true;
    if (std::none_of(title.begin(), title.end(), [](unsigned char c) { return std::isupper(c); })) {
        for (char &c : title) {
            if (wordStart && std::isalpha(static_cast<unsigned char>(c))) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            wordStart = c == ' ';
        }
    }
}

std::string displayFolder(const fs::path &dir) {
    std::string text = dir.lexically_normal().string();
    if (const char *home = std::getenv("HOME")) {
        const std::string prefix = std::string(home) + "/";
        if (text.rfind(prefix, 0) == 0) text = "~/" + text.substr(prefix.size());
    }
    return text;
}

bool isImage(const fs::path &path) {
    const std::string extension = lower(path.extension().string());
    return extension == ".png" || extension == ".jpg" || extension == ".jpeg";
}

fs::path findArtwork(const fs::path &dir, const std::string &stem) {
    std::error_code error;
    for (const char *extension : {".png", ".jpg", ".jpeg"}) {
        const fs::path candidate = dir / (stem + extension);
        if (fs::is_regular_file(candidate, error)) return candidate;
    }
    fs::path base = dir;
    for (int level = 0; level < 3 && !base.empty(); ++level, base = base.parent_path()) {
        for (const char *name : {"screenshots", "media", "images"}) {
            const fs::path folder = base / name;
            if (!fs::is_directory(folder, error)) continue;
            std::vector<fs::path> images;
            for (const auto &entry : fs::directory_iterator(folder, error)) {
                if (entry.is_regular_file(error) && isImage(entry.path())) images.push_back(entry.path());
            }
            if (images.empty()) continue;
            std::sort(images.begin(), images.end());
            for (const fs::path &image : images) if (lower(image.stem().string()) == lower(stem)) return image;
            for (const fs::path &image : images) if (lower(image.filename().string()).find("title") != std::string::npos) return image;
            return images.front();
        }
        if (base == base.root_path()) break;
    }
    return {};
}

struct Candidate {
    fs::path file;      // on disk, or the name inside the archive
    fs::path archive;
    std::uintmax_t bytes = 0;
};

} // namespace

const char *systemName(System system) {
    switch (system) {
    case System::plus4: return "Plus/4";
    case System::c64: return "C64";
    case System::amiga: return "Amiga";
    case System::mega65: return "MEGA65";
    }
    return "";
}

const char *systemId(System system) {
    switch (system) {
    case System::plus4: return "plus4";
    case System::c64: return "c64";
    case System::amiga: return "amiga";
    case System::mega65: return "mega65";
    }
    return "";
}

bool systemFromId(const std::string &id, System &system) {
    for (System candidate : allSystems) {
        if (lower(id) == systemId(candidate)) {
            system = candidate;
            return true;
        }
    }
    return false;
}

Library scanLibrary(const fs::path &root, const fs::path &config) {
    Library library;
    std::set<std::string> seen;
    for (const Root &entry : readConfig(root, config, library.notes)) {
        std::error_code error;
        if (!fs::is_directory(entry.path, error)) continue;
        // Groups of disks, by folder and name without the disk number
        std::map<std::string, std::vector<std::pair<int, Candidate>>> groups;
        std::vector<std::string> order;
        // A group is "<folder or archive>\n<title stem>"
        auto add = [&](const Candidate &candidate) {
            std::string rest;
            const std::string stem = candidate.file.stem().string();
            const int disk = entry.system == System::amiga ? diskNumber(stem, rest) : (rest = stem, 0);
            const std::string group = candidate.archive.empty()
                                          ? candidate.file.parent_path().string() + "\n" + rest
                                          : candidate.archive.string() + "\n" + candidate.archive.stem().string();
            if (!groups.count(group)) order.push_back(group);
            groups[group].push_back({disk, candidate});
        };
        walk(entry.path, entry.depth, [&](const fs::path &path) {
            const std::string name = path.filename().string();
            if (excluded(entry, name)) return;
            if (lower(path.extension().string()) == ".zip") {
                for (const zip::Entry &inside : zip::list(path)) {
                    const fs::path member(inside.name);
                    if (supported(entry.system, member) && !unpackedBeside(path, member, inside.size))
                        add({member, path, inside.size});
                }
            } else if (supported(entry.system, path)) {
                std::error_code sizeError;
                add({path, {}, fs::file_size(path, sizeError)});
            }
        });
        for (const std::string &group : order) {
            auto &disks = groups[group];
            std::stable_sort(disks.begin(), disks.end(), [](const auto &a, const auto &b) { return a.first < b.first; });
            // Without disk numbers, every file is a game of its own
            const bool multiDisk = disks.size() > 1 && std::all_of(disks.begin(), disks.end(), [](const auto &d) { return d.first > 0; });
            std::vector<std::vector<Candidate>> games;
            if (multiDisk) {
                games.emplace_back();
                for (const auto &disk : disks) games.back().push_back(disk.second);
            } else {
                for (const auto &disk : disks) games.push_back({disk.second});
            }
            for (const auto &files : games) {
                const Candidate &first = files.front();
                Game game;
                game.system = entry.system;
                game.archive = first.archive;
                const fs::path location = first.archive.empty() ? first.file : first.archive;
                game.key = location.string() + (first.archive.empty() ? "" : "#" + first.file.string());
                if (!seen.insert(game.key).second) continue;
                std::string stem = group.substr(group.find('\n') + 1);
                // The game file's name, also in a ZIP ("space-commander.d81"
                // says more than "space_commander_v1_3.zip")
                if (!multiDisk) stem = first.file.stem().string();
                // An 8.3 name cut from its folder's or its ZIP's
                // ("anotherw.d81" in AnotherWorld/): those say it better
                if (!multiDisk && stem.size() == 8) {
                    std::vector<std::string> names;
                    if (first.archive.empty()) {
                        if (first.file.parent_path() != entry.path) names.push_back(first.file.parent_path().filename().string());
                    } else {
                        if (first.file.has_parent_path()) names.push_back(first.file.parent_path().filename().string());
                        names.push_back(first.archive.stem().string());
                    }
                    for (const std::string &name : names) {
                        if (compact(name).size() > compact(stem).size() && compact(name).rfind(compact(stem), 0) == 0) {
                            stem = name;
                            break;
                        }
                    }
                }
                describe(stem, game.title, game.details);
                game.format = upper(first.file.extension().string().substr(1));
                if (files.size() > 1) game.format += " ×" + std::to_string(files.size());
                game.folder = displayFolder(location.parent_path());
                for (const Candidate &file : files) {
                    game.files.push_back(file.file);
                    game.bytes += file.bytes;
                }
                std::error_code missing;
                game.artwork = screenshotPath(game);
                if (!fs::is_regular_file(game.artwork, missing))
                    game.artwork = findArtwork(location.parent_path(), location.stem().string());
                library.games.push_back(std::move(game));
            }
        }
    }
    std::stable_sort(library.games.begin(), library.games.end(), [](const Game &a, const Game &b) {
        if (a.system != b.system) return a.system < b.system;
        const std::string left = lower(a.title), right = lower(b.title);
        if (left != right) return left < right;
        return a.details < b.details;
    });
    return library;
}

std::vector<fs::path> libraryFolders(const fs::path &root, const fs::path &config, System system) {
    std::vector<std::string> notes;
    std::vector<fs::path> folders;
    for (const Root &entry : readConfig(root, config, notes)) {
        if (entry.system == system) folders.push_back(entry.path);
    }
    return folders;
}

fs::path screenshotPath(const Game &game) {
    // Named after the game's (first) file, also inside a ZIP: one ZIP may hold several games
    return dataDir() / "screenshots" / systemId(game.system) / (game.files.front().stem().string() + ".png");
}

std::vector<fs::path> mediaFiles(const Game &game, const fs::path &cacheDir, std::string &error) {
    if (game.archive.empty()) return game.files;
    const std::size_t hash = std::hash<std::string>{}(game.archive.string());
    const fs::path target = cacheDir / (std::to_string(hash) + "-" + game.archive.stem().string());
    // A ZIP replaced under the same name (a patched disk) is unpacked anew:
    // the folder remembers the size and time of the ZIP it came from
    std::error_code sizeFailed, timeFailed;
    const auto size = fs::file_size(game.archive, sizeFailed);
    const auto time = fs::last_write_time(game.archive, timeFailed).time_since_epoch().count();
    const std::string stamp = std::to_string(size) + " " + std::to_string(time);
    const fs::path stampFile = target / ".zip-stamp";
    std::string unpacked;
    if (std::ifstream in(stampFile); in) std::getline(in, unpacked);
    const bool stale = !sizeFailed && !timeFailed && unpacked != stamp;
    std::error_code ignored;
    if (stale) fs::remove_all(target, ignored);
    // All of the archive, into one folder: games may need the files beside
    // their disks (data files a MEGA65 game loads from the SD card). Inside
    // the cache, files keep their names (emulators show them).
    for (const zip::Entry &entry : zip::list(game.archive)) {
        const fs::path out = target / fs::path(entry.name).filename();
        if (fs::exists(out, ignored)) continue;
        if (!zip::extract(game.archive, entry, out)) {
            error = "Konnte nicht entpackt werden: " + entry.name;
            return {};
        }
    }
    if (stale) std::ofstream(stampFile) << stamp << "\n";
    std::vector<fs::path> files;
    for (const fs::path &member : game.files) files.push_back(target / member.filename());
    return files;
}

} // namespace rd
