// The screens SDL knows, with the connector each is on (DP-3, HDMI-A-1, ...).
// SDL names screens by their model under Wayland ("BNQ BenQ GL2480"), but
// SDL_VIDEO_DISPLAY_PRIORITY, which tells an emulator where to open, wants
// the connector; the monitor's EDID in /sys/class/drm links the two.
//
// The setting names the monitor itself, by its EDID (maker, model, serial
// number): a dock that restarts names its connectors anew (DP-3 became DP-5).
#pragma once

#include <SDL.h>

#include <string>
#include <vector>

namespace rd {

struct Display {
    int index = 0;          // SDL's display index
    std::string name;       // as SDL reports it
    std::string connector;  // empty if it cannot be told
    std::string id;         // the monitor itself (monitorId), empty without EDID
    SDL_Rect bounds{};
};

std::vector<Display> displays();
// The connector of SDL display `index`, or empty
std::string connectorOf(int index);
// SDL's display index for a connector, or -1
int displayIndexOf(const std::string &connector);

// "edid:HKC-2316-21632327": maker, product code, serial number
bool isMonitorId(const std::string &setting);
// The connector of the connected monitor with that id, or empty
std::string connectorOfMonitor(const std::string &id);
// The monitor id of a connected connector, or empty
std::string monitorOf(const std::string &connector);
// The desktop's main screen (KDE's output order under Wayland), or empty
// where the desktop does not say (Game Mode: there is only one)
std::string primaryConnector();
// Where RetroDeck belongs for the setting ("auto" or a monitor id): that
// monitor if connected, the main screen otherwise; empty: SDL's choice
std::string preferredConnector(const std::string &setting);

} // namespace rd
