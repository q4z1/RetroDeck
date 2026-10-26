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
#include <sstream>

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

// YapeSDL takes a controller's right stick (and its D-pad) as the joystick:
// SDL's mapping of the controller in use, with the left stick's axes as the
// right one's too, for SDL_GAMECONTROLLERCONFIG. Empty without a mapping.
std::string yapeMapping(SDL_GameController *controller) {
    char *mapping = SDL_GameControllerMapping(controller);
    if (!mapping) return {};
    std::vector<std::string> fields;
    std::stringstream in(mapping);
    SDL_free(mapping);
    for (std::string field; std::getline(in, field, ',');) if (!field.empty()) fields.push_back(field);
    auto value = [&](const std::string &key) -> std::string {
        for (const std::string &field : fields) if (field.rfind(key + ":", 0) == 0) return field.substr(key.size() + 1);
        return {};
    };
    const std::string x = value("leftx"), y = value("lefty");
    if (fields.size() < 2 || x.empty() || y.empty()) return {};
    std::string out;
    for (const std::string &field : fields) {
        if (field.rfind("rightx:", 0) == 0 || field.rfind("righty:", 0) == 0) continue;
        out += field + ",";
    }
    return out + "rightx:" + x + ",righty:" + y + ",";
}

// YapeSDL's settings for RetroDeck: a yape.conf of its own (Yape keeps it
// in SDL's pref path, $XDG_DATA_HOME/Gaia/yapeSDL), so that a Yape started
// otherwise keeps the user's. It starts as a copy of the user's yape.conf,
// if there is one, and is kept between runs (what Yape's menu saves stays);
// every start puts the one controller on both joystick ports
// (ActiveJoystick 0). Returns the folder for XDG_DATA_HOME.
fs::path yapeDataHome() {
    const fs::path home = configDir() / "yape";
    const fs::path conf = home / "Gaia" / "yapeSDL" / "yape.conf";
    std::error_code ignored;
    fs::create_directories(conf.parent_path(), ignored);
    const fs::path users = dataDir().parent_path() / "Gaia" / "yapeSDL" / "yape.conf";
    std::vector<std::string> lines;
    {
        std::ifstream in(fs::exists(conf, ignored) ? conf : users);
        for (std::string line; std::getline(in, line);) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.rfind("ActiveJoystick ", 0) != 0) lines.push_back(line);
        }
    }
    if (lines.empty() || lines.front() != "[Yape configuration file]") lines = {"[Yape configuration file]"};
    std::ofstream out(conf);
    for (const std::string &line : lines) out << line << "\n";
    out << "ActiveJoystick = 0\n";
    return home;
}

// plus4emu's settings for a game, on top of the keyboard map from its
// p4_keys.cfg: a PAL Plus/4 with 64 KB, its ROMs and the drives' (as
// p4makecfg's P4_64k_PAL.cfg), and the controller in use as the joystick in
// `port`. plus4emu reads joysticks through SDL as key codes - axis n
// 0xC000 + 2n (to the negative side) and + 1 (positive), button b
// 0xC010 + b, hat h 0xC020 + 4h + 0/1/2/3 (right, up, left, down) - which
// keyboard.RR.0 and .1 put on row RR of the Plus/4's keyboard matrix, where
// the joystick lines are too. The other port's joystick codes are cleared:
// p4_keys.cfg puts axes 2 and 3 there, an Xbox controller's left trigger
// (which rests at one end) and right stick.
void writePlus4emuConfig(const fs::path &file, const fs::path &roms, SDL_GameController *controller, int port) {
    std::ofstream out(file);
    out << "# plus4emu settings, written by RetroDeck on every start\n";
    out << "display.ntscMode\tNo\nmemory.ram.size\t64\n";
    auto rom = [&](const char *slot, const char *name, int offset) {
        out << "memory.rom." << slot << ".file\t\"" << (*name ? (roms / name).string() : "") << "\"\n";
        out << "memory.rom." << slot << ".offset\t" << offset << "\n";
    };
    rom("00", "p4_basic.rom", 0);
    rom("01", "p4kernal.rom", 0);
    for (const char *slot : {"02", "03", "04", "05", "06", "07"}) rom(slot, "", 0);
    rom("0A", "mps801.rom", 0);
    rom("0C", "1526_07c.rom", 0);
    rom("10", "dos1541.rom", 0);
    rom("20", "dos1551.rom", 0);
    rom("30", "dos1581.rom", 0);
    rom("31", "dos1581.rom", 16384);
    out << "vm.cpuClockFrequency\t1\nvm.videoClockFrequency\t17734475\n";
    out << "vm.enableACIA\tYes\nvm.serialBusDelayOffset\t0\n";
    out << "joystick.enableJoystick\tYes\n";
    // Sound on (the device is PortAudio's default: PLUS4EMU_DEFAULT_AUDIO)
    out << "sound.enabled\tYes\nsound.device\t0\n";
    if (!controller) return;

    // Rows of up, down, left, right, fire in the matrix
    const int rows2[5] = {0x48, 0x49, 0x4A, 0x4B, 0x4F}, rows1[5] = {0x50, 0x51, 0x52, 0x53, 0x56};
    const int *mine = port == 1 ? rows1 : rows2, *other = port == 1 ? rows2 : rows1;
    auto key = [&](int row, int slot, int code) {
        out << "keyboard." << std::hex << std::uppercase << row << '.' << slot << "\t";
        if (code < 0) out << "-1";
        else out << "0x" << code;
        out << std::dec << "\n";
    };
    auto buttonCode = [&](SDL_GameControllerButton button) {
        const SDL_GameControllerButtonBind bind = SDL_GameControllerGetBindForButton(controller, button);
        if (bind.bindType == SDL_CONTROLLER_BINDTYPE_BUTTON && bind.value.button < 16) return 0xC010 + bind.value.button;
        if (bind.bindType == SDL_CONTROLLER_BINDTYPE_HAT && bind.value.hat.hat < 2) {
            const int mask = bind.value.hat.hat_mask;
            const int side = mask & SDL_HAT_RIGHT ? 0 : mask & SDL_HAT_UP ? 1 : mask & SDL_HAT_LEFT ? 2 : 3;
            return 0xC020 + 4 * bind.value.hat.hat + side;
        }
        return -1;
    };
    auto axisCode = [&](SDL_GameControllerAxis axis, bool positive) {
        const SDL_GameControllerButtonBind bind = SDL_GameControllerGetBindForAxis(controller, axis);
        if (bind.bindType != SDL_CONTROLLER_BINDTYPE_AXIS || bind.value.axis >= 8) return -1;
        return 0xC000 + 2 * bind.value.axis + (positive ? 1 : 0);
    };
    // Left stick in the first place, the D-pad in the second; A and B fire
    key(mine[0], 0, axisCode(SDL_CONTROLLER_AXIS_LEFTY, false));
    key(mine[0], 1, buttonCode(SDL_CONTROLLER_BUTTON_DPAD_UP));
    key(mine[1], 0, axisCode(SDL_CONTROLLER_AXIS_LEFTY, true));
    key(mine[1], 1, buttonCode(SDL_CONTROLLER_BUTTON_DPAD_DOWN));
    key(mine[2], 0, axisCode(SDL_CONTROLLER_AXIS_LEFTX, false));
    key(mine[2], 1, buttonCode(SDL_CONTROLLER_BUTTON_DPAD_LEFT));
    key(mine[3], 0, axisCode(SDL_CONTROLLER_AXIS_LEFTX, true));
    key(mine[3], 1, buttonCode(SDL_CONTROLLER_BUTTON_DPAD_RIGHT));
    key(mine[4], 0, buttonCode(SDL_CONTROLLER_BUTTON_A));
    key(mine[4], 1, buttonCode(SDL_CONTROLLER_BUTTON_B));
    for (int i = 0; i < 5; ++i) key(other[i], 1, -1);
}

} // namespace

Toolchain Toolchain::locate(const fs::path &root, const fs::path &launcherDir, const std::vector<fs::path> &amigaFolders) {
    Toolchain t;
    t.root = root;
    const fs::path tc = root / "toolchain";
    t.x64sc = tc / "vice" / "bin" / "x64sc";
    t.xplus4 = tc / "vice" / "bin" / "xplus4";
    t.yapesdl = tc / "bin" / "yapesdl";
    t.plus4emu = tc / "bin" / "plus4emu";
    t.plus4emuRoms = tc / "rom" / "plus4emu";
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
        if (settings.plus4Emulator == "plus4emu")
            return executable(plus4emu) && fs::exists(plus4emuRoms / "p4_keys.cfg") ? ""
                                                                                  : "plus4emu fehlt – toolchain/setup.sh plus4emu";
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
    case System::plus4:
        if (settings.plus4Emulator == "yape") return "YapeSDL";
        if (settings.plus4Emulator == "plus4emu") return "plus4emu";
        return "VICE · xplus4";
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
    // Where a screenshot in the game goes (View + X, Print or Shift+F12;
    // patches/*-retrodeck-screenshot.patch): the game's picture in the list
    const fs::path png = screenshotPath(game);
    std::error_code ignored;
    fs::create_directories(png.parent_path(), ignored);
    launch.env.emplace_back("RETRODECK_SCREENSHOT", png.string());
    const fs::path first = media.front();
    const std::string extension = lower(first.extension().string());

    if (game.system == System::c64 || (game.system == System::plus4 && settings.plus4Emulator != "yape" &&
                                         settings.plus4Emulator != "plus4emu")) {
        const bool c64 = game.system == System::c64;
        const fs::path dir = configDir() / "vice";
        fs::create_directories(dir, ignored);
        // VICE's own settings (saved from its menu) live in sdl-vicerc;
        // what RetroDeck sets for this run comes on top from retrodeck.rc
        const fs::path extra = dir / "retrodeck.rc", joymap = dir / "retrodeck.vjm";
        {
            std::ofstream rc(extra);
            rc << "[" << (c64 ? "C64SC" : "PLUS4") << "]\n";
            // F12 is RetroDeck's (patches/vice-retrodeck-f12.patch): VICE's
            // menu on F10 (291, as VICE numbers keys)
            rc << "MenuKey=291\n";
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
        if (settings.plus4Emulator == "plus4emu") {
            // Its own folder instead of ~/.plus4emu (patches/plus4emu-home.patch)
            const fs::path home = configDir() / "plus4emu", cfg = home / "retrodeck.cfg";
            fs::create_directories(home, ignored);
            writePlus4emuConfig(cfg, toolchain.plus4emuRoms, pad && view.index >= 0 ? pad->controller : nullptr, settings.joyPort);
            launch.argv = {toolchain.plus4emu.string(), "-cfg", (toolchain.plus4emuRoms / "p4_keys.cfg").string(),
                           "-cfg", cfg.string(), first.string()};
            launch.env.emplace_back("PLUS4EMU_HOME", home.string());
            launch.env.emplace_back("PLUS4EMU_FULLSCREEN", "1");
            // PortAudio's default output (PipeWire), not the first device of
            // its list, a raw HDMI one (patches/plus4emu-default-audio.patch)
            launch.env.emplace_back("PLUS4EMU_DEFAULT_AUDIO", "1");
            if (settings.loadTurbo) launch.env.emplace_back("PLUS4EMU_LOAD_TURBO", "1");
            break;
        }
        launch.argv = {toolchain.yapesdl.string(), first.string()};
        launch.env.emplace_back("YAPE_FULLSCREEN", "1");
        launch.env.emplace_back("XDG_DATA_HOME", yapeDataHome().string());
        // The gamepad's buttons only fire, not Yape's own functions (B
        // stepped the active joystick on, LB typed RUN, RB opened the menu)
        launch.env.emplace_back("YAPE_PADKEYS", "off");
        if (pad && view.index >= 0) {
            if (const std::string mapping = yapeMapping(pad->controller); !mapping.empty())
                launch.env.emplace_back("SDL_GAMECONTROLLERCONFIG", mapping);
        }
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

void Process::screenshot() {
    if (pid_ > 0 && !stopAt_) ::kill(pid_, SIGUSR2);
}

} // namespace rd
