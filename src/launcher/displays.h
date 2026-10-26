// The screens SDL knows, with the connector each is on (DP-3, HDMI-A-1, ...).
// SDL names screens by their model under Wayland ("BNQ BenQ GL2480"), but
// SDL_VIDEO_DISPLAY_PRIORITY, which tells an emulator where to open, wants
// the connector; the monitor's EDID in /sys/class/drm links the two.
#pragma once

#include <SDL.h>

#include <string>
#include <vector>

namespace rd {

struct Display {
    int index = 0;          // SDL's display index
    std::string name;       // as SDL reports it
    std::string connector;  // empty if it cannot be told
    SDL_Rect bounds{};
};

std::vector<Display> displays();
// The connector of SDL display `index`, or empty
std::string connectorOf(int index);
// SDL's display index for a connector, or -1
int displayIndexOf(const std::string &connector);

} // namespace rd
