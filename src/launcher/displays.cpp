#include "displays.h"

#include <filesystem>
#include <fstream>
#include <iterator>

namespace rd {

namespace {

struct Connector {
    std::string name;    // "DP-4"
    std::string monitor; // the EDID's monitor name, "BenQ GL2480"
};

// The connected connectors with their monitor's name
std::vector<Connector> connectors() {
    std::vector<Connector> list;
    std::error_code error;
    for (const auto &entry : std::filesystem::directory_iterator("/sys/class/drm", error)) {
        const std::string dir = entry.path().filename().string();
        const auto dash = dir.find('-');
        if (dir.rfind("card", 0) != 0 || dash == std::string::npos) continue;
        std::string status;
        std::ifstream(entry.path() / "status") >> status;
        if (status != "connected") continue;
        Connector connector{dir.substr(dash + 1), {}};
        std::ifstream file(entry.path() / "edid", std::ios::binary);
        const std::vector<unsigned char> edid{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
        // Four 18-byte descriptors from byte 54; 0xfc is the monitor name
        for (std::size_t at = 54; edid.size() >= 128 && at <= 108; at += 18) {
            if (edid[at] || edid[at + 1] || edid[at + 2] || edid[at + 3] != 0xfc) continue;
            std::string text(edid.begin() + static_cast<long>(at) + 5, edid.begin() + static_cast<long>(at) + 18);
            text = text.substr(0, text.find('\n'));
            while (!text.empty() && text.back() == ' ') text.pop_back();
            connector.monitor = text;
        }
        list.push_back(connector);
    }
    return list;
}

std::string match(const std::string &name, const std::vector<Connector> &list) {
    // X11 names start with the connector ("DP-4 24\"")
    const std::string first = name.substr(0, name.find(' '));
    for (const Connector &connector : list) if (connector.name == first) return connector.name;
    // Wayland names end with the monitor's name ("BNQ BenQ GL2480"); two
    // monitors of the same model cannot be told apart this way
    std::string found;
    for (const Connector &connector : list) {
        const std::string &monitor = connector.monitor;
        if (monitor.empty()) continue;
        const bool ends = name.size() > monitor.size() && name.compare(name.size() - monitor.size(), monitor.size(), monitor) == 0 &&
                          name[name.size() - monitor.size() - 1] == ' ';
        if (name == monitor || ends) {
            if (!found.empty()) return {};
            found = connector.name;
        }
    }
    return found;
}

} // namespace

std::vector<Display> displays() {
    const std::vector<Connector> list = connectors();
    std::vector<Display> result;
    for (int i = 0; i < SDL_GetNumVideoDisplays(); ++i) {
        Display screen;
        screen.index = i;
        const char *name = SDL_GetDisplayName(i);
        screen.name = name ? name : "Bildschirm " + std::to_string(i + 1);
        screen.connector = match(screen.name, list);
        SDL_GetDisplayBounds(i, &screen.bounds);
        result.push_back(screen);
    }
    return result;
}

std::string connectorOf(int index) {
    const char *name = SDL_GetDisplayName(index);
    return name ? match(name, connectors()) : "";
}

int displayIndexOf(const std::string &connector) {
    for (const Display &screen : displays()) if (screen.connector == connector) return screen.index;
    return -1;
}

} // namespace rd
