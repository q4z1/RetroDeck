// Starting a game: which emulator from toolchain/, with which arguments, and
// the emulator's process while it runs.
#pragma once

#include "library.h"
#include "pads.h"
#include "settings.h"

#include <sys/types.h>

#include <csignal>
#include <string>
#include <utility>
#include <vector>

namespace rd {

struct Toolchain {
    fs::path root;
    fs::path x64sc, xplus4, yapesdl, plus4emu, xmega65, amiga;
    fs::path recorder; // gpu-screen-recorder
    fs::path plus4emuRoms, mega65Rom, kickstart;

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
    int timeout = 0;       // seconds after which it is stopped; 0: none
    std::string audioApp;  // its sound in PipeWire (application.name)
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

// A video of the screen (`connector`, or the first one) with only the
// sound of `audioApp` (Launch::audioApp), into `file` (MP4); empty argv
// and error set if the recorder is missing
Launch planRecording(const Toolchain &toolchain, const std::string &audioApp, const std::string &connector,
                     const fs::path &file, std::string &error);

class Process {
public:
    ~Process();
    bool start(const Launch &launch, const fs::path &log, std::string &error);
    // Polls; false once it has exited (then exitCode() is valid)
    bool running();
    // Asks it to quit (`signal`), and kills it if it is still there after
    // `grace` ms
    void stop(int signal = SIGTERM, Uint32 grace = 2500);
    // Freezes it (SIGSTOP) and lets it go on (SIGCONT), all its processes
    void pause();
    void resume();
    // Sends it `signal`, to it alone (not its children)
    void send(int signal);
    int exitCode() const { return exitCode_; }

private:
    pid_t pid_ = 0;
    int exitCode_ = 0;
    Uint32 stopAt_ = 0;
};

} // namespace rd
