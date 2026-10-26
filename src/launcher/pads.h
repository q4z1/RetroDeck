// The connected game controllers, which one plays, and how an emulator
// started as a child process gets to see only that one.
#pragma once

#include <SDL.h>

#include <string>
#include <vector>

namespace rd {

struct Pad {
    SDL_GameController *controller = nullptr;
    SDL_JoystickID id = -1;
    std::string name;   // as SDL reports it
    std::string kind;   // "Steam Deck", "Xbox", "PlayStation", ...
    Uint16 vendor = 0, product = 0;
    Uint32 lastUsed = 0; // SDL_GetTicks() of its last button press

    // "Akku 80 %" style; empty when wired or unknown
    std::string battery() const;
};

// How a child process sees the joysticks: SDL_GAMECONTROLLER_IGNORE_DEVICES
// to set, and the index of the chosen pad among those it still sees
struct ChildPads {
    std::string ignore;
    // -1: no pad chosen. Pads with the chosen pad's vendor/product (Steam's
    // virtual gamepads all have the same) stay visible; the index tells them apart.
    int index = -1;
};

class Pads {
public:
    ~Pads() { closeAll(); }
    void closeAll();
    // Opens the controller; false if it is no game controller or already open
    bool open(int deviceIndex);
    void close(SDL_JoystickID id);
    void used(SDL_JoystickID id);

    const std::vector<Pad> &list() const { return pads_; }
    const Pad *find(SDL_JoystickID id) const;
    // The pad named in `preference` if connected; otherwise ("auto") the
    // one last used
    const Pad *active(const std::string &preference) const;
    ChildPads childView(const Pad *pad) const;

private:
    std::vector<Pad> pads_;
};

} // namespace rd
