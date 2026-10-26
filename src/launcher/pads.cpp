#include "pads.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>

namespace rd {

namespace {

constexpr Uint16 valve = 0x28de, steamDeck = 0x1205;

std::string vidpid(Uint16 vendor, Uint16 product) {
    char text[16];
    std::snprintf(text, sizeof text, "0x%04x/0x%04x", vendor, product);
    return text;
}

std::string kindOf(SDL_GameController *controller, const std::string &name, Uint16 vendor, Uint16 product) {
    if ((vendor == valve && product == steamDeck) || name.find("Steam Deck") != std::string::npos) return "Steam Deck";
    switch (SDL_GameControllerGetType(controller)) {
    case SDL_CONTROLLER_TYPE_XBOX360:
    case SDL_CONTROLLER_TYPE_XBOXONE: return "Xbox";
    case SDL_CONTROLLER_TYPE_PS3:
    case SDL_CONTROLLER_TYPE_PS4:
    case SDL_CONTROLLER_TYPE_PS5: return "PlayStation";
    case SDL_CONTROLLER_TYPE_NINTENDO_SWITCH_PRO: return "Switch";
    default: break;
    }
    if (vendor == valve) return "Steam";
    return "Controller";
}

} // namespace

std::string Pad::battery() const {
    switch (SDL_JoystickCurrentPowerLevel(SDL_GameControllerGetJoystick(controller))) {
    case SDL_JOYSTICK_POWER_EMPTY: return "Akku leer";
    case SDL_JOYSTICK_POWER_LOW: return "Akku schwach";
    case SDL_JOYSTICK_POWER_MEDIUM: return "Akku halb";
    case SDL_JOYSTICK_POWER_FULL: return "Akku voll";
    default: return {};
    }
}

void Pads::closeAll() {
    for (Pad &pad : pads_) SDL_GameControllerClose(pad.controller);
    pads_.clear();
}

bool Pads::open(int deviceIndex) {
    if (!SDL_IsGameController(deviceIndex)) return false;
    const SDL_JoystickID id = SDL_JoystickGetDeviceInstanceID(deviceIndex);
    if (find(id)) return false;
    SDL_GameController *controller = SDL_GameControllerOpen(deviceIndex);
    if (!controller) return false;
    Pad pad;
    pad.controller = controller;
    pad.id = id;
    const char *name = SDL_GameControllerName(controller);
    pad.name = name ? name : "Controller";
    pad.vendor = SDL_GameControllerGetVendor(controller);
    pad.product = SDL_GameControllerGetProduct(controller);
    pad.kind = kindOf(controller, pad.name, pad.vendor, pad.product);
    // A pad that is plugged in later is probably the one to play with
    pad.lastUsed = pads_.empty() ? 0 : SDL_GetTicks();
    pads_.push_back(pad);
    return true;
}

void Pads::close(SDL_JoystickID id) {
    const auto found = std::find_if(pads_.begin(), pads_.end(), [&](const Pad &pad) { return pad.id == id; });
    if (found == pads_.end()) return;
    SDL_GameControllerClose(found->controller);
    pads_.erase(found);
}

void Pads::used(SDL_JoystickID id) {
    for (Pad &pad : pads_) if (pad.id == id) pad.lastUsed = SDL_GetTicks();
}

const Pad *Pads::find(SDL_JoystickID id) const {
    for (const Pad &pad : pads_) if (pad.id == id) return &pad;
    return nullptr;
}

const Pad *Pads::active(const std::string &preference) const {
    if (preference != "auto") {
        for (const Pad &pad : pads_) if (pad.name == preference) return &pad;
    }
    const Pad *best = nullptr;
    for (const Pad &pad : pads_) if (!best || pad.lastUsed > best->lastUsed) best = &pad;
    return best;
}

ChildPads Pads::childView(const Pad *pad) const {
    ChildPads view;
    // Keep what is hidden already (Steam hides the physical controllers
    // behind its virtual ones this way)
    const char *inherited = std::getenv("SDL_GAMECONTROLLER_IGNORE_DEVICES");
    std::vector<std::string> ignore;
    if (inherited && *inherited) ignore.emplace_back(inherited);
    if (!pad) {
        view.ignore = ignore.empty() ? "" : ignore.front();
        return view;
    }
    const std::string chosen = vidpid(pad->vendor, pad->product);
    // The Deck's own controller also sends its motion sensors all the
    // time; hide it unless it is the one that plays
    if (chosen != vidpid(valve, steamDeck)) ignore.push_back(vidpid(valve, steamDeck));
    const int count = SDL_NumJoysticks();
    for (int i = 0; i < count; ++i) {
        const std::string device = vidpid(SDL_JoystickGetDeviceVendor(i), SDL_JoystickGetDeviceProduct(i));
        if (device != chosen && std::find(ignore.begin(), ignore.end(), device) == ignore.end()) ignore.push_back(device);
    }
    int visible = 0;
    for (int i = 0; i < count; ++i) {
        if (SDL_JoystickGetDeviceInstanceID(i) == pad->id) view.index = visible;
        if (vidpid(SDL_JoystickGetDeviceVendor(i), SDL_JoystickGetDeviceProduct(i)) == chosen) ++visible;
    }
    for (const std::string &entry : ignore) view.ignore += (view.ignore.empty() ? "" : ",") + entry;
    return view;
}

} // namespace rd
