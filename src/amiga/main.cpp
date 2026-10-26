// retrodeck-amiga: an Amiga 500 in a window (or fullscreen), on the vAmiga
// core, for playing with a controller.
//
//   retrodeck-amiga --kickstart KICK.rom [--model A500_OCS_1MB|A500_ECS_1MB]
//                   [--pad N] [--load-turbo] [--window WxH]
//                   [--frames N --screenshot F.png] DISK1.adf [DISK2.adf ...]
//
// --pad N plays with SDL game controller N only (default: all of them).
// --load-turbo warps while a drive motor runs (the sound is muted then).
// The drive always reads eight times as fast as a real one.
//
// Controller                           Keyboard
//   left stick, D-pad  joystick port 2   Amiga keys as on a PC keyboard
//   A                  fire              F11  fullscreen on/off
//   B / X / Y          Space / Return / F1   F12  quit
//   Menu               P (pause in many games)
//   right stick        mouse (port 1), RT/LT left/right button
//   View + LB/RB       previous/next disk in DF0
//   View + Y           warp on/off
//   View + Menu, held  quit
//   (View alone is the launcher's: it asks whether to end the game)

#include "VAmiga.h"

#include "draw.h"
#include "font.h"
#include "paths.h"

#include <SDL.h>

#include <algorithm>
#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

namespace fs = std::filesystem;
using namespace vamiga;

namespace {

// The part of the texture a PAL monitor shows: 368 x 287 lores pixels. A
// texel is 64 bits, one hires pixel twice, so read as 32-bit values a row
// has four values per lores pixel; one row per line.
constexpr int textureW = 2 * HPIXELS, cropX = 4 * 69, cropY = 0x1a, cropW = 4 * 368, cropH = 287;

std::atomic<bool> stopSignal{false};
void onSignal(int) { stopSignal = true; }

struct Options {
    fs::path kickstart;
    std::string model = "A500_OCS_1MB";
    int pad = -1;
    int windowW = 0, windowH = 0;
    bool loadTurbo = false;
    long frames = 0;
    std::string screenshot;
    std::vector<fs::path> disks;
};

KeyCode amigaKey(SDL_Scancode code, bool &known) {
    known = true;
    if (code >= SDL_SCANCODE_A && code <= SDL_SCANCODE_Z) {
        static const KeyCode letters[26] = {0x20, 0x35, 0x33, 0x22, 0x12, 0x23, 0x24, 0x25, 0x17, 0x26, 0x27, 0x28, 0x37,
                                            0x36, 0x18, 0x19, 0x10, 0x13, 0x21, 0x14, 0x16, 0x34, 0x11, 0x32, 0x15, 0x31};
        return letters[code - SDL_SCANCODE_A];
    }
    if (code >= SDL_SCANCODE_1 && code <= SDL_SCANCODE_0) return static_cast<KeyCode>(0x01 + (code - SDL_SCANCODE_1));
    if (code >= SDL_SCANCODE_F1 && code <= SDL_SCANCODE_F10) return static_cast<KeyCode>(0x50 + (code - SDL_SCANCODE_F1));
    switch (code) {
    case SDL_SCANCODE_GRAVE: return 0x00;
    case SDL_SCANCODE_MINUS: return 0x0b;
    case SDL_SCANCODE_EQUALS: return 0x0c;
    case SDL_SCANCODE_BACKSLASH: return 0x0d;
    case SDL_SCANCODE_LEFTBRACKET: return 0x1a;
    case SDL_SCANCODE_RIGHTBRACKET: return 0x1b;
    case SDL_SCANCODE_SEMICOLON: return 0x29;
    case SDL_SCANCODE_APOSTROPHE: return 0x2a;
    case SDL_SCANCODE_NONUSHASH: return 0x2b;
    case SDL_SCANCODE_NONUSBACKSLASH: return 0x30;
    case SDL_SCANCODE_COMMA: return 0x38;
    case SDL_SCANCODE_PERIOD: return 0x39;
    case SDL_SCANCODE_SLASH: return 0x3a;
    case SDL_SCANCODE_SPACE: return 0x40;
    case SDL_SCANCODE_BACKSPACE: return 0x41;
    case SDL_SCANCODE_TAB: return 0x42;
    case SDL_SCANCODE_KP_ENTER: return 0x43;
    case SDL_SCANCODE_RETURN: return 0x44;
    case SDL_SCANCODE_ESCAPE: return 0x45;
    case SDL_SCANCODE_DELETE: return 0x46;
    case SDL_SCANCODE_KP_MINUS: return 0x4a;
    case SDL_SCANCODE_UP: return 0x4c;
    case SDL_SCANCODE_DOWN: return 0x4d;
    case SDL_SCANCODE_RIGHT: return 0x4e;
    case SDL_SCANCODE_LEFT: return 0x4f;
    case SDL_SCANCODE_KP_0: return 0x0f;
    case SDL_SCANCODE_KP_1: return 0x1d;
    case SDL_SCANCODE_KP_2: return 0x1e;
    case SDL_SCANCODE_KP_3: return 0x1f;
    case SDL_SCANCODE_KP_4: return 0x2d;
    case SDL_SCANCODE_KP_5: return 0x2e;
    case SDL_SCANCODE_KP_6: return 0x2f;
    case SDL_SCANCODE_KP_7: return 0x3d;
    case SDL_SCANCODE_KP_8: return 0x3e;
    case SDL_SCANCODE_KP_9: return 0x3f;
    case SDL_SCANCODE_KP_PERIOD: return 0x3c;
    case SDL_SCANCODE_KP_DIVIDE: return 0x5c;
    case SDL_SCANCODE_KP_MULTIPLY: return 0x5d;
    case SDL_SCANCODE_KP_PLUS: return 0x5e;
    case SDL_SCANCODE_INSERT: return 0x5f;      // Help
    case SDL_SCANCODE_LSHIFT: return 0x60;
    case SDL_SCANCODE_RSHIFT: return 0x61;
    case SDL_SCANCODE_CAPSLOCK: return 0x62;
    case SDL_SCANCODE_LCTRL:
    case SDL_SCANCODE_RCTRL: return 0x63;
    case SDL_SCANCODE_LALT: return 0x64;
    case SDL_SCANCODE_RALT: return 0x65;
    case SDL_SCANCODE_LGUI: return 0x66;
    case SDL_SCANCODE_RGUI: return 0x67;
    default: known = false; return 0;
    }
}

class Frontend {
public:
    explicit Frontend(Options options) : options_(std::move(options)) {}
    ~Frontend();
    int run();

private:
    bool setup();
    void handle(const SDL_Event &event);
    void pollPads();
    void controllerButton(Uint8 button, bool down);
    void key(KeyCode code, bool down);
    void insertDisk(int index);
    void say(const std::string &text, Uint32 ms = 2500);
    void draw();
    bool ours(SDL_JoystickID id) const;
    static void audioCallback(void *user, Uint8 *stream, int length);
    static void message(const void *, Message, std::exception_ptr) {}
    Warp baseWarp() const { return options_.loadTurbo ? Warp::AUTO : Warp::NEVER; }

    Options options_;
    VAmiga amiga_;
    SDL_Window *window_ = nullptr;
    SDL_Renderer *renderer_ = nullptr;
    SDL_Texture *frame_ = nullptr, *sharp_ = nullptr;
    int sharpW_ = 0, sharpH_ = 0;
    SDL_AudioDeviceID audio_ = 0;
    std::vector<SDL_GameController *> pads_;
    rd::Fonts *fonts_ = nullptr;
    int font_ = -1;
    std::string osd_;
    Uint32 osdUntil_ = 0;
    int disk_ = 0;
    bool warp_ = false, quit_ = false, launched_ = false;
    // vAmiga's thread computes the frames that are due whenever it is woken
    // and waits up to 50 ms otherwise; woken this often, every frame (and
    // its sound) comes on time instead of in bursts
    std::thread waker_;
    std::atomic<bool> waking_{false};
    Uint32 comboSince_ = 0;
    int joyX_ = 0, joyY_ = 0;
    bool mouseLeft_ = false, mouseRight_ = false;
};

Frontend::~Frontend() {
    // The audio callback and the waker use the emulator: stop them first
    if (audio_) SDL_CloseAudioDevice(audio_);
    waking_ = false;
    if (waker_.joinable()) waker_.join();
    if (launched_) amiga_.halt();
    for (SDL_GameController *pad : pads_) SDL_GameControllerClose(pad);
    delete fonts_;
    if (sharp_) SDL_DestroyTexture(sharp_);
    if (frame_) SDL_DestroyTexture(frame_);
    if (renderer_) SDL_DestroyRenderer(renderer_);
    if (window_) SDL_DestroyWindow(window_);
    SDL_Quit();
}

bool Frontend::setup() {
    SDL_SetHint(SDL_HINT_VIDEO_MINIMIZE_ON_FOCUS_LOSS, "0");
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER) != 0) {
        std::fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return false;
    }
    const bool windowed = options_.windowW > 0 && options_.windowH > 0;
    window_ = SDL_CreateWindow("Amiga – RetroDeck", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                               windowed ? options_.windowW : 1280, windowed ? options_.windowH : 800,
                               SDL_WINDOW_ALLOW_HIGHDPI | (windowed ? SDL_WINDOW_RESIZABLE : SDL_WINDOW_FULLSCREEN_DESKTOP));
    if (!window_) return false;
    renderer_ = SDL_CreateRenderer(window_, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC | SDL_RENDERER_TARGETTEXTURE);
    if (!renderer_) renderer_ = SDL_CreateRenderer(window_, -1, SDL_RENDERER_TARGETTEXTURE);
    if (!renderer_) return false;
    frame_ = SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STREAMING, cropW, cropH);
    SDL_SetTextureScaleMode(frame_, SDL_ScaleModeNearest);
    SDL_ShowCursor(SDL_DISABLE);
    if (!windowed) SDL_SetRelativeMouseMode(SDL_TRUE);

    fonts_ = new rd::Fonts(renderer_);
    font_ = fonts_->load(rd::findRoot() / "assets" / "fonts" / "TitilliumWeb-SemiBold.ttf");

    for (int i = 0; i < SDL_NumJoysticks(); ++i) {
        if (!SDL_IsGameController(i) || (options_.pad >= 0 && i != options_.pad)) continue;
        if (SDL_GameController *pad = SDL_GameControllerOpen(i)) pads_.push_back(pad);
    }

    SDL_AudioSpec want{}, have{};
    want.freq = 48000;
    want.format = AUDIO_F32SYS;
    want.channels = 2;
    want.samples = 1024;
    want.callback = audioCallback;
    want.userdata = this;
    audio_ = SDL_OpenAudioDevice(nullptr, 0, &want, &have, SDL_AUDIO_ALLOW_FREQUENCY_CHANGE);

    try {
        amiga_.set(options_.model == "A500_ECS_1MB" ? ConfigScheme::A500_ECS_1MB : ConfigScheme::A500_OCS_1MB);
        amiga_.set(Opt::HOST_TEX_FORMAT, static_cast<i64>(TexFormat::ABGR));
        if (audio_) amiga_.set(Opt::HOST_SAMPLE_RATE, have.freq);
        // Disk DMA eight words a slot: loads faster, the sound unchanged
        // (Flashback: 31 s to the protection screen instead of 46 s; the
        // turbo drive, -1, hangs it)
        amiga_.set(Opt::DC_SPEED, 8);
        // Warp while the drive runs only with --load-turbo: it mutes the
        // sound, and many games play music while they load
        amiga_.set(Opt::AMIGA_WARP_MODE, static_cast<i64>(baseWarp()));
        amiga_.mem.loadRom(options_.kickstart);
        if (!options_.disks.empty()) amiga_.df0.insert(options_.disks.front(), false);
        amiga_.launch(this, message);
        launched_ = true;
        amiga_.run();
        waking_ = true;
        waker_ = std::thread([this] {
            while (waking_) {
                amiga_.wakeUp();
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
        });
    } catch (const std::exception &error) {
        std::fprintf(stderr, "vAmiga: %s\n", error.what());
        say(std::string("Amiga startet nicht: ") + error.what(), 5000);
        return true;
    }
    if (audio_) SDL_PauseAudioDevice(audio_, 0);
    say(options_.disks.size() > 1 ? "View: Beenden  ·  View + LB/RB: Disk wechseln"
                                   : "View: Beenden  ·  View + Y: Turbo",
        5000);
    return true;
}

void Frontend::audioCallback(void *user, Uint8 *stream, int length) {
    auto *self = static_cast<Frontend *>(user);
    auto *out = reinterpret_cast<float *>(stream);
    const isize frames = length / static_cast<int>(2 * sizeof(float));
    const isize copied = self->launched_ ? self->amiga_.audioPort.copyInterleaved(out, frames) : 0;
    std::fill(out + 2 * copied, out + 2 * frames, 0.0f);
}

void Frontend::say(const std::string &text, Uint32 ms) {
    osd_ = text;
    osdUntil_ = SDL_GetTicks() + ms;
}

bool Frontend::ours(SDL_JoystickID id) const {
    for (SDL_GameController *pad : pads_) {
        if (SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(pad)) == id) return true;
    }
    return false;
}

void Frontend::key(KeyCode code, bool down) {
    if (!launched_) return;
    if (down) amiga_.keyboard.press(code);
    else amiga_.keyboard.release(code);
}

void Frontend::insertDisk(int index) {
    if (options_.disks.size() < 2 || !launched_) return;
    const int count = static_cast<int>(options_.disks.size());
    disk_ = (index + count) % count;
    try {
        amiga_.df0.insert(options_.disks[static_cast<std::size_t>(disk_)], false);
        say("DF0: Disk " + std::to_string(disk_ + 1) + " von " + std::to_string(count) + "  ·  " +
            options_.disks[static_cast<std::size_t>(disk_)].stem().string());
    } catch (const std::exception &error) {
        say(std::string("Disk: ") + error.what());
    }
}

bool viewHeld(const std::vector<SDL_GameController *> &pads) {
    for (SDL_GameController *pad : pads) if (SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_BACK)) return true;
    return false;
}

void Frontend::controllerButton(Uint8 button, bool down) {
    const bool view = viewHeld(pads_);
    if (down && view) {
        switch (button) {
        case SDL_CONTROLLER_BUTTON_LEFTSHOULDER: insertDisk(disk_ - 1); return;
        case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: insertDisk(disk_ + 1); return;
        case SDL_CONTROLLER_BUTTON_Y:
            warp_ = !warp_;
            amiga_.set(Opt::AMIGA_WARP_MODE, static_cast<i64>(warp_ ? Warp::ALWAYS : baseWarp()));
            say(warp_ ? "Turbo an" : "Turbo aus");
            return;
        case SDL_CONTROLLER_BUTTON_START: return; // the quit combination
        default: break;
        }
    }
    switch (button) {
    case SDL_CONTROLLER_BUTTON_A:
        if (launched_) amiga_.controlPort2.joystick.trigger(down ? GamePadAction::PRESS_FIRE : GamePadAction::RELEASE_FIRE);
        break;
    case SDL_CONTROLLER_BUTTON_B: key(0x40, down); break;   // Space
    case SDL_CONTROLLER_BUTTON_X: key(0x44, down); break;   // Return
    case SDL_CONTROLLER_BUTTON_Y: key(0x50, down); break;   // F1
    case SDL_CONTROLLER_BUTTON_START: key(0x19, down); break; // P
    default: break;
    }
}

void Frontend::pollPads() {
    if (!launched_) return;
    // Directions from all sticks and D-pads together, sent when they change
    int x = 0, y = 0;
    float mx = 0, my = 0;
    bool left = false, right = false, quitCombo = false;
    for (SDL_GameController *pad : pads_) {
        auto axis = [&](SDL_GameControllerAxis a) { return SDL_GameControllerGetAxis(pad, a); };
        auto button = [&](SDL_GameControllerButton b) { return SDL_GameControllerGetButton(pad, b) != 0; };
        if (axis(SDL_CONTROLLER_AXIS_LEFTX) < -14000 || button(SDL_CONTROLLER_BUTTON_DPAD_LEFT)) x = -1;
        if (axis(SDL_CONTROLLER_AXIS_LEFTX) > 14000 || button(SDL_CONTROLLER_BUTTON_DPAD_RIGHT)) x = 1;
        if (axis(SDL_CONTROLLER_AXIS_LEFTY) < -14000 || button(SDL_CONTROLLER_BUTTON_DPAD_UP)) y = -1;
        if (axis(SDL_CONTROLLER_AXIS_LEFTY) > 14000 || button(SDL_CONTROLLER_BUTTON_DPAD_DOWN)) y = 1;
        // Right stick: mouse, faster the further it is pushed
        for (auto [a, value] : {std::pair{SDL_CONTROLLER_AXIS_RIGHTX, &mx}, std::pair{SDL_CONTROLLER_AXIS_RIGHTY, &my}}) {
            const int v = axis(a);
            if (std::abs(v) > 6000) *value += (v / 32768.0f) * std::abs(v / 32768.0f) * 12.0f;
        }
        left |= axis(SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > 12000;
        right |= axis(SDL_CONTROLLER_AXIS_TRIGGERLEFT) > 12000;
        quitCombo |= button(SDL_CONTROLLER_BUTTON_BACK) && button(SDL_CONTROLLER_BUTTON_START);
    }
    auto &joystick = amiga_.controlPort2.joystick;
    if (x != joyX_) {
        joystick.trigger(x < 0 ? GamePadAction::PULL_LEFT : x > 0 ? GamePadAction::PULL_RIGHT : GamePadAction::RELEASE_X);
        joyX_ = x;
    }
    if (y != joyY_) {
        joystick.trigger(y < 0 ? GamePadAction::PULL_UP : y > 0 ? GamePadAction::PULL_DOWN : GamePadAction::RELEASE_Y);
        joyY_ = y;
    }
    auto &mouse = amiga_.controlPort1.mouse;
    if (mx != 0 || my != 0) mouse.setDxDy(mx, my);
    if (left != mouseLeft_) mouse.trigger((mouseLeft_ = left) ? GamePadAction::PRESS_LEFT : GamePadAction::RELEASE_LEFT);
    if (right != mouseRight_) mouse.trigger((mouseRight_ = right) ? GamePadAction::PRESS_RIGHT : GamePadAction::RELEASE_RIGHT);

    if (!quitCombo) {
        comboSince_ = 0;
    } else {
        if (!comboSince_) comboSince_ = SDL_GetTicks();
        if (SDL_GetTicks() - comboSince_ > 800) quit_ = true;
    }
}

void Frontend::handle(const SDL_Event &event) {
    switch (event.type) {
    case SDL_QUIT: quit_ = true; break;
    case SDL_CONTROLLERDEVICEADDED:
        // A controller connected later plays too, unless one was chosen
        if (options_.pad < 0 && SDL_IsGameController(event.cdevice.which)) {
            SDL_GameController *pad = SDL_GameControllerOpen(event.cdevice.which);
            if (pad && std::find(pads_.begin(), pads_.end(), pad) == pads_.end()) pads_.push_back(pad);
        }
        break;
    case SDL_CONTROLLERBUTTONDOWN:
    case SDL_CONTROLLERBUTTONUP:
        if (ours(event.cbutton.which)) controllerButton(event.cbutton.button, event.type == SDL_CONTROLLERBUTTONDOWN);
        break;
    case SDL_KEYDOWN:
    case SDL_KEYUP: {
        const bool down = event.type == SDL_KEYDOWN;
        if (event.key.keysym.scancode == SDL_SCANCODE_F12 && down) quit_ = true;
        if (event.key.keysym.scancode == SDL_SCANCODE_F11 && down) {
            const bool full = SDL_GetWindowFlags(window_) & SDL_WINDOW_FULLSCREEN_DESKTOP;
            SDL_SetWindowFullscreen(window_, full ? 0 : SDL_WINDOW_FULLSCREEN_DESKTOP);
            SDL_SetRelativeMouseMode(full ? SDL_FALSE : SDL_TRUE);
        }
        bool known = false;
        const KeyCode code = amigaKey(event.key.keysym.scancode, known);
        if (known && !event.key.repeat) key(code, down);
        break;
    }
    case SDL_MOUSEMOTION:
        if (launched_ && SDL_GetRelativeMouseMode()) amiga_.controlPort1.mouse.setDxDy(event.motion.xrel, event.motion.yrel);
        break;
    case SDL_MOUSEBUTTONDOWN:
    case SDL_MOUSEBUTTONUP:
        if (launched_ && (event.button.button == SDL_BUTTON_LEFT || event.button.button == SDL_BUTTON_RIGHT)) {
            const bool down = event.type == SDL_MOUSEBUTTONDOWN, leftButton = event.button.button == SDL_BUTTON_LEFT;
            amiga_.controlPort1.mouse.trigger(leftButton ? (down ? GamePadAction::PRESS_LEFT : GamePadAction::RELEASE_LEFT)
                                                         : (down ? GamePadAction::PRESS_RIGHT : GamePadAction::RELEASE_RIGHT));
        }
        break;
    case SDL_RENDER_TARGETS_RESET:
    case SDL_RENDER_DEVICE_RESET:
        if (sharp_) SDL_DestroyTexture(sharp_);
        sharp_ = nullptr;
        fonts_->flush();
        break;
    default: break;
    }
}

void Frontend::draw() {
    int w = 0, h = 0;
    SDL_GetRendererOutputSize(renderer_, &w, &h);
    SDL_SetRenderDrawColor(renderer_, 0, 0, 0, 255);
    SDL_RenderClear(renderer_);
    if (launched_) {
        amiga_.videoPort.lockTexture();
        const u32 *texture = amiga_.videoPort.getTexture();
        SDL_UpdateTexture(frame_, nullptr, texture + cropY * textureW + cropX, textureW * 4);
        amiga_.videoPort.unlockTexture();

        // Lores pixels about square: 368 x 287 shown as 4:3-ish.
        // "Sharp bilinear": scaled up by whole numbers without smoothing
        // first, then smoothly to the screen, so pixels stay crisp but even.
        const SDL_Rect target = rd::fitInto({0, 0, w, h}, cropW / 4, cropH);
        const int kx = std::max(1, (target.w + cropW - 1) / cropW), ky = std::max(1, (target.h + cropH - 1) / cropH);
        if (!sharp_ || sharpW_ != cropW * kx || sharpH_ != cropH * ky) {
            if (sharp_) SDL_DestroyTexture(sharp_);
            sharpW_ = cropW * kx;
            sharpH_ = cropH * ky;
            sharp_ = SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_TARGET, sharpW_, sharpH_);
            if (sharp_) SDL_SetTextureScaleMode(sharp_, SDL_ScaleModeLinear);
        }
        if (sharp_ && SDL_SetRenderTarget(renderer_, sharp_) == 0) {
            SDL_RenderCopy(renderer_, frame_, nullptr, nullptr);
            SDL_SetRenderTarget(renderer_, nullptr);
            SDL_RenderCopy(renderer_, sharp_, nullptr, &target);
        } else {
            SDL_RenderCopy(renderer_, frame_, nullptr, &target);
        }
    }
    if (!osd_.empty() && !SDL_TICKS_PASSED(SDL_GetTicks(), osdUntil_) && font_ >= 0) {
        const float scale = std::min(w / 1280.0f, h / 800.0f);
        const int px = static_cast<int>(20 * scale), pad = static_cast<int>(18 * scale);
        const std::string text = fonts_->fit(font_, px, osd_, w - 4 * pad);
        const int tw = fonts_->width(font_, px, text);
        const SDL_Rect box{(w - tw) / 2 - pad, h - static_cast<int>(84 * scale), tw + 2 * pad, static_cast<int>(46 * scale)};
        rd::fillRounded(renderer_, box, box.h / 2, SDL_Color{20, 25, 33, 220});
        fonts_->draw(font_, px, box.x + pad, box.y + static_cast<int>(8 * scale), text, SDL_Color{236, 240, 245, 255});
    }
}

int Frontend::run() {
    if (!setup()) {
        std::fprintf(stderr, "SDL: %s\n", SDL_GetError());
        return 1;
    }
    long frames = 0;
    while (!quit_ && !stopSignal) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) handle(event);
        pollPads();
        draw();
        ++frames;
        // Test runs may lack vsync (offscreen); keep them near real time
        if (options_.frames > 0) SDL_Delay(16);
        if (options_.frames > 0 && frames >= options_.frames) {
            if (!options_.screenshot.empty()) {
                int w = 0, h = 0;
                SDL_GetRendererOutputSize(renderer_, &w, &h);
                std::vector<unsigned char> pixels(static_cast<std::size_t>(w) * h * 4);
                SDL_RenderReadPixels(renderer_, nullptr, SDL_PIXELFORMAT_RGBA32, pixels.data(), w * 4);
                stbi_write_png(options_.screenshot.c_str(), w, h, 4, pixels.data(), w * 4);
            }
            break;
        }
        SDL_RenderPresent(renderer_);
        // Without a startable machine there is only the message to show
        if (!launched_ && SDL_TICKS_PASSED(SDL_GetTicks(), osdUntil_)) return 2;
    }
    if (launched_) {
        // For the log: gaps in the sound show up here
        const AudioPortMetrics &audio = amiga_.audioPort.getStats();
        std::fprintf(stderr, "audio: %td underflows, %td overflows, fill level %.2f\n", audio.bufferUnderflows,
                     audio.bufferOverflows, audio.fillLevel);
    }
    return 0;
}

} // namespace

int main(int argc, char **argv) {
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
        if (arg == "--kickstart") options.kickstart = next();
        else if (arg == "--model") options.model = next();
        else if (arg == "--pad") options.pad = std::atoi(next().c_str());
        else if (arg == "--load-turbo") options.loadTurbo = true;
        else if (arg == "--window") std::sscanf(next().c_str(), "%dx%d", &options.windowW, &options.windowH);
        else if (arg == "--frames") options.frames = std::atol(next().c_str());
        else if (arg == "--screenshot") options.screenshot = next();
        else options.disks.emplace_back(arg);
    }
    if (options.kickstart.empty()) {
        std::fprintf(stderr, "usage: retrodeck-amiga --kickstart KICK.rom [--model M] [--pad N] [--load-turbo] [--window WxH] DISK...\n");
        return 1;
    }
    std::signal(SIGTERM, onSignal);
    std::signal(SIGINT, onSignal);
    Frontend frontend(std::move(options));
    return frontend.run();
}
