// Starting a game: which emulator from toolchain/, with which arguments, and
// the emulator's process while it runs.
#pragma once

#include "library.h"
#include "pads.h"
#include "settings.h"

#include <sys/types.h>

#include <string>
#include <utility>
#include <vector>

namespace rd {

struct Toolchain {
    fs::path root;
    fs::path x64sc, xplus4, yapesdl, xmega65, amiga;
    fs::path mega65Rom, kickstart;

    // The Kickstart is looked for in `amigaFolders` (the library's)
    static Toolchain locate(const fs::path &root, const fs::path &launcherDir, const std::vector<fs::path> &amigaFolders);
    // What is missing to run games of this system; empty if all is there
    std::string missing(System system, const Settings &settings) const;
    // "VICE · xplus4"
    std::string emulatorName(System system, const Settings &settings) const;
};

struct Launch {
    std::vector<std::string> argv;
    std::vector<std::pair<std::string, std::string>> env;
    int timeout = 0; // seconds after which it is stopped; 0: none
};

// Empty argv (and error set) if the game cannot be started. `connector`
// is the screen to open on (where the launcher is); empty leaves it to SDL.
Launch planLaunch(const Game &game, const std::vector<fs::path> &media, const Settings &settings,
                  const Toolchain &toolchain, const Pad *pad, const ChildPads &view,
                  const std::string &connector, std::string &error);

// The game run without a screen and sound for about `seconds` of its time,
// leaving a screenshot of its last picture in `png` (the title screen, with
// the right number of seconds). Plus/4 games always use VICE for this.
Launch planScreenshot(const Game &game, const std::vector<fs::path> &media, const Settings &settings,
                      const Toolchain &toolchain, const fs::path &png, int seconds, std::string &error);
// Seconds that reach most games' title screens
int screenshotSeconds(System system);

class Process {
public:
    ~Process();
    bool start(const Launch &launch, const fs::path &log, std::string &error);
    // Polls; false once it has exited (then exitCode() is valid)
    bool running();
    // Asks it to quit (SIGTERM), and kills it if it is still there after a while
    void stop();
    // Freezes it (SIGSTOP) and lets it go on (SIGCONT), all its processes
    void pause();
    void resume();
    int exitCode() const { return exitCode_; }

private:
    pid_t pid_ = 0;
    int exitCode_ = 0;
    Uint32 stopAt_ = 0;
};

} // namespace rd
