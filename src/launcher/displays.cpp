#include "displays.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>

#ifdef RD_WAYLAND
#include <wayland-client.h>
#endif

namespace rd {

namespace {

struct Connector {
    std::string name;    // "DP-4"
    std::string monitor; // the EDID's monitor name, "BenQ GL2480"
    std::string id;      // monitorId of the EDID
};

// Maker (three letters), product code and serial number: the same monitor
// keeps it on any connector. The serial is the EDID's serial-number text
// if it has one, else its 32-bit serial number.
std::string monitorId(const std::vector<unsigned char> &edid, const std::string &serialText) {
    if (edid.size() < 128) return {};
    const unsigned maker = edid[8] << 8 | edid[9];
    const char letters[4] = {static_cast<char>('@' + (maker >> 10 & 31)), static_cast<char>('@' + (maker >> 5 & 31)),
                             static_cast<char>('@' + (maker & 31)), 0};
    const unsigned product = edid[10] | edid[11] << 8;
    const unsigned long serial = edid[12] | edid[13] << 8 | edid[14] << 16 | static_cast<unsigned long>(edid[15]) << 24;
    char id[64];
    if (!serialText.empty())
        std::snprintf(id, sizeof id, "edid:%s-%04X-%s", letters, product, serialText.c_str());
    else
        std::snprintf(id, sizeof id, "edid:%s-%04X-%08lX", letters, product, serial);
    return id;
}

// The connected connectors with their monitor's name and id
std::vector<Connector> connectors() {
    std::vector<Connector> list;
    std::error_code error;
    for (const auto &entry : std::filesystem::directory_iterator("/sys/class/drm", error)) {
        const std::string dir = entry.path().filename().string();
        const auto dash = dir.find('-');
        if (dir.rfind("card", 0) != 0 || dash == std::string::npos) continue;
        std::string status;
        std::ifstream(entry.path() / "status") >> status;
        if (status != "connected") continue;
        Connector connector{dir.substr(dash + 1), {}, {}};
        std::ifstream file(entry.path() / "edid", std::ios::binary);
        const std::vector<unsigned char> edid{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
        // Four 18-byte descriptors from byte 54; 0xfc is the monitor name,
        // 0xff the serial number as text
        std::string serialText;
        for (std::size_t at = 54; edid.size() >= 128 && at <= 108; at += 18) {
            if (edid[at] || edid[at + 1] || edid[at + 2] || (edid[at + 3] != 0xfc && edid[at + 3] != 0xff)) continue;
            std::string text(edid.begin() + static_cast<long>(at) + 5, edid.begin() + static_cast<long>(at) + 18);
            text = text.substr(0, text.find('\n'));
            while (!text.empty() && text.back() == ' ') text.pop_back();
            for (char &c : text) if (static_cast<unsigned char>(c) < 0x20 || static_cast<unsigned char>(c) > 0x7e) c = '_';
            (edid[at + 3] == 0xfc ? connector.monitor : serialText) = text;
        }
        connector.id = monitorId(edid, serialText);
        list.push_back(connector);
    }
    return list;
}

const Connector *match(const std::string &name, const std::vector<Connector> &list) {
    // X11 names start with the connector ("DP-4 24\"")
    const std::string first = name.substr(0, name.find(' '));
    for (const Connector &connector : list) if (connector.name == first) return &connector;
    // Wayland names end with the monitor's name ("BNQ BenQ GL2480"); two
    // monitors of the same model cannot be told apart this way
    const Connector *found = nullptr;
    for (const Connector &connector : list) {
        const std::string &monitor = connector.monitor;
        if (monitor.empty()) continue;
        const bool ends = name.size() > monitor.size() && name.compare(name.size() - monitor.size(), monitor.size(), monitor) == 0 &&
                          name[name.size() - monitor.size() - 1] == ' ';
        if (name == monitor || ends) {
            if (found) return nullptr;
            found = &connector;
        }
    }
    return found;
}

#ifdef RD_WAYLAND
// kde_output_order_v1 (plasma-wayland-protocols): the outputs by name, the
// main one first, then done. Written out here instead of generated, it is
// this small.
const wl_interface *noTypes[] = {nullptr};
const wl_message orderRequests[] = {{"destroy", "", noTypes}};
const wl_message orderEvents[] = {{"output", "s", noTypes}, {"done", "", noTypes}};
const wl_interface kdeOutputOrder = {"kde_output_order_v1", 1, 1, orderRequests, 2, orderEvents};

struct OrderListener {
    void (*output)(void *data, wl_proxy *order, const char *name);
    void (*done)(void *data, wl_proxy *order);
};

struct Order {
    std::vector<std::string> pending, outputs;
    bool done = false;
};

const OrderListener orderListener = {
    [](void *data, wl_proxy *, const char *name) { static_cast<Order *>(data)->pending.emplace_back(name ? name : ""); },
    [](void *data, wl_proxy *) {
        Order *order = static_cast<Order *>(data);
        order->outputs = std::move(order->pending);
        order->pending.clear();
        order->done = true;
    },
};

struct Globals {
    wl_registry *registry = nullptr;
    wl_proxy *order = nullptr;
};

const wl_registry_listener registryListener = {
    [](void *data, wl_registry *registry, uint32_t name, const char *interface, uint32_t) {
        Globals *globals = static_cast<Globals *>(data);
        if (!globals->order && !std::strcmp(interface, kdeOutputOrder.name))
            globals->order = static_cast<wl_proxy *>(wl_registry_bind(registry, name, &kdeOutputOrder, 1));
    },
    [](void *, wl_registry *, uint32_t) {},
};

std::vector<std::string> waylandOutputOrder() {
    if (!std::getenv("WAYLAND_DISPLAY")) return {};
    wl_display *display = wl_display_connect(nullptr);
    if (!display) return {};
    Globals globals;
    Order order;
    globals.registry = wl_display_get_registry(display);
    wl_registry_add_listener(globals.registry, &registryListener, &globals);
    wl_display_roundtrip(display);
    if (globals.order) {
        wl_proxy_add_listener(globals.order, reinterpret_cast<void (**)(void)>(const_cast<OrderListener *>(&orderListener)), &order);
        for (int i = 0; i < 3 && !order.done; ++i) wl_display_roundtrip(display);
        wl_proxy_marshal(globals.order, 0);
        wl_proxy_destroy(globals.order);
    }
    wl_registry_destroy(globals.registry);
    wl_display_disconnect(display);
    return order.outputs;
}
#endif

} // namespace

std::vector<Display> displays() {
    const std::vector<Connector> list = connectors();
    std::vector<Display> result;
    for (int i = 0; i < SDL_GetNumVideoDisplays(); ++i) {
        Display screen;
        screen.index = i;
        const char *name = SDL_GetDisplayName(i);
        screen.name = name ? name : "Bildschirm " + std::to_string(i + 1);
        if (const Connector *connector = match(screen.name, list)) {
            screen.connector = connector->name;
            screen.id = connector->id;
        }
        SDL_GetDisplayBounds(i, &screen.bounds);
        result.push_back(screen);
    }
    return result;
}

std::string connectorOf(int index) {
    const char *name = SDL_GetDisplayName(index);
    if (!name) return {};
    const std::vector<Connector> list = connectors();
    const Connector *connector = match(name, list);
    return connector ? connector->name : "";
}

int displayIndexOf(const std::string &connector) {
    for (const Display &screen : displays()) if (screen.connector == connector) return screen.index;
    return -1;
}

bool isMonitorId(const std::string &setting) { return setting.rfind("edid:", 0) == 0; }

std::string connectorOfMonitor(const std::string &id) {
    if (id.empty()) return {};
    for (const Connector &connector : connectors()) if (connector.id == id) return connector.name;
    return {};
}

std::string monitorOf(const std::string &connector) {
    for (const Connector &entry : connectors()) if (entry.name == connector) return entry.id;
    return {};
}

std::string primaryConnector() {
#ifdef RD_WAYLAND
    const std::vector<Connector> list = connectors();
    for (const std::string &output : waylandOutputOrder()) {
        for (const Connector &connector : list) if (connector.name == output) return output;
    }
#endif
    return {};
}

std::string preferredConnector(const std::string &setting) {
    if (isMonitorId(setting)) {
        if (std::string connector = connectorOfMonitor(setting); !connector.empty()) return connector;
    }
    return primaryConnector();
}

} // namespace rd
