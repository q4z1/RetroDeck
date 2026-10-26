#include "emulators.h"

#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>

extern char **environ;

namespace rd {

namespace {

bool executable(const fs::path &path) {
    return !path.empty() && ::access(path.c_str(), X_OK) == 0;
}

std::string lower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

// VICE reads controllers through SDL's plain joystick interface, by raw
// axis, button and hat numbers, which differ from controller to controller.
// SDL's game controller mapping knows them; this writes them into a VICE
// joystick map (.vjm) for VICE's device `device`.
//
//   left stick, D-pad   joystick directions
//   A, B                fire
//   Start (Menu)        VICE's menu
void writeViceJoymap(const fs::path &file, SDL_GameController *controller, int device) {
    enum { up = 1, down = 2, left = 4, right = 8, fire = 16 };
    enum { inputAxis = 0, inputButton = 1, inputHat = 2 };
    enum { actionJoystick = 1, actionMenu = 4 };
    std::ofstream out(file);
    out << "# VICE joystick map, written by RetroDeck for the controller in use\n";
    out << "!CLEAR\n";
    auto mapButton = [&](SDL_GameControllerButton button, int action, int pin) {
        const SDL_GameControllerButtonBind bind = SDL_GameControllerGetBindForButton(controller, button);
        if (bind.bindType == SDL_CONTROLLER_BINDTYPE_BUTTON) {
            out << device << ' ' << inputButton << ' ' << bind.value.button << ' ' << action;
        } else if (bind.bindType == SDL_CONTROLLER_BINDTYPE_HAT) {
            // Four rows per hat: up, down, left, right
            const int mask = bind.value.hat.hat_mask;
            const int row = mask & SDL_HAT_UP ? 0 : mask & SDL_HAT_DOWN ? 1 : mask & SDL_HAT_LEFT ? 2 : 3;
            out << device << ' ' << inputHat << ' ' << bind.value.hat.hat * 4 + row << ' ' << action;
        } else if (bind.bindType == SDL_CONTROLLER_BINDTYPE_AXIS) {
            out << device << ' ' << inputAxis << ' ' << bind.value.axis * 2 << ' ' << action;
        } else {
            return;
        }
        if (action == actionJoystick) out << ' ' << pin;
        out << '\n';
    };
    auto mapAxis = [&](SDL_GameControllerAxis axis, int positive, int negative) {
        const SDL_GameControllerButtonBind bind = SDL_GameControllerGetBindForAxis(controller, axis);
        if (bind.bindType != SDL_CONTROLLER_BINDTYPE_AXIS) return;
        // Two rows per axis: positive, negative
        out << device << ' ' << inputAxis << ' ' << bind.value.axis * 2 << ' ' << actionJoystick << ' ' << positive << '\n';
        out << device << ' ' << inputAxis << ' ' << bind.value.axis * 2 + 1 << ' ' << actionJoystick << ' ' << negative << '\n';
    };
    mapAxis(SDL_CONTROLLER_AXIS_LEFTX, right, left);
    mapAxis(SDL_CONTROLLER_AXIS_LEFTY, down, up);
    mapButton(SDL_CONTROLLER_BUTTON_DPAD_UP, actionJoystick, up);
    mapButton(SDL_CONTROLLER_BUTTON_DPAD_DOWN, actionJoystick, down);
    mapButton(SDL_CONTROLLER_BUTTON_DPAD_LEFT, actionJoystick, left);
    mapButton(SDL_CONTROLLER_BUTTON_DPAD_RIGHT, actionJoystick, right);
    mapButton(SDL_CONTROLLER_BUTTON_A, actionJoystick, fire);
    mapButton(SDL_CONTROLLER_BUTTON_B, actionJoystick, fire);
    mapButton(SDL_CONTROLLER_BUTTON_START, actionMenu, 0);
}

std::string viceQuote(const fs::path &path) {
    return "\"" + path.string() + "\"";
}

// What Xemu shows the MEGA65 as the SD card's root, so that a game finds
// the files it loads from the card next to its D81 (Another World's
// anotherw.dat, Space Commander's SPACE.SCP): a folder in the cache with a
// link to each file in `dir`, also under its name in lower case, since the
// MEGA65 asks for "space.scp" and Linux tells it apart from "SPACE.SCP".
// Files a game writes stay in that folder.
fs::path sdFolder(const fs::path &dir) {
    const fs::path folder = cacheDir() / "mega65-sd" /
                            (std::to_string(std::hash<std::string>{}(dir.string())) + "-" + dir.filename().string());
    std::error_code error;
    fs::create_directories(folder, error);
    if (error) return {};
    for (const auto &entry : fs::directory_iterator(folder, error)) {
        if (entry.is_symlink(error)) fs::remove(entry.path(), error);
    }
    for (const auto &entry : fs::directory_iterator(dir, error)) {
        if (!entry.is_regular_file(error)) continue;
        const std::string name = entry.path().filename().string();
        for (const std::string &link : {name, lower(name)}) {
            if (!fs::exists(folder / link, error)) fs::create_symlink(entry.path(), folder / link, error);
        }
    }
    return folder;
}

} // namespace

Toolchain Toolchain::locate(const fs::path &root, const fs::path &launcherDir, const std::vector<fs::path> &amigaFolders) {
    Toolchain t;
    t.root = root;
    const fs::path tc = root / "toolchain";
    t.x64sc = tc / "vice" / "bin" / "x64sc";
    t.xplus4 = tc / "vice" / "bin" / "xplus4";
    t.yapesdl = tc / "bin" / "yapesdl";
    t.xmega65 = tc / "bin" / "xmega65";
    t.amiga = launcherDir / "retrodeck-amiga";
    t.mega65Rom = tc / "rom" / "MEGA65.ROM";
    // Your own Kickstart (it may not be passed on), next to the Amiga games
    for (const fs::path &folder : amigaFolders) {
        std::error_code error;
        std::vector<fs::path> found;
        for (const auto &entry : fs::directory_iterator(folder, error)) {
            const std::string name = lower(entry.path().filename().string());
            if (name.find("kick") != std::string::npos && (name.ends_with(".rom") || name.ends_with(".bin"))) found.push_back(entry.path());
        }
        if (found.empty()) continue;
        std::sort(found.begin(), found.end());
        t.kickstart = found.front();
        break;
    }
    return t;
}

std::string Toolchain::missing(System system, const Settings &settings) const {
    switch (system) {
    case System::plus4:
        if (settings.plus4Emulator == "yape")
            return executable(yapesdl) ? "" : "YapeSDL fehlt – toolchain/setup.sh yapesdl";
        return executable(xplus4) ? "" : "VICE fehlt – toolchain/setup.sh vice";
    case System::c64:
        return executable(x64sc) ? "" : "VICE fehlt – toolchain/setup.sh vice";
    case System::amiga:
        if (!executable(amiga)) return "Amiga-Frontend fehlt – ./build.sh";
        return kickstart.empty() ? "Kickstart-ROM fehlt – nach ~/Games/Retro/Amiga/ kopieren" : "";
    case System::mega65:
        if (!executable(xmega65)) return "Xemu fehlt – toolchain/setup.sh xemu";
        return fs::exists(mega65Rom) ? "" : "MEGA65.ROM fehlt – toolchain/setup.sh rom";
    }
    return {};
}

std::string Toolchain::emulatorName(System system, const Settings &settings) const {
    switch (system) {
    case System::plus4: return settings.plus4Emulator == "yape" ? "YapeSDL" : "VICE · xplus4";
    case System::c64: return "VICE · x64sc";
    case System::amiga: return settings.amigaModel == "A500_ECS_1MB" ? "vAmiga · A500 ECS" : "vAmiga · A500 OCS";
    case System::mega65: return "Xemu · xmega65";
    }
    return {};
}

Launch planLaunch(const Game &game, const std::vector<fs::path> &media, const Settings &settings,
                  const Toolchain &toolchain, const Pad *pad, const ChildPads &view,
                  const std::string &connector, std::string &error) {
    Launch launch;
    error = toolchain.missing(game.system, settings);
    if (!error.empty() || media.empty()) {
        if (error.empty()) error = "Keine Datei zum Starten";
        return launch;
    }
    if (!view.ignore.empty()) launch.env.emplace_back("SDL_GAMECONTROLLER_IGNORE_DEVICES", view.ignore);
    // That screen becomes the emulator's first, where SDL opens centered
    // and fullscreen windows (SDL 3.2+, also under sdl2-compat)
    if (!connector.empty()) launch.env.emplace_back("SDL_VIDEO_DISPLAY_PRIORITY", connector);
    const fs::path first = media.front();
    const std::string extension = lower(first.extension().string());

    if (game.system == System::c64 || (game.system == System::plus4 && settings.plus4Emulator != "yape")) {
        const bool c64 = game.system == System::c64;
        const fs::path dir = configDir() / "vice";
        std::error_code ignored;
        fs::create_directories(dir, ignored);
        // VICE's own settings (saved from its menu) live in sdl-vicerc;
        // what RetroDeck sets for this run comes on top from retrodeck.rc
        const fs::path extra = dir / "retrodeck.rc", joymap = dir / "retrodeck.vjm";
        {
            std::ofstream rc(extra);
            rc << "[" << (c64 ? "C64SC" : "PLUS4") << "]\n";
            if (pad && view.index >= 0) {
                writeViceJoymap(joymap, pad->controller, view.index);
                rc << "JoyMapFile=" << viceQuote(joymap) << "\n";
            }
        }
        launch.argv = {(c64 ? toolchain.x64sc : toolchain.xplus4).string(),
                       "-config", (dir / "sdl-vicerc").string(),
                       "-addconfig", extra.string(),
                       c64 ? "-VICIIfull" : "-TEDfull",
                       "+confirmonexit",
                       // centered, on the first screen, whatever VICE has saved
                       "-windowxpos", std::to_string(SDL_WINDOWPOS_CENTERED),
                       "-windowypos", std::to_string(SDL_WINDOWPOS_CENTERED)};
        // Analog joystick N is device 4 + N
        const std::string device = pad && view.index >= 0 ? std::to_string(4 + view.index) : "0";
        const std::string port = settings.joyPort == 1 ? "-joydev1" : "-joydev2";
        const std::string other = settings.joyPort == 1 ? "-joydev2" : "-joydev1";
        launch.argv.insert(launch.argv.end(), {port, device, other, "0"});
        // Warp while it loads: autostart, and whenever a drive motor runs
        // (patches/vice-drive-warp.patch)
        if (settings.loadTurbo) launch.argv.insert(launch.argv.end(), {"-autostart-warp", "-drivewarp"});
        else launch.argv.insert(launch.argv.end(), {"+autostart-warp", "+drivewarp"});
        if (c64 && extension == ".crt") launch.argv.insert(launch.argv.end(), {"-cartcrt", first.string()});
        else launch.argv.insert(launch.argv.end(), {"-autostart", first.string()});
        return launch;
    }

    switch (game.system) {
    case System::plus4:
        launch.argv = {toolchain.yapesdl.string(), first.string()};
        launch.env.emplace_back("YAPE_FULLSCREEN", "1");
        // patches/yapesdl-load-turbo.patch
        if (settings.loadTurbo) launch.env.emplace_back("YAPE_LOAD_TURBO", "1");
        break;
    case System::mega65:
        launch.argv = {toolchain.xmega65.string(), "-rom", toolchain.mega65Rom.string(), "-fullscreen", "-besure"};
        // Hyppo's file functions see the game's folder instead of the SD image
        if (const fs::path sd = sdFolder(first.parent_path()); !sd.empty())
            launch.argv.insert(launch.argv.end(), {"-hdosvirt", "-hdosdir", sd.string()});
        if (extension == ".prg") launch.argv.insert(launch.argv.end(), {"-prg", first.string()});
        else launch.argv.insert(launch.argv.end(), {"-8", first.string(), "-autoload"});
        // Xemu's menu is a GTK popup, which only shows over the SDL window with X11
        if (std::getenv("DISPLAY")) launch.env.emplace_back("GDK_BACKEND", "x11");
        break;
    case System::amiga:
        launch.argv = {toolchain.amiga.string(), "--kickstart", toolchain.kickstart.string(), "--model", settings.amigaModel};
        if (pad && view.index >= 0) launch.argv.insert(launch.argv.end(), {"--pad", std::to_string(view.index)});
        if (settings.loadTurbo) launch.argv.push_back("--load-turbo");
        for (const fs::path &disk : media) launch.argv.push_back(disk.string());
        break;
    default:
        break;
    }
    return launch;
}

int screenshotSeconds(System system) {
    switch (system) {
    case System::plus4:
    case System::c64: return 25;
    case System::amiga: return 40;
    case System::mega65: return 30;
    }
    return 30;
}

Launch planScreenshot(const Game &game, const std::vector<fs::path> &media, const Settings &settings,
                      const Toolchain &toolchain, const fs::path &png, int seconds, std::string &error) {
    Launch launch;
    Settings vice = settings;
    vice.plus4Emulator = "vice"; // YapeSDL cannot save a screenshot
    error = toolchain.missing(game.system, vice);
    if (!error.empty() || media.empty()) {
        if (error.empty()) error = "Keine Datei zum Starten";
        return launch;
    }
    launch.env = {{"SDL_VIDEODRIVER", "offscreen"}, {"SDL_RENDER_DRIVER", "software"}, {"SDL_AUDIODRIVER", "dummy"}};
    const fs::path first = media.front();
    const std::string extension = lower(first.extension().string());
    switch (game.system) {
    case System::plus4:
    case System::c64: {
        // Warp throughout; VICE quits after the given number of cycles
        // (seconds at the machine's clock) and saves its last picture
        const bool c64 = game.system == System::c64;
        const long clock = c64 ? 985248 : 1773447;
        // VICE makes its own folder in $XDG_CONFIG_HOME, but not that one
        std::error_code ignored;
        fs::create_directories(configDir(), ignored);
        launch.argv = {(c64 ? toolchain.x64sc : toolchain.xplus4).string(),
                       "-config", (cacheDir() / "vice-screenshot.rc").string(),
                       "-sounddev", "dummy", "-warp", "+confirmonexit",
                       "-limitcycles", std::to_string(seconds * clock),
                       "-exitscreenshot", png.string(),
                       // a PRG straight into memory: loading through the
                       // drive takes the Plus/4 longer than the seconds
                       "-autostartprgmode", "1"};
        if (c64 && extension == ".crt") launch.argv.insert(launch.argv.end(), {"-cartcrt", first.string()});
        else launch.argv.insert(launch.argv.end(), {"-autostart", first.string()});
        launch.timeout = seconds + 30;
        break;
    }
    case System::mega65:
        // As fast as it goes, stopped after `seconds`; Xemu saves the last
        // complete frame when it quits
        launch.argv = {toolchain.xmega65.string(), "-rom", toolchain.mega65Rom.string(),
                       "-headless", "-sleepless", "-besure", "-screenshot", png.string()};
        if (const fs::path sd = sdFolder(first.parent_path()); !sd.empty())
            launch.argv.insert(launch.argv.end(), {"-hdosvirt", "-hdosdir", sd.string()});
        if (extension == ".prg") launch.argv.insert(launch.argv.end(), {"-prg", first.string()});
        else launch.argv.insert(launch.argv.end(), {"-8", first.string(), "-autoload"});
        launch.timeout = seconds;
        break;
    case System::amiga:
        // In real time: 60 drawn frames a second
        launch.argv = {toolchain.amiga.string(), "--kickstart", toolchain.kickstart.string(), "--model", settings.amigaModel,
                       "--window", "736x574", "--frames", std::to_string(seconds * 60), "--screenshot", png.string()};
        for (const fs::path &disk : media) launch.argv.push_back(disk.string());
        launch.timeout = seconds + 30;
        break;
    }
    return launch;
}

Process::~Process() {
    if (pid_ > 0) {
        ::kill(-pid_, SIGKILL);
        ::waitpid(pid_, nullptr, 0);
    }
}

bool Process::start(const Launch &launch, const fs::path &log, std::string &error) {
    std::map<std::string, std::string> overrides(launch.env.begin(), launch.env.end());
    std::vector<std::string> envStrings;
    for (char **entry = environ; *entry; ++entry) {
        const std::string text = *entry;
        if (!overrides.count(text.substr(0, text.find('=')))) envStrings.push_back(text);
    }
    for (const auto &[key, value] : overrides) envStrings.push_back(key + "=" + value);
    std::vector<char *> envp, argv;
    for (std::string &text : envStrings) envp.push_back(text.data());
    envp.push_back(nullptr);
    std::vector<std::string> args = launch.argv;
    for (std::string &arg : args) argv.push_back(arg.data());
    argv.push_back(nullptr);

    std::error_code ignored;
    fs::create_directories(log.parent_path(), ignored);
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, log.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    posix_spawn_file_actions_adddup2(&actions, STDOUT_FILENO, STDERR_FILENO);
    posix_spawnattr_t attributes;
    posix_spawnattr_init(&attributes);
    // A process group of its own, so that stop() reaches its children too
    posix_spawnattr_setpgroup(&attributes, 0);
    posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETPGROUP);
    const int result = posix_spawn(&pid_, argv[0], &actions, &attributes, argv.data(), envp.data());
    posix_spawn_file_actions_destroy(&actions);
    posix_spawnattr_destroy(&attributes);
    if (result != 0) {
        pid_ = 0;
        error = std::string("Start fehlgeschlagen: ") + std::strerror(result);
        return false;
    }
    stopAt_ = 0;
    return true;
}

bool Process::running() {
    if (pid_ <= 0) return false;
    int status = 0;
    const pid_t done = ::waitpid(pid_, &status, WNOHANG);
    if (done == pid_) {
        exitCode_ = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
        pid_ = 0;
        return false;
    }
    if (stopAt_ && SDL_TICKS_PASSED(SDL_GetTicks(), stopAt_)) ::kill(-pid_, SIGKILL);
    return true;
}

void Process::stop() {
    if (pid_ <= 0 || stopAt_) return;
    ::kill(-pid_, SIGTERM);
    // A paused process only sees the SIGTERM once it runs again
    ::kill(-pid_, SIGCONT);
    stopAt_ = SDL_GetTicks() + 2500;
}

void Process::pause() {
    if (pid_ > 0) ::kill(-pid_, SIGSTOP);
}

void Process::resume() {
    if (pid_ > 0) ::kill(-pid_, SIGCONT);
}

} // namespace rd
