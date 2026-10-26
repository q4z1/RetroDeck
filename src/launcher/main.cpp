// RetroDeck: picks a Plus/4, C64, Amiga or MEGA65 game with a controller
// and starts it in the emulator from toolchain/. Made for the Steam Deck
// (1280x800) and an external screen (1920x1080), in Steam's Game Mode or
// on the desktop.
//
//   retrodeck                      fullscreen on the current screen
//   retrodeck --window 1920x1080   in a window of that size
//   retrodeck --screenshot F.png   draw one frame into F.png and quit
//   retrodeck --dry-run            print how the selected game would start
//   retrodeck --make-screenshots   title screenshots for the games without one;
//                                  with --select, that game's anew, and
//                                  --seconds N waits N seconds of game time
//
// --system N, --select N and --screen settings|quit pick what is shown
// first (for screenshots and --dry-run).

#include "displays.h"
#include "draw.h"
#include "emulators.h"
#include "font.h"
#include "library.h"
#include "pads.h"
#include "paths.h"
#include "settings.h"

#include <SDL.h>

#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <map>
#include <ranges>
#include <string>
#include <vector>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

using namespace rd;

namespace {

// The Plus/4's emulators, in the order X and the settings step through them
std::string nextPlus4Emulator(const std::string &current, int step) {
    static const std::array<const char *, 3> order{"vice", "yape", "plus4emu"};
    int index = 0;
    for (int i = 0; i < 3; ++i) if (current == order[static_cast<std::size_t>(i)]) index = i;
    return order[static_cast<std::size_t>((index + step + 3) % 3)];
}

std::string plus4EmulatorLabel(const std::string &emulator) {
    if (emulator == "yape") return "YapeSDL";
    if (emulator == "plus4emu") return "plus4emu";
    return "VICE";
}

// Colours
constexpr SDL_Color bgTop{12, 15, 21, 255}, bgBottom{22, 27, 36, 255};
constexpr SDL_Color panel{27, 33, 43, 255}, panelHigh{38, 46, 60, 255}, panelLow{20, 25, 33, 255};
constexpr SDL_Color ink{236, 240, 245, 255}, muted{140, 151, 168, 255}, faint{78, 88, 104, 255};
constexpr SDL_Color warn{255, 176, 72, 255}, bad{240, 92, 92, 255};
constexpr SDL_Color buttonA{61, 190, 91, 255}, buttonB{229, 72, 77, 255}, buttonX{58, 141, 255, 255}, buttonY{245, 197, 66, 255};

// What a hint shows: the controller's button, or the key that does the same
// (keyAction) when the keyboard was used last
struct Control {
    const char *button;
    SDL_Color color;
    bool round;
    const char *key;
};
constexpr Control ctlAccept{"A", buttonA, true, "Enter"}, ctlBack{"B", buttonB, true, "Esc"};
constexpr Control ctlFavorite{"Y", buttonY, true, "F"}, ctlOption{"X", buttonX, true, "X"};
constexpr Control ctlMenu{"Menu", muted, false, "M"}, ctlPrevSystem{"LB", muted, false, "Q"}, ctlNextSystem{"RB", muted, false, "E"};

// An emulator asks to end its game (RETRODECK_PID, SIGUSR1: F12 there)
std::atomic<bool> askToEnd{false};
void onAskToEnd(int) { askToEnd = true; }

SDL_Color accent(System system) {
    switch (system) {
    case System::plus4: return {242, 153, 74, 255};
    case System::c64: return {132, 146, 255, 255};
    case System::amiga: return {236, 84, 88, 255};
    case System::mega65: return {61, 214, 181, 255};
    }
    return ink;
}

enum class Action { none, up, down, left, right, pageUp, pageDown, prevSystem, nextSystem, accept, back, option, favorite, menu };
enum class Screen { library, settings, game, quit };

struct Art {
    SDL_Texture *texture = nullptr;
    int w = 0, h = 0;
    Uint32 lastUse = 0;
};

std::string formatBytes(std::uintmax_t bytes) {
    char text[32];
    if (bytes >= 1024 * 1024) std::snprintf(text, sizeof text, "%.1f MB", bytes / (1024.0 * 1024.0));
    else std::snprintf(text, sizeof text, "%.0f KB", std::max(1.0, bytes / 1024.0));
    std::string result = text;
    std::replace(result.begin(), result.end(), '.', ',');
    return result;
}

class App {
public:
    bool init(int argc, char **argv);
    int run();

private:
    // State
    fs::path root_, settingsFile_, gamesFile_;
    Toolchain toolchain_;
    Settings settings_;
    Library library_;
    std::array<std::vector<int>, 4> lists_;
    std::array<int, 4> selected_{};
    std::array<float, 4> scroll_{};
    int system_ = 0;
    Screen screen_ = Screen::library;
    int settingsRow_ = 0;
    int gameRow_ = 0;            // in the game's options (X)
    Pads pads_;
    std::vector<Display> displays_;
    Process process_;
    bool running_ = false, stopRequested_ = false;
    std::string runningTitle_;
    Uint32 comboSince_ = 0;
    // The quit prompt over a running game, opened by View pressed alone
    bool prompt_ = false, viewDown_ = false, viewAlone_ = false;
    Uint32 viewSince_ = 0;
    std::string toast_;
    Uint32 toastUntil_ = 0;
    bool toastError_ = false;
    std::map<std::string, Art> art_;
    Uint32 selectedAt_ = 0;
    bool quit_ = false, dryRun_ = false, makeScreenshots_ = false;
    int screenshotSeconds_ = 0;
    std::string screenshot_;

    // Input repeat for held directions
    Action held_ = Action::none;
    Uint32 heldNext_ = 0;
    int stickX_ = 0, stickY_ = 0;
    // Hints show keys instead of buttons: the keyboard was used last (or
    // there was no controller at the start)
    bool keyboard_ = false;

    // Video
    SDL_Window *window_ = nullptr;
    SDL_Renderer *renderer_ = nullptr;
    Fonts *fonts_ = nullptr;
    int head_ = -1, body_ = -1, semi_ = -1, bold_ = -1;
    int width_ = 1280, height_ = 800;
    float scale_ = 1;

    int S(float value) const { return static_cast<int>(std::lround(value * scale_)); }
    System system() const { return allSystems[static_cast<std::size_t>(system_)]; }
    const Game *current() const;

    void rescan();
    void sortLists();
    void makeScreenshots(bool onlySelected);
    void handle(Action action, SDL_JoystickID from = -1);
    void handleLibrary(Action action);
    void handleSettings(Action action);
    void handleGameOptions(Action action);
    void moveSelection(int delta);
    void changeSystem(int delta);
    void launch();
    void finished();
    std::string targetConnector() const;
    void moveToDisplay(const std::string &connector);
    void say(const std::string &text, bool error = false);
    void pollHeld(Uint32 now);
    bool isHeld(Action action) const;
    void watchCombo(Uint32 now);
    void watchView(Uint8 button, bool down);
    void promptButton(Uint8 button);
    void openPrompt();
    void resumeGame();
    void quitGame();
    Art *artwork(const Game &game);
    void trimArt();

    void draw();
    void drawHeader();
    void drawList(SDL_Rect area);
    void drawDetails(SDL_Rect area);
    void drawFooter();
    void drawSettings();
    void drawGameOptions();
    void drawQuit();
    void drawPrompt();
    void drawToast();
    int glyphWidth(const Control &control) const;
    void glyph(int x, int cy, const Control &control);
    int hint(int x, int cy, const Control &control, const std::string &text);
    int hintWidth(const Control &control, const std::string &text) const;
    void saveScreenshot();
};

// ---------------------------------------------------------------- setup

bool App::init(int argc, char **argv) {
    int windowW = 0, windowH = 0, startSystem = -1, startSelect = -1;
    std::string startScreen;
    bool forceKeyboard = false; // hints with keys (for screenshots)
    for (int i = 1; i < argc; ++i) {
        const bool more = i + 1 < argc;
        if (!std::strcmp(argv[i], "--window") && more) std::sscanf(argv[++i], "%dx%d", &windowW, &windowH);
        else if (!std::strcmp(argv[i], "--screenshot") && more) screenshot_ = argv[++i];
        else if (!std::strcmp(argv[i], "--system") && more) startSystem = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--select") && more) startSelect = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--screen") && more) startScreen = argv[++i];
        else if (!std::strcmp(argv[i], "--dry-run")) dryRun_ = true;
        else if (!std::strcmp(argv[i], "--keyboard")) forceKeyboard = true;
        else if (!std::strcmp(argv[i], "--make-screenshots")) makeScreenshots_ = true;
        else if (!std::strcmp(argv[i], "--seconds") && more) screenshotSeconds_ = std::atoi(argv[++i]);
    }
    root_ = findRoot();
    toolchain_ = Toolchain::locate(root_, executableDir(), libraryFolders(root_, root_ / "library.conf", System::amiga));
    settingsFile_ = configDir() / "settings.conf";
    settings_.load(settingsFile_);
    gamesFile_ = configDir() / "games.conf";
    settings_.loadGames(gamesFile_);

    // Controllers keep working while an emulator has the focus (for the
    // quit combination) and while the window is hidden
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    // The emulators (RetroDeck's builds) send SIGUSR1 here for F12, which
    // asks whether to end the game as View does
    setenv("RETRODECK_PID", std::to_string(getpid()).c_str(), 1);
    std::signal(SIGUSR1, onAskToEnd);
    SDL_SetHint(SDL_HINT_VIDEO_MINIMIZE_ON_FOCUS_LOSS, "0");
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "linear");
    if (dryRun_ || makeScreenshots_) SDL_SetHint(SDL_HINT_VIDEODRIVER, "offscreen");
    // Named as the desktop entry (retrodeck.desktop, StartupWMClass), so the
    // desktop shows its icon for the window: SDL 3's app id (through
    // sdl2-compat), SDL 2's window classes
    SDL_SetHint("SDL_APP_ID", "retrodeck");
    SDL_SetHint("SDL_VIDEO_WAYLAND_WMCLASS", "retrodeck");
    SDL_SetHint("SDL_VIDEO_X11_WMCLASS", "retrodeck");
    // Open on the screen chosen in the settings (it becomes SDL's first)
    if (settings_.display != "auto") setenv("SDL_VIDEO_DISPLAY_PRIORITY", settings_.display.c_str(), 1);
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER | SDL_INIT_EVENTS) != 0) {
        std::fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return false;
    }
    keyboard_ = forceKeyboard || SDL_NumJoysticks() == 0;
    Uint32 flags = SDL_WINDOW_ALLOW_HIGHDPI;
    if (windowW > 0 && windowH > 0) {
        flags |= SDL_WINDOW_RESIZABLE;
    } else {
        flags |= SDL_WINDOW_FULLSCREEN_DESKTOP;
        windowW = 1280;
        windowH = 800;
    }
    window_ = SDL_CreateWindow("RetroDeck", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, windowW, windowH, flags);
    if (!window_) {
        std::fprintf(stderr, "SDL_CreateWindow: %s\n", SDL_GetError());
        return false;
    }
    if (SDL_Surface *icon = loadSurface(root_ / "assets" / "icon" / "retrodeck.png")) {
        SDL_SetWindowIcon(window_, icon);
        SDL_FreeSurface(icon);
    }
    renderer_ = SDL_CreateRenderer(window_, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!renderer_) renderer_ = SDL_CreateRenderer(window_, -1, 0);
    if (!renderer_) {
        std::fprintf(stderr, "SDL_CreateRenderer: %s\n", SDL_GetError());
        return false;
    }
    SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_BLEND);
    SDL_ShowCursor(SDL_DISABLE);

    fonts_ = new Fonts(renderer_);
    const fs::path fonts = root_ / "assets" / "fonts";
    head_ = fonts_->load(fonts / "Rajdhani-Bold.ttf");
    body_ = fonts_->load(fonts / "TitilliumWeb-Regular.ttf");
    semi_ = fonts_->load(fonts / "TitilliumWeb-SemiBold.ttf");
    bold_ = fonts_->load(fonts / "TitilliumWeb-Bold.ttf");
    if (body_ < 0) {
        std::fprintf(stderr, "Fonts missing in %s\n", fonts.c_str());
        return false;
    }
    if (head_ < 0) head_ = body_;
    if (semi_ < 0) semi_ = body_;
    if (bold_ < 0) bold_ = semi_;

    for (int i = 0; i < SDL_NumJoysticks(); ++i) pads_.open(i);
    displays_ = displays();
    system_ = std::clamp(startSystem >= 0 ? startSystem : settings_.lastSystem, 0, 3);
    rescan();
    if (startSelect >= 0) selected_[static_cast<std::size_t>(system_)] = startSelect;
    if (startScreen == "settings") screen_ = Screen::settings;
    if (startScreen == "game" && current()) screen_ = Screen::game;
    if (startScreen == "quit") screen_ = Screen::quit;
    if (startScreen == "prompt" && current()) {
        prompt_ = true;
        runningTitle_ = current()->title;
        settings_.lastGame = current()->key;
    }
    if (makeScreenshots_) {
        makeScreenshots(startSelect >= 0);
        return false;
    }
    if (dryRun_) {
        // How the selected game would start, without starting it
        const Game *game = current();
        if (!game) return false;
        std::string error;
        const std::vector<fs::path> media = mediaFiles(*game, cacheDir() / "unpacked", error);
        const Pad *pad = pads_.active(settings_.pad);
        const Launch plan = planLaunch(*game, media, settings_.forGame(game->key), toolchain_, pad, pads_.childView(pad), targetConnector(), error);
        std::printf("%s (%s)\n", game->title.c_str(), game->key.c_str());
        for (const Display &display : displays_)
            std::printf("display %d: %s [%s]\n", display.index, display.name.c_str(), display.connector.c_str());
        if (!error.empty()) std::printf("error: %s\n", error.c_str());
        for (const auto &[key, value] : plan.env) std::printf("env %s=%s\n", key.c_str(), value.c_str());
        for (const std::string &arg : plan.argv) std::printf("arg %s\n", arg.c_str());
        return false;
    }
    return true;
}

void App::rescan() {
    library_ = scanLibrary(root_, root_ / "library.conf");
    for (Art &art : std::views::values(art_)) if (art.texture) SDL_DestroyTexture(art.texture);
    art_.clear();
    sortLists();
    // Back to the game played last
    for (std::size_t s = 0; s < lists_.size(); ++s) {
        selected_[s] = 0;
        for (std::size_t i = 0; i < lists_[s].size(); ++i) {
            if (library_.games[static_cast<std::size_t>(lists_[s][i])].key == settings_.lastGame) selected_[s] = static_cast<int>(i);
        }
    }
    if (!library_.notes.empty()) say(library_.notes.front(), true);
}

// Runs each game without a screen until its title (more or less) and keeps
// the last picture as its screenshot
void App::makeScreenshots(bool onlySelected) {
    std::vector<const Game *> games;
    if (onlySelected) {
        if (const Game *game = current()) games.push_back(game);
    } else {
        std::error_code error;
        for (const Game &game : library_.games) {
            if (!fs::exists(screenshotPath(game), error)) games.push_back(&game);
        }
    }
    if (games.empty()) std::printf("Alle Spiele haben schon einen Screenshot.\n");
    for (const Game *game : games) {
        const fs::path png = screenshotPath(*game);
        // --seconds, else the game's own (its options), else the system's
        const auto options = settings_.games.find(game->key);
        const int own = options != settings_.games.end() ? options->second.screenshotSeconds : 0;
        const int seconds = screenshotSeconds_ > 0 ? screenshotSeconds_ : own > 0 ? own : screenshotSeconds(game->system);
        std::printf("%-8s %-32s %3d s ... ", systemName(game->system), game->title.c_str(), seconds);
        std::fflush(stdout);
        std::string error;
        const std::vector<fs::path> media = mediaFiles(*game, cacheDir() / "unpacked", error);
        Launch plan;
        if (!media.empty()) plan = planScreenshot(*game, media, settings_.forGame(game->key), toolchain_, png, seconds, error);
        std::error_code ignored;
        fs::create_directories(png.parent_path(), ignored);
        fs::remove(png, ignored);
        Process process;
        if (plan.argv.empty() || !process.start(plan, cacheDir() / "screenshot.log", error)) {
            std::printf("%s\n", error.c_str());
            continue;
        }
        const Uint32 started = SDL_GetTicks();
        while (process.running()) {
            if (plan.timeout > 0 && SDL_GetTicks() - started > static_cast<Uint32>(plan.timeout) * 1000) process.stop();
            SDL_Delay(100);
        }
        if (fs::exists(png, ignored)) std::printf("%s\n", png.c_str());
        else std::printf("kein Bild (siehe %s)\n", (cacheDir() / "screenshot.log").c_str());
    }
}

void App::sortLists() {
    const std::string keep = current() ? current()->key : "";
    for (auto &list : lists_) list.clear();
    for (std::size_t i = 0; i < library_.games.size(); ++i) {
        lists_[static_cast<std::size_t>(library_.games[i].system)].push_back(static_cast<int>(i));
    }
    if (settings_.favoritesFirst) {
        for (auto &list : lists_) {
            std::stable_partition(list.begin(), list.end(), [&](int index) {
                return settings_.favorites.count(library_.games[static_cast<std::size_t>(index)].key) > 0;
            });
        }
    }
    auto &list = lists_[static_cast<std::size_t>(system_)];
    for (std::size_t i = 0; i < list.size(); ++i) {
        if (library_.games[static_cast<std::size_t>(list[i])].key == keep) selected_[static_cast<std::size_t>(system_)] = static_cast<int>(i);
    }
}

const Game *App::current() const {
    const auto &list = lists_[static_cast<std::size_t>(system_)];
    const int index = selected_[static_cast<std::size_t>(system_)];
    if (index < 0 || index >= static_cast<int>(list.size())) return nullptr;
    return &library_.games[static_cast<std::size_t>(list[static_cast<std::size_t>(index)])];
}

void App::say(const std::string &text, bool error) {
    toast_ = text;
    toastError_ = error;
    toastUntil_ = SDL_GetTicks() + (error ? 6000 : 3000);
}

// ---------------------------------------------------------------- input

Action keyAction(SDL_Keycode key, Uint16 mod) {
    switch (key) {
    case SDLK_UP: return Action::up;
    case SDLK_DOWN: return Action::down;
    case SDLK_LEFT: return Action::left;
    case SDLK_RIGHT: return Action::right;
    case SDLK_PAGEUP: return Action::pageUp;
    case SDLK_PAGEDOWN: return Action::pageDown;
    case SDLK_TAB: return (mod & KMOD_SHIFT) ? Action::prevSystem : Action::nextSystem;
    case SDLK_q: return Action::prevSystem;
    case SDLK_e: return Action::nextSystem;
    case SDLK_RETURN:
    case SDLK_KP_ENTER:
    case SDLK_SPACE: return Action::accept;
    case SDLK_ESCAPE:
    case SDLK_BACKSPACE: return Action::back;
    case SDLK_x: return Action::option;
    case SDLK_f: return Action::favorite;
    case SDLK_m:
    case SDLK_F1: return Action::menu;
    default: return Action::none;
    }
}

Action buttonAction(Uint8 button) {
    switch (button) {
    case SDL_CONTROLLER_BUTTON_DPAD_UP: return Action::up;
    case SDL_CONTROLLER_BUTTON_DPAD_DOWN: return Action::down;
    case SDL_CONTROLLER_BUTTON_DPAD_LEFT: return Action::left;
    case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: return Action::right;
    case SDL_CONTROLLER_BUTTON_LEFTSHOULDER: return Action::prevSystem;
    case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: return Action::nextSystem;
    case SDL_CONTROLLER_BUTTON_A: return Action::accept;
    case SDL_CONTROLLER_BUTTON_B: return Action::back;
    case SDL_CONTROLLER_BUTTON_X: return Action::option;
    case SDL_CONTROLLER_BUTTON_Y: return Action::favorite;
    case SDL_CONTROLLER_BUTTON_START: return Action::menu;
    default: return Action::none;
    }
}

bool repeats(Action action) {
    return action == Action::up || action == Action::down || action == Action::pageUp || action == Action::pageDown ||
           (action == Action::left || action == Action::right);
}

bool App::isHeld(Action action) const {
    const Uint8 *keys = SDL_GetKeyboardState(nullptr);
    switch (action) {
    case Action::up: if (keys[SDL_SCANCODE_UP] || stickY_ < 0) return true; break;
    case Action::down: if (keys[SDL_SCANCODE_DOWN] || stickY_ > 0) return true; break;
    case Action::left: if (keys[SDL_SCANCODE_LEFT] || stickX_ < 0) return true; break;
    case Action::right: if (keys[SDL_SCANCODE_RIGHT] || stickX_ > 0) return true; break;
    case Action::pageUp: if (keys[SDL_SCANCODE_PAGEUP]) return true; break;
    case Action::pageDown: if (keys[SDL_SCANCODE_PAGEDOWN]) return true; break;
    default: return false;
    }
    for (const Pad &pad : pads_.list()) {
        for (int b = 0; b < SDL_CONTROLLER_BUTTON_MAX; ++b) {
            if (buttonAction(static_cast<Uint8>(b)) == action && SDL_GameControllerGetButton(pad.controller, static_cast<SDL_GameControllerButton>(b)))
                return true;
        }
        if (action == Action::pageUp && SDL_GameControllerGetAxis(pad.controller, SDL_CONTROLLER_AXIS_TRIGGERLEFT) > 16000) return true;
        if (action == Action::pageDown && SDL_GameControllerGetAxis(pad.controller, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > 16000) return true;
    }
    return false;
}

void App::pollHeld(Uint32 now) {
    // Sticks and triggers as directions, with an edge per crossing
    int x = 0, y = 0;
    bool pageUp = false, pageDown = false;
    for (const Pad &pad : pads_.list()) {
        const int ax = SDL_GameControllerGetAxis(pad.controller, SDL_CONTROLLER_AXIS_LEFTX);
        const int ay = SDL_GameControllerGetAxis(pad.controller, SDL_CONTROLLER_AXIS_LEFTY);
        if (std::abs(ay) > 20000 && std::abs(ay) > std::abs(ax)) y = ay < 0 ? -1 : 1;
        else if (std::abs(ax) > 20000) x = ax < 0 ? -1 : 1;
        pageUp |= SDL_GameControllerGetAxis(pad.controller, SDL_CONTROLLER_AXIS_TRIGGERLEFT) > 16000;
        pageDown |= SDL_GameControllerGetAxis(pad.controller, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > 16000;
    }
    if (x || y || pageUp || pageDown) keyboard_ = false;
    if (y != stickY_) {
        stickY_ = y;
        if (y) handle(y < 0 ? Action::up : Action::down);
    }
    if (x != stickX_) {
        stickX_ = x;
        if (x) handle(x < 0 ? Action::left : Action::right);
    }
    static bool wasPageUp = false, wasPageDown = false;
    if (pageUp && !wasPageUp) handle(Action::pageUp);
    if (pageDown && !wasPageDown) handle(Action::pageDown);
    wasPageUp = pageUp;
    wasPageDown = pageDown;

    if (held_ != Action::none) {
        if (!isHeld(held_)) {
            held_ = Action::none;
        } else if (SDL_TICKS_PASSED(now, heldNext_)) {
            const Action action = held_;
            handle(action);
            held_ = action;
            heldNext_ = now + (action == Action::left || action == Action::right ? 220 : 70);
        }
    }
}

void App::handle(Action action, SDL_JoystickID from) {
    if (action == Action::none || running_) return;
    if (from >= 0) pads_.used(from);
    if (repeats(action)) {
        held_ = action;
        heldNext_ = SDL_GetTicks() + 380;
    }
    switch (screen_) {
    case Screen::library: handleLibrary(action); break;
    case Screen::settings: handleSettings(action); break;
    case Screen::game: handleGameOptions(action); break;
    case Screen::quit:
        if (action == Action::accept) quit_ = true;
        else if (action == Action::back) screen_ = Screen::library;
        break;
    }
}

void App::moveSelection(int delta) {
    const auto &list = lists_[static_cast<std::size_t>(system_)];
    if (list.empty()) return;
    int &index = selected_[static_cast<std::size_t>(system_)];
    const int last = static_cast<int>(list.size()) - 1;
    // Single steps wrap around, page steps stop at the ends
    if (std::abs(delta) == 1 && index + delta < 0) index = last;
    else if (std::abs(delta) == 1 && index + delta > last) index = 0;
    else index = std::clamp(index + delta, 0, last);
    selectedAt_ = SDL_GetTicks();
}

void App::changeSystem(int delta) {
    system_ = (system_ + delta + 4) % 4;
    selectedAt_ = SDL_GetTicks();
}

void App::handleLibrary(Action action) {
    switch (action) {
    case Action::up: moveSelection(-1); break;
    case Action::down: moveSelection(1); break;
    case Action::pageUp: moveSelection(-8); break;
    case Action::pageDown: moveSelection(8); break;
    case Action::left:
    case Action::prevSystem: changeSystem(-1); held_ = Action::none; break;
    case Action::right:
    case Action::nextSystem: changeSystem(1); held_ = Action::none; break;
    case Action::accept: launch(); break;
    case Action::back: screen_ = Screen::quit; break;
    case Action::menu: screen_ = Screen::settings; settingsRow_ = 0; break;
    case Action::favorite:
        if (const Game *game = current()) {
            const bool added = settings_.favorites.insert(game->key).second;
            if (!added) settings_.favorites.erase(game->key);
            sortLists();
            settings_.save(settingsFile_);
            say(added ? "Zu den Favoriten" : "Aus den Favoriten entfernt");
        }
        break;
    case Action::option:
        if (current()) {
            screen_ = Screen::game;
            gameRow_ = 0;
        }
        break;
    default: break;
    }
}

// The rows of the settings screen
enum SettingsRow { rowPad, rowDisplay, rowPort, rowPlus4, rowAmiga, rowTurbo, rowFavorites, rowRescan, rowQuit, rowCount };

void App::handleSettings(Action action) {
    const int step = action == Action::left ? -1 : action == Action::right || action == Action::accept ? 1 : 0;
    switch (action) {
    case Action::up: settingsRow_ = (settingsRow_ + rowCount - 1) % rowCount; return;
    case Action::down: settingsRow_ = (settingsRow_ + 1) % rowCount; return;
    case Action::back:
    case Action::menu: screen_ = Screen::library; return;
    default: break;
    }
    if (step == 0) return;
    switch (settingsRow_) {
    case rowPad: {
        std::vector<std::string> choices{"auto"};
        for (const Pad &pad : pads_.list()) {
            if (std::find(choices.begin(), choices.end(), pad.name) == choices.end()) choices.push_back(pad.name);
        }
        auto at = std::find(choices.begin(), choices.end(), settings_.pad);
        int index = at == choices.end() ? 0 : static_cast<int>(at - choices.begin());
        index = (index + step + static_cast<int>(choices.size())) % static_cast<int>(choices.size());
        settings_.pad = choices[static_cast<std::size_t>(index)];
        break;
    }
    case rowDisplay: {
        std::vector<std::string> choices{"auto"};
        for (const Display &display : displays_) if (!display.connector.empty()) choices.push_back(display.connector);
        auto at = std::find(choices.begin(), choices.end(), settings_.display);
        int index = at == choices.end() ? 0 : static_cast<int>(at - choices.begin());
        index = (index + step + static_cast<int>(choices.size())) % static_cast<int>(choices.size());
        settings_.display = choices[static_cast<std::size_t>(index)];
        if (settings_.display != "auto") moveToDisplay(settings_.display);
        break;
    }
    case rowPort: settings_.joyPort = settings_.joyPort == 2 ? 1 : 2; break;
    case rowPlus4: settings_.plus4Emulator = nextPlus4Emulator(settings_.plus4Emulator, step); break;
    case rowAmiga: settings_.amigaModel = settings_.amigaModel == "A500_OCS_1MB" ? "A500_ECS_1MB" : "A500_OCS_1MB"; break;
    case rowTurbo: settings_.loadTurbo = !settings_.loadTurbo; break;
    case rowFavorites: settings_.favoritesFirst = !settings_.favoritesFirst; sortLists(); break;
    case rowRescan:
        if (action != Action::accept) return;
        rescan();
        say("Bibliothek neu eingelesen: " + std::to_string(library_.games.size()) + " Spiele");
        screen_ = Screen::library;
        break;
    case rowQuit:
        if (action == Action::accept) quit_ = true;
        return;
    }
    settings_.save(settingsFile_);
}

// ---------------------------------------------------------------- a game's options

// What a game can choose for itself (X), by system: each starts at
// "Vorgabe", the setting, and steps through its own values
enum class GameRow { emulator, joyPort, amigaModel, loadTurbo, screenshot, reset };

std::vector<GameRow> gameRows(System system) {
    switch (system) {
    case System::plus4:
        return {GameRow::emulator, GameRow::joyPort, GameRow::loadTurbo, GameRow::screenshot, GameRow::reset};
    case System::c64: return {GameRow::joyPort, GameRow::loadTurbo, GameRow::screenshot, GameRow::reset};
    case System::amiga: return {GameRow::amigaModel, GameRow::loadTurbo, GameRow::screenshot, GameRow::reset};
    case System::mega65: return {GameRow::screenshot, GameRow::reset};
    }
    return {GameRow::reset};
}

std::string amigaModelLabel(const std::string &model) {
    return model == "A500_ECS_1MB" ? "A500 · ECS" : "A500 · OCS";
}

void App::handleGameOptions(Action action) {
    const Game *game = current();
    if (!game) {
        screen_ = Screen::library;
        return;
    }
    const std::vector<GameRow> rows = gameRows(game->system);
    const int count = static_cast<int>(rows.size());
    const int step = action == Action::left ? -1 : action == Action::right || action == Action::accept ? 1 : 0;
    switch (action) {
    case Action::up: gameRow_ = (gameRow_ + count - 1) % count; return;
    case Action::down: gameRow_ = (gameRow_ + 1) % count; return;
    case Action::back:
    case Action::option: screen_ = Screen::library; return;
    default: break;
    }
    if (step == 0) return;
    gameRow_ = std::clamp(gameRow_, 0, count - 1);
    GameOptions options = settings_.games[game->key];
    auto cycle = [step](int index, int size) { return (index + step + size) % size; };
    switch (rows[static_cast<std::size_t>(gameRow_)]) {
    case GameRow::emulator: {
        static const std::array<const char *, 4> order{"", "vice", "yape", "plus4emu"};
        int index = 0;
        for (int i = 0; i < 4; ++i) if (options.plus4Emulator == order[static_cast<std::size_t>(i)]) index = i;
        options.plus4Emulator = order[static_cast<std::size_t>(cycle(index, 4))];
        break;
    }
    case GameRow::joyPort: options.joyPort = cycle(options.joyPort, 3); break;
    case GameRow::amigaModel: {
        static const std::array<const char *, 3> order{"", "A500_OCS_1MB", "A500_ECS_1MB"};
        int index = 0;
        for (int i = 0; i < 3; ++i) if (options.amigaModel == order[static_cast<std::size_t>(i)]) index = i;
        options.amigaModel = order[static_cast<std::size_t>(cycle(index, 3))];
        break;
    }
    case GameRow::loadTurbo: options.loadTurbo = cycle(options.loadTurbo + 1, 3) - 1; break; // -1, 0, 1
    case GameRow::screenshot:
        // 0 (the system's), then 5 to 300 s in steps of 5
        options.screenshotSeconds = cycle(options.screenshotSeconds / 5, 61) * 5;
        break;
    case GameRow::reset:
        if (action != Action::accept) return;
        options = {};
        say("Alles auf Vorgabe");
        break;
    }
    if (options.empty()) settings_.games.erase(game->key);
    else settings_.games[game->key] = options;
    settings_.saveGames(gamesFile_);
}

void App::drawGameOptions() {
    const Game *game = current();
    if (!game) return;
    const std::vector<GameRow> rows = gameRows(game->system);
    const auto found = settings_.games.find(game->key);
    const GameOptions options = found != settings_.games.end() ? found->second : GameOptions{};
    auto standard = [](const std::string &value) { return "Vorgabe (" + value + ")"; };

    const int rowH = S(52);
    const int w = std::min(width_ - S(80), S(760));
    const int h = std::min(height_ - S(60), S(150) + static_cast<int>(rows.size()) * rowH + S(70));
    fillRect(renderer_, {0, 0, width_, height_}, SDL_Color{0, 0, 0, 160});
    const SDL_Rect box{(width_ - w) / 2, (height_ - h) / 2, w, h};
    fillRounded(renderer_, box, S(20), panel);
    outlineRounded(renderer_, box, S(20), std::max(1, S(2)), withAlpha(accent(system()), 90));
    fonts_->draw(head_, S(36), box.x + S(32), box.y + S(22), "Spiel-Optionen", ink);
    fonts_->draw(semi_, S(20), box.x + S(34), box.y + S(74), fonts_->fit(semi_, S(20), game->title, w - S(68)), muted);

    int y = box.y + S(120);
    for (int i = 0; i < static_cast<int>(rows.size()); ++i) {
        std::string label, value;
        switch (rows[static_cast<std::size_t>(i)]) {
        case GameRow::emulator:
            label = "Plus/4-Emulator";
            value = options.plus4Emulator.empty() ? standard(plus4EmulatorLabel(settings_.plus4Emulator))
                                                  : plus4EmulatorLabel(options.plus4Emulator);
            break;
        case GameRow::joyPort:
            label = "Joystick-Port";
            value = options.joyPort ? "Port " + std::to_string(options.joyPort) : standard("Port " + std::to_string(settings_.joyPort));
            break;
        case GameRow::amigaModel:
            label = "Amiga-Modell";
            value = options.amigaModel.empty() ? standard(amigaModelLabel(settings_.amigaModel)) : amigaModelLabel(options.amigaModel);
            break;
        case GameRow::loadTurbo:
            label = "Turbo beim Laden";
            value = options.loadTurbo < 0 ? standard(settings_.loadTurbo ? "Ja" : "Nein") : options.loadTurbo ? "Ja" : "Nein";
            break;
        case GameRow::screenshot:
            label = "Screenshot nach";
            value = options.screenshotSeconds ? std::to_string(options.screenshotSeconds) + " s"
                                              : standard(std::to_string(screenshotSeconds(game->system)) + " s");
            break;
        case GameRow::reset: label = "Alles auf Vorgabe"; break;
        }
        const SDL_Rect row{box.x + S(20), y, box.w - S(40), rowH - S(6)};
        const bool selected = i == gameRow_;
        if (selected) {
            fillRounded(renderer_, row, S(12), panelHigh);
            fillRounded(renderer_, {row.x + S(6), row.y + S(12), S(4), row.h - S(24)}, S(2), accent(system()));
        }
        fonts_->draw(semi_, S(21), row.x + S(22), row.y + S(8), label, selected ? ink : muted);
        if (!value.empty()) {
            const std::string shown = fonts_->fit(body_, S(20), value, row.w / 2);
            const int right = row.x + row.w - (selected ? S(44) : S(20));
            const int shownW = fonts_->draw(body_, S(20), right, row.y + S(9), shown, selected ? accent(system()) : SDL_Color{200, 207, 218, 255}, Align::right);
            if (selected) {
                fillArrow(renderer_, right - shownW - S(18), row.y + row.h / 2, S(12), -1, accent(system()));
                fillArrow(renderer_, right + S(18), row.y + row.h / 2, S(12), 1, accent(system()));
            }
        }
        y += rowH;
    }
    const int cy = box.y + box.h - S(34);
    int x = box.x + S(32);
    x += hint(x, cy, ctlAccept, "Ändern");
    hint(x, cy, ctlBack, "Zurück");
    fonts_->draw(body_, S(17), box.x + box.w - S(32), cy - S(12), "Nur für dieses Spiel", faint, Align::right);
}

// ---------------------------------------------------------------- launching

void App::launch() {
    const Game *game = current();
    if (!game) return;
    std::string error;
    const std::vector<fs::path> media = mediaFiles(*game, cacheDir() / "unpacked", error);
    const Pad *pad = pads_.active(settings_.pad);
    const ChildPads view = pads_.childView(pad);
    Launch plan;
    if (!media.empty()) plan = planLaunch(*game, media, settings_.forGame(game->key), toolchain_, pad, view, targetConnector(), error);
    if (plan.argv.empty() || !process_.start(plan, cacheDir() / "last-run.log", error)) {
        say(error, true);
        return;
    }
    running_ = true;
    askToEnd = false;
    stopRequested_ = false;
    runningTitle_ = game->title;
    settings_.lastGame = game->key;
    settings_.lastSystem = system_;
    settings_.save(settingsFile_);
    held_ = Action::none;
    // Out of the way, so the emulator's window gets the screen (Game Mode
    // shows the newest window of the game)
    SDL_HideWindow(window_);
}

// The screen the launcher is on, so the emulator opens there too
std::string App::targetConnector() const {
    const std::string connector = connectorOf(SDL_GetWindowDisplayIndex(window_));
    if (!connector.empty() || settings_.display == "auto") return connector;
    return settings_.display;
}

void App::moveToDisplay(const std::string &connector) {
    const int index = displayIndexOf(connector);
    if (index < 0 || index == SDL_GetWindowDisplayIndex(window_)) return;
    // Under Wayland a fullscreen window only moves by leaving fullscreen,
    // moving, and going fullscreen again on the new screen
    const bool fullscreen = SDL_GetWindowFlags(window_) & SDL_WINDOW_FULLSCREEN_DESKTOP;
    if (fullscreen) {
        SDL_SetWindowFullscreen(window_, 0);
        SDL_PumpEvents();
        SDL_Delay(50);
    }
    SDL_SetWindowPosition(window_, SDL_WINDOWPOS_CENTERED_DISPLAY(index), SDL_WINDOWPOS_CENTERED_DISPLAY(index));
    if (fullscreen) {
        SDL_PumpEvents();
        SDL_Delay(50);
        SDL_SetWindowFullscreen(window_, SDL_WINDOW_FULLSCREEN_DESKTOP);
    }
}

void App::finished() {
    running_ = false;
    prompt_ = viewDown_ = false;
    SDL_ShowWindow(window_);
    SDL_RaiseWindow(window_);
    stickX_ = stickY_ = 0;
    const int code = process_.exitCode();
    if (code != 0 && !stopRequested_) {
        say("Emulator mit Code " + std::to_string(code) + " beendet – Log: ~/.cache/retrodeck/last-run.log", true);
    }
}

// Back (View) + Start (Menu) held for a moment quits the running emulator
void App::watchCombo(Uint32 now) {
    bool both = false;
    for (const Pad &pad : pads_.list()) {
        both |= SDL_GameControllerGetButton(pad.controller, SDL_CONTROLLER_BUTTON_BACK) &&
                SDL_GameControllerGetButton(pad.controller, SDL_CONTROLLER_BUTTON_START);
    }
    if (!both) {
        comboSince_ = 0;
        return;
    }
    if (!comboSince_) comboSince_ = now;
    if (now - comboSince_ > 900 && !stopRequested_) {
        stopRequested_ = true;
        process_.stop();
    }
}

// View pressed and let go again on its own (no other button meanwhile,
// not held long) asks whether to end the game; while asking, View again
// goes back to it
void App::watchView(Uint8 button, bool down) {
    if (button != SDL_CONTROLLER_BUTTON_BACK) {
        if (down) viewAlone_ = false;
        return;
    }
    if (down) {
        viewDown_ = viewAlone_ = true;
        viewSince_ = SDL_GetTicks();
        return;
    }
    const bool alone = viewDown_ && viewAlone_ && SDL_GetTicks() - viewSince_ < 800;
    viewDown_ = false;
    if (!alone || stopRequested_) return;
    if (prompt_) resumeGame();
    else openPrompt();
}

void App::promptButton(Uint8 button) {
    if (button == SDL_CONTROLLER_BUTTON_A) quitGame();
    else if (button == SDL_CONTROLLER_BUTTON_B) resumeGame();
}

void App::openPrompt() {
    // The game stands still while the question is on screen; showing the
    // window again puts it in front of the emulator's (Game Mode shows the
    // window mapped last)
    process_.pause();
    prompt_ = true;
    SDL_ShowWindow(window_);
    SDL_RaiseWindow(window_);
}

void App::resumeGame() {
    prompt_ = false;
    SDL_HideWindow(window_);
    process_.resume();
}

void App::quitGame() {
    prompt_ = false;
    stopRequested_ = true;
    process_.stop();
}

// ---------------------------------------------------------------- main loop

int App::run() {
    Uint32 last = SDL_GetTicks();
    while (!quit_) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            switch (event.type) {
            case SDL_QUIT: if (!running_) quit_ = true; break;
            case SDL_CONTROLLERDEVICEADDED:
                if (pads_.open(event.cdevice.which) && !running_) say(pads_.list().back().name + " verbunden");
                break;
            case SDL_CONTROLLERDEVICEREMOVED:
                if (const Pad *pad = pads_.find(event.cdevice.which); pad && !running_) say(pad->name + " getrennt");
                pads_.close(event.cdevice.which);
                if (pads_.list().empty()) keyboard_ = true;
                break;
            case SDL_CONTROLLERBUTTONDOWN:
                keyboard_ = false;
                if (running_) {
                    pads_.used(event.cbutton.which);
                    if (prompt_) promptButton(event.cbutton.button);
                    watchView(event.cbutton.button, true);
                } else {
                    handle(buttonAction(event.cbutton.button), event.cbutton.which);
                }
                break;
            case SDL_CONTROLLERBUTTONUP:
                if (running_) watchView(event.cbutton.button, false);
                break;
            case SDL_KEYDOWN:
                if (event.key.repeat) break;
                keyboard_ = true;
                if (running_ && prompt_) {
                    if (keyAction(event.key.keysym.sym, event.key.keysym.mod) == Action::accept) quitGame();
                    else if (keyAction(event.key.keysym.sym, event.key.keysym.mod) == Action::back) resumeGame();
                    else if (event.key.keysym.sym == SDLK_F12) resumeGame(); // as View again
                } else if (!running_) {
                    handle(keyAction(event.key.keysym.sym, event.key.keysym.mod));
                }
                break;
            case SDL_DISPLAYEVENT:
                displays_ = displays();
                break;
            case SDL_RENDER_TARGETS_RESET:
            case SDL_RENDER_DEVICE_RESET:
                fonts_->flush();
                for (Art &art : std::views::values(art_)) art.texture = nullptr;
                art_.clear();
                break;
            default: break;
            }
        }
        const Uint32 now = SDL_GetTicks();
        if (running_) {
            watchCombo(now);
            // F12 in the emulator: the question, with keys in its hints
            if (askToEnd.exchange(false) && !prompt_ && !stopRequested_) {
                keyboard_ = true;
                openPrompt();
            }
            if (!process_.running()) finished();
        }
        if (running_ && !prompt_) {
            SDL_Delay(30);
            continue;
        }
        if (!running_) pollHeld(now);

        const float dt = std::min(0.1f, (now - last) / 1000.0f);
        last = now;
        for (std::size_t s = 0; s < scroll_.size(); ++s) {
            // scroll_ eases towards the row the selection needs
            const int rows = std::max(1, (height_ - S(176) - S(72)) / S(76));
            const int count = static_cast<int>(lists_[s].size());
            float target = scroll_[s];
            const int sel = selected_[s];
            if (sel - 1 < target) target = static_cast<float>(std::max(0, sel - 1));
            if (sel + 2 > target + rows) target = static_cast<float>(std::min(std::max(0, count - rows), sel + 2 - rows));
            target = std::clamp(target, 0.0f, static_cast<float>(std::max(0, count - rows)));
            scroll_[s] += (std::round(target) - scroll_[s]) * std::min(1.0f, dt * 16);
            if (std::abs(scroll_[s] - std::round(target)) < 0.01f) scroll_[s] = std::round(target);
        }
        draw();
        if (!screenshot_.empty()) {
            saveScreenshot();
            break;
        }
        SDL_RenderPresent(renderer_);
    }
    settings_.lastSystem = system_;
    settings_.save(settingsFile_);
    for (Art &art : std::views::values(art_)) if (art.texture) SDL_DestroyTexture(art.texture);
    art_.clear();
    pads_.closeAll();
    delete fonts_;
    SDL_DestroyRenderer(renderer_);
    SDL_DestroyWindow(window_);
    SDL_Quit();
    return 0;
}

void App::saveScreenshot() {
    std::vector<unsigned char> pixels(static_cast<std::size_t>(width_) * height_ * 4);
    SDL_RenderReadPixels(renderer_, nullptr, SDL_PIXELFORMAT_RGBA32, pixels.data(), width_ * 4);
    stbi_write_png(screenshot_.c_str(), width_, height_, 4, pixels.data(), width_ * 4);
}

// ---------------------------------------------------------------- artwork

Art *App::artwork(const Game &game) {
    if (game.artwork.empty()) return nullptr;
    const std::string key = game.artwork.string();
    auto found = art_.find(key);
    if (found == art_.end()) {
        // Loaded once the selection rests, so that scrolling stays smooth
        if (!screenshot_.empty() || SDL_GetTicks() - selectedAt_ > 120) {
            Art art;
            art.texture = loadImage(renderer_, game.artwork, art.w, art.h, true);
            found = art_.emplace(key, art).first;
            trimArt();
        } else {
            return nullptr;
        }
    }
    found->second.lastUse = SDL_GetTicks();
    return found->second.texture ? &found->second : nullptr;
}

void App::trimArt() {
    while (art_.size() > 32) {
        auto oldest = std::min_element(art_.begin(), art_.end(), [](const auto &a, const auto &b) { return a.second.lastUse < b.second.lastUse; });
        if (oldest->second.texture) SDL_DestroyTexture(oldest->second.texture);
        art_.erase(oldest);
    }
}

// ---------------------------------------------------------------- drawing

void App::draw() {
    SDL_GetRendererOutputSize(renderer_, &width_, &height_);
    scale_ = std::min(width_ / 1280.0f, height_ / 800.0f);
    fillGradient(renderer_, {0, 0, width_, height_}, bgTop, bgBottom);
    if (prompt_) return drawPrompt();
    // A soft glow in the system's colour behind the header
    fillGradient(renderer_, {0, 0, width_, S(220)}, withAlpha(accent(system()), 34), withAlpha(accent(system()), 0));

    drawHeader();
    const int top = S(176), bottom = height_ - S(72);
    const int margin = S(40), gap = S(24);
    const int listW = static_cast<int>((width_ - 2 * margin - gap) * (width_ * 10 > height_ * 17 ? 0.52f : 0.56f));
    drawList({margin, top, listW, bottom - top});
    drawDetails({margin + listW + gap, top, width_ - 2 * margin - listW - gap, bottom - top - S(8)});
    drawFooter();
    if (screen_ == Screen::settings) drawSettings();
    if (screen_ == Screen::game) drawGameOptions();
    if (screen_ == Screen::quit) drawQuit();
    drawToast();
}

void App::drawHeader() {
    const int margin = S(40);
    const int logo = fonts_->draw(head_, S(40), margin, S(22), "RETRO", ink);
    fonts_->draw(head_, S(40), margin + logo + S(2), S(22), "DECK", accent(system()));

    // Clock
    char clock[8];
    const std::time_t now = std::time(nullptr);
    std::strftime(clock, sizeof clock, "%H:%M", std::localtime(&now));
    const int clockW = fonts_->draw(semi_, S(24), width_ - margin, S(30), clock, ink, Align::right);

    // The controller that plays
    const Pad *pad = pads_.active(settings_.pad);
    const std::string label = pad ? pad->name : "Tastatur";
    const std::string battery = pad ? pad->battery() : "";
    const int textW = std::min(fonts_->width(semi_, S(19), label), S(330));
    const int chipW = textW + S(64) + (battery.empty() ? 0 : fonts_->width(body_, S(16), battery) + S(16));
    const SDL_Rect chip{width_ - margin - clockW - S(24) - chipW, S(26), chipW, S(40)};
    fillRounded(renderer_, chip, S(20), withAlpha(panelHigh, 220));
    fillCircle(renderer_, chip.x + S(20), chip.y + S(20), S(11), pad ? buttonA : buttonX);
    fonts_->draw(head_, S(16), chip.x + S(20), chip.y + S(10), pad ? pad->kind.substr(0, 1) : "T", bgTop, Align::center);
    fonts_->draw(semi_, S(19), chip.x + S(40), chip.y + S(7), fonts_->fit(semi_, S(19), label, S(330)), ink);
    if (!battery.empty()) fonts_->draw(body_, S(16), chip.x + S(40) + textW + S(14), chip.y + S(10), battery, muted);

    // System tabs between the shoulder buttons
    int x = margin;
    const int cy = S(128);
    glyph(x, cy, ctlPrevSystem);
    x += glyphWidth(ctlPrevSystem) + S(12);
    for (System sys : allSystems) {
        const bool active = sys == system();
        const std::string name = systemName(sys);
        const std::string count = std::to_string(lists_[static_cast<std::size_t>(sys)].size());
        const int nameW = fonts_->width(head_, S(26), name), countW = fonts_->width(semi_, S(16), count);
        const SDL_Rect tab{x, cy - S(22), nameW + countW + S(52), S(44)};
        fillRounded(renderer_, tab, S(22), active ? accent(sys) : withAlpha(panel, 230));
        fonts_->draw(head_, S(26), tab.x + S(18), tab.y + S(6), name, active ? bgTop : muted);
        const SDL_Rect badge{tab.x + S(26) + nameW, tab.y + S(11), countW + S(14), S(22)};
        fillRounded(renderer_, badge, S(11), active ? withAlpha(bgTop, 60) : withAlpha(panelHigh, 255));
        fonts_->draw(semi_, S(16), badge.x + badge.w / 2, badge.y - S(2), count, active ? bgTop : faint, Align::center);
        x += tab.w + S(12);
    }
    glyph(x + S(4), cy, ctlNextSystem);
}

void App::drawList(SDL_Rect area) {
    const auto &list = lists_[static_cast<std::size_t>(system_)];
    const SDL_Color color = accent(system());
    if (list.empty()) {
        fillRounded(renderer_, {area.x, area.y, area.w, S(200)}, S(16), withAlpha(panel, 200));
        fonts_->draw(semi_, S(26), area.x + S(32), area.y + S(36), std::string("Keine Spiele für ") + systemName(system()), ink);
        int y = area.y + S(84);
        static const char *const folders[] = {"Plus4", "C64", "Amiga", "MEGA65"};
        const std::string help = std::string("Lege Spiele in ~/Games/Retro/") + folders[system_] +
                                 "/ ab (oder trage Ordner in library.conf ein). Dann im Menü „Bibliothek neu einlesen“.";
        for (const std::string &line : fonts_->wrap(body_, S(19), help, area.w - S(64))) {
            fonts_->draw(body_, S(19), area.x + S(32), y, line, muted);
            y += S(28);
        }
        return;
    }
    const int rowH = S(76);
    const float scroll = scroll_[static_cast<std::size_t>(system_)];
    const int first = static_cast<int>(std::floor(scroll));
    const int selected = selected_[static_cast<std::size_t>(system_)];
    SDL_RenderSetClipRect(renderer_, &area);
    for (int i = first; i < static_cast<int>(list.size()); ++i) {
        const int y = area.y + static_cast<int>(std::lround((i - scroll) * rowH));
        if (y > area.y + area.h) break;
        const Game &game = library_.games[static_cast<std::size_t>(list[static_cast<std::size_t>(i)])];
        const bool active = i == selected;
        const SDL_Rect row{area.x, y, area.w, rowH - S(8)};
        if (active) {
            fillRounded(renderer_, row, S(14), panelHigh);
            outlineRounded(renderer_, row, S(14), std::max(2, S(2)), withAlpha(color, 200));
            fillRounded(renderer_, {row.x + S(8), row.y + S(14), S(5), row.h - S(28)}, S(3), color);
        } else {
            fillRounded(renderer_, row, S(14), withAlpha(panel, 150));
        }
        const bool favorite = settings_.favorites.count(game.key) > 0;
        const int textX = row.x + S(28);
        const int formatW = fonts_->width(semi_, S(16), game.format) + S(20);
        const int titleRoom = row.w - S(28) - formatW - S(24) - (favorite ? S(28) : 0);
        fonts_->draw(semi_, S(25), textX, row.y + S(5), fonts_->fit(semi_, S(25), game.title, titleRoom), active ? ink : SDL_Color{205, 212, 222, 255});
        std::string sub = game.details.empty() ? game.folder : game.details;
        fonts_->draw(body_, S(17), textX, row.y + S(37), fonts_->fit(body_, S(17), sub, row.w - S(60) - formatW), active ? muted : faint);
        // Format badge and favorite star at the right
        const SDL_Rect badge{row.x + row.w - formatW - S(16), row.y + (row.h - S(26)) / 2, formatW, S(26)};
        fillRounded(renderer_, badge, S(8), active ? withAlpha(color, 60) : withAlpha(panelHigh, 200));
        fonts_->draw(semi_, S(16), badge.x + badge.w / 2, badge.y + S(2), game.format, active ? color : muted, Align::center);
        if (favorite) fillStar(renderer_, badge.x - S(24), row.y + row.h / 2, S(11), buttonY);
    }
    SDL_RenderSetClipRect(renderer_, nullptr);
    // Scroll position
    if (static_cast<int>(list.size()) * rowH > area.h) {
        const int trackH = area.h - S(16);
        const float visible = static_cast<float>(area.h) / (list.size() * rowH);
        const int thumbH = std::max(S(40), static_cast<int>(trackH * visible));
        const float maxScroll = std::max(1.0f, list.size() - static_cast<float>(area.h) / rowH);
        const int thumbY = area.y + S(8) + static_cast<int>((trackH - thumbH) * std::clamp(scroll / maxScroll, 0.0f, 1.0f));
        fillRounded(renderer_, {area.x + area.w + S(8), thumbY, S(4), thumbH}, S(2), withAlpha(muted, 120));
    }
}

void App::drawDetails(SDL_Rect area) {
    fillRounded(renderer_, area, S(18), withAlpha(panel, 235));
    const Game *game = current();
    const SDL_Color color = accent(system());
    const int pad = S(20);
    // Artwork, 16:10, or the system's name as a placeholder
    const int artH = std::min(area.h * 46 / 100, (area.w - 2 * pad) * 10 / 16);
    const SDL_Rect art{area.x + pad, area.y + pad, area.w - 2 * pad, artH};
    fillRounded(renderer_, art, S(12), panelLow);
    if (Art *image = game ? artwork(*game) : nullptr) {
        const SDL_Rect fitted = fitInto(art, image->w, image->h);
        SDL_RenderCopy(renderer_, image->texture, nullptr, &fitted);
    } else {
        fillGradient(renderer_, {art.x, art.y + art.h / 2, art.w, art.h / 2}, withAlpha(color, 0), withAlpha(color, 40));
        fonts_->draw(head_, S(64), art.x + art.w / 2, art.y + art.h / 2 - S(44), systemName(system()), withAlpha(color, 110), Align::center);
        if (game) fonts_->draw(semi_, S(20), art.x + art.w / 2, art.y + art.h / 2 + S(26), game->format, withAlpha(ink, 90), Align::center);
    }
    if (!game) return;

    int y = art.y + art.h + S(18);
    const int textW = area.w - 2 * pad;
    const auto titleLines = fonts_->wrap(bold_, S(30), game->title, textW);
    for (std::size_t i = 0; i < std::min<std::size_t>(2, titleLines.size()); ++i) {
        const std::string line = i == 1 && titleLines.size() > 2 ? fonts_->fit(bold_, S(30), titleLines[1] + " …", textW) : titleLines[i];
        fonts_->draw(bold_, S(30), area.x + pad, y, line, ink);
        y += S(38);
    }
    if (!game->details.empty()) {
        fonts_->draw(body_, S(18), area.x + pad, y, fonts_->fit(body_, S(18), game->details, textW), muted);
        y += S(30);
    }
    y += S(8);

    const Settings settings = settings_.forGame(game->key);
    const std::string problem = toolchain_.missing(game->system, settings);
    std::vector<std::pair<std::string, std::string>> facts{
        {"Format", game->format + "  ·  " + formatBytes(game->bytes)},
        {"Emulator", toolchain_.emulatorName(game->system, settings)},
    };
    if (game->system == System::c64 || game->system == System::plus4) facts.emplace_back("Joystick", "Port " + std::to_string(settings.joyPort));
    facts.emplace_back("Ordner", game->folder + (game->archive.empty() ? "" : "  (ZIP)"));
    const int labelW = S(110);
    for (const auto &[label, value] : facts) {
        if (y + S(28) > area.y + area.h - S(56)) break;
        fonts_->draw(semi_, S(17), area.x + pad, y + S(1), label, faint);
        fonts_->draw(body_, S(18), area.x + pad + labelW, y, fonts_->fit(body_, S(18), value, textW - labelW), SDL_Color{200, 207, 218, 255});
        y += S(30);
    }
    // Start hint or what is missing
    const int by = area.y + area.h - S(48);
    if (!problem.empty()) {
        fonts_->draw(semi_, S(18), area.x + pad, by + S(4), fonts_->fit(semi_, S(18), problem, textW), warn);
    } else {
        hint(area.x + pad, by + S(18), ctlAccept, "Starten");
    }
}

// A round button, or a key cap (also for the controller's Menu, LB, RB),
// in the button's colour
int App::glyphWidth(const Control &control) const {
    if (control.round && !keyboard_) return S(30);
    return std::max(S(40), fonts_->width(head_, S(18), keyboard_ ? control.key : control.button) + S(16));
}

void App::glyph(int x, int cy, const Control &control) {
    if (control.round && !keyboard_) {
        fillCircle(renderer_, x + S(15), cy, S(15), control.color);
        fonts_->draw(head_, S(20), x + S(15), cy - S(12), control.button, bgTop, Align::center);
        return;
    }
    const int w = glyphWidth(control);
    fillRounded(renderer_, {x, cy - S(14), w, S(28)}, S(8), withAlpha(control.color, 60));
    outlineRounded(renderer_, {x, cy - S(14), w, S(28)}, S(8), std::max(1, S(2)), withAlpha(control.color, 160));
    fonts_->draw(head_, S(18), x + w / 2, cy - S(11), keyboard_ ? control.key : control.button, ink, Align::center);
}

int App::hint(int x, int cy, const Control &control, const std::string &text) {
    glyph(x, cy, control);
    const int w = fonts_->draw(semi_, S(19), x + glyphWidth(control) + S(10), cy - S(14), text, SDL_Color{205, 212, 222, 255});
    return glyphWidth(control) + S(10) + w + S(30);
}

int App::hintWidth(const Control &control, const std::string &text) const {
    return glyphWidth(control) + S(10) + fonts_->width(semi_, S(19), text);
}

void App::drawFooter() {
    const int y = height_ - S(64);
    fillRect(renderer_, {0, y, width_, height_ - y}, withAlpha(panelLow, 230));
    fillRect(renderer_, {0, y, width_, std::max(1, S(1))}, withAlpha(faint, 120));
    const int cy = y + S(32);
    int x = S(40);
    x += hint(x, cy, ctlAccept, "Starten");
    x += hint(x, cy, ctlFavorite, "Favorit");
    if (current()) x += hint(x, cy, ctlOption, "Optionen");
    x += hint(x, cy, ctlMenu, "Einstellungen");
    hint(x, cy, ctlBack, "Beenden");
    // In-game: how to get back (F12 in all emulators: RETRODECK_PID)
    const std::string back = keyboard_ ? "Im Spiel: F12 = Beenden" : "Im Spiel: View = Beenden";
    fonts_->draw(body_, S(17), width_ - S(40), cy - S(12), back, faint, Align::right);
}

void App::drawSettings() {
    fillRect(renderer_, {0, 0, width_, height_}, SDL_Color{0, 0, 0, 160});
    const int w = std::min(width_ - S(80), S(820)), h = std::min(height_ - S(60), S(740));
    const SDL_Rect box{(width_ - w) / 2, (height_ - h) / 2, w, h};
    fillRounded(renderer_, box, S(20), panel);
    outlineRounded(renderer_, box, S(20), std::max(1, S(2)), withAlpha(accent(system()), 90));
    fonts_->draw(head_, S(36), box.x + S(32), box.y + S(22), "Einstellungen", ink);

    const Pad *active = pads_.active(settings_.pad);
    auto padText = [&] { return settings_.pad == "auto" ? std::string("Automatisch (zuletzt benutzt)") : settings_.pad; };
    auto displayText = [&] {
        if (settings_.display == "auto") return std::string("Wo RetroDeck geöffnet wurde");
        for (const Display &display : displays_) {
            if (display.connector == settings_.display) return display.name + " · " + display.connector;
        }
        return settings_.display + " (nicht verbunden)";
    };
    const std::array<std::pair<std::string, std::string>, rowCount> rows{{
        {"Controller", padText()},
        {"Bildschirm", displayText()},
        {"Joystick-Port (C64, Plus/4)", "Port " + std::to_string(settings_.joyPort)},
        {"Plus/4-Emulator", settings_.plus4Emulator == "vice" ? "VICE (xplus4)" : plus4EmulatorLabel(settings_.plus4Emulator)},
        {"Amiga-Modell", settings_.amigaModel == "A500_ECS_1MB" ? "A500 · ECS · 1 MB Chip" : "A500 · OCS · 512K + 512K"},
        {"Turbo beim Laden", settings_.loadTurbo ? "Ja" : "Nein"},
        {"Favoriten zuerst", settings_.favoritesFirst ? "Ja" : "Nein"},
        {"Bibliothek neu einlesen", ""},
        {"RetroDeck beenden", ""},
    }};
    int y = box.y + S(84);
    const int rowH = S(52);
    for (int i = 0; i < rowCount; ++i) {
        const SDL_Rect row{box.x + S(20), y, box.w - S(40), rowH - S(6)};
        const bool selected = i == settingsRow_;
        if (selected) {
            fillRounded(renderer_, row, S(12), panelHigh);
            fillRounded(renderer_, {row.x + S(6), row.y + S(12), S(4), row.h - S(24)}, S(2), accent(system()));
        }
        fonts_->draw(semi_, S(21), row.x + S(22), row.y + S(8), rows[static_cast<std::size_t>(i)].first, selected ? ink : muted);
        const std::string &value = rows[static_cast<std::size_t>(i)].second;
        if (!value.empty()) {
            const std::string shown = fonts_->fit(body_, S(20), value, row.w / 2);
            const int right = row.x + row.w - (selected ? S(44) : S(20));
            const int shownW = fonts_->draw(body_, S(20), right, row.y + S(9), shown, selected ? accent(system()) : SDL_Color{200, 207, 218, 255}, Align::right);
            if (selected) {
                fillArrow(renderer_, right - shownW - S(18), row.y + row.h / 2, S(12), -1, accent(system()));
                fillArrow(renderer_, right + S(18), row.y + row.h / 2, S(12), 1, accent(system()));
            }
        }
        y += rowH;
    }
    // The connected controllers
    y += S(12);
    fonts_->draw(semi_, S(18), box.x + S(40), y, "Verbundene Controller", faint);
    y += S(32);
    if (pads_.list().empty()) {
        fonts_->draw(body_, S(19), box.x + S(40), y, "Keiner – Xbox-Controller per Bluetooth koppeln oder Deck-Steuerung nutzen", muted);
    }
    for (const Pad &pad : pads_.list()) {
        if (y > box.y + box.h - S(80)) break;
        const bool playing = &pad == active;
        fillCircle(renderer_, box.x + S(50), y + S(14), S(7), playing ? buttonA : faint);
        std::string line = pad.kind + "  ·  " + pad.name;
        if (const std::string battery = pad.battery(); !battery.empty()) line += "  ·  " + battery;
        fonts_->draw(body_, S(19), box.x + S(68), y, fonts_->fit(body_, S(19), line, box.w - S(220)), playing ? ink : muted);
        if (playing) fonts_->draw(semi_, S(17), box.x + box.w - S(40), y + S(2), "spielt", buttonA, Align::right);
        y += S(30);
    }
    const int cy = box.y + box.h - S(34);
    int x = box.x + S(32);
    x += hint(x, cy, ctlAccept, "Ändern");
    x += hint(x, cy, ctlBack, "Zurück");
    fonts_->draw(body_, S(17), box.x + box.w - S(32), cy - S(12),
                 keyboard_ ? "Pfeiltasten links/rechts wechseln den Wert" : "Steuerkreuz links/rechts wechselt den Wert",
                 faint, Align::right);
}

void App::drawQuit() {
    fillRect(renderer_, {0, 0, width_, height_}, SDL_Color{0, 0, 0, 160});
    const int w = S(520), h = S(200);
    const SDL_Rect box{(width_ - w) / 2, (height_ - h) / 2, w, h};
    fillRounded(renderer_, box, S(20), panel);
    fonts_->draw(head_, S(34), box.x + w / 2, box.y + S(32), "RetroDeck beenden?", ink, Align::center);
    const int cy = box.y + h - S(52);
    int x = box.x + (w - hintWidth(ctlAccept, "Beenden") - S(30) - hintWidth(ctlBack, "Abbrechen")) / 2;
    x += hint(x, cy, ctlAccept, "Beenden");
    hint(x, cy, ctlBack, "Abbrechen");
}

// Over a paused game: end it or play on. The game's screenshot, if there
// is one, stands in for the frozen picture behind.
void App::drawPrompt() {
    const SDL_Color color = accent(system());
    for (const Game &game : library_.games) {
        if (game.key != settings_.lastGame) continue;
        if (Art *image = artwork(game)) {
            SDL_SetTextureAlphaMod(image->texture, 70);
            const SDL_Rect fitted = fitInto({0, 0, width_, height_}, image->w, image->h);
            SDL_RenderCopy(renderer_, image->texture, nullptr, &fitted);
            SDL_SetTextureAlphaMod(image->texture, 255);
        }
        break;
    }
    const int w = std::min(width_ - S(80), S(640)), h = S(210);
    const SDL_Rect box{(width_ - w) / 2, (height_ - h) / 2, w, h};
    fillRounded(renderer_, box, S(20), withAlpha(panel, 245));
    outlineRounded(renderer_, box, S(20), std::max(1, S(2)), withAlpha(color, 160));
    fonts_->draw(semi_, S(18), box.x + w / 2, box.y + S(26), "Spiel pausiert", faint, Align::center);
    const std::string title = fonts_->fit(head_, S(34), runningTitle_ + " beenden?", w - S(64));
    fonts_->draw(head_, S(34), box.x + w / 2, box.y + S(62), title, ink, Align::center);
    const int cy = box.y + h - S(56);
    const int buttonsW = hintWidth(ctlAccept, "Beenden") + S(30) + hintWidth(ctlBack, "Weiterspielen");
    int x = box.x + (w - buttonsW) / 2;
    x += hint(x, cy, ctlAccept, "Beenden");
    hint(x, cy, ctlBack, "Weiterspielen");
}

void App::drawToast() {
    if (toast_.empty() || SDL_TICKS_PASSED(SDL_GetTicks(), toastUntil_)) return;
    const int maxW = width_ - S(160);
    const std::string text = fonts_->fit(semi_, S(20), toast_, maxW - S(48));
    const int w = fonts_->width(semi_, S(20), text) + S(48);
    const SDL_Rect box{(width_ - w) / 2, height_ - S(64) - S(68), w, S(48)};
    fillRounded(renderer_, box, S(24), toastError_ ? SDL_Color{70, 30, 34, 245} : SDL_Color{44, 54, 70, 245});
    outlineRounded(renderer_, box, S(24), std::max(1, S(2)), toastError_ ? withAlpha(bad, 200) : withAlpha(accent(system()), 160));
    fonts_->draw(semi_, S(20), box.x + S(24), box.y + S(9), text, ink);
}

} // namespace

int main(int argc, char **argv) {
    App app;
    if (!app.init(argc, argv)) {
        const bool dryRun = std::any_of(argv + 1, argv + argc, [](const char *arg) {
            return !std::strcmp(arg, "--dry-run") || !std::strcmp(arg, "--make-screenshots");
        });
        return dryRun ? 0 : 1;
    }
    return app.run();
}
