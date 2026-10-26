#include "draw.h"

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#include "stb_image.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace rd {

namespace {

SDL_Vertex vertex(float x, float y, SDL_Color color) {
    return {{x, y}, color, {0, 0}};
}

// SDL_RenderGeometry does not smooth edges: every shape gets a rim one
// pixel wide around it that fades from its colour to transparent
constexpr float rim = 1.0f;

void draw(SDL_Renderer *renderer, const std::vector<SDL_Vertex> &vertices) {
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    SDL_RenderGeometry(renderer, nullptr, vertices.data(), static_cast<int>(vertices.size()), nullptr, 0);
}

// The band between two closed outlines with the same number of points
void band(std::vector<SDL_Vertex> &vertices, const std::vector<SDL_FPoint> &a, SDL_Color colorA,
          const std::vector<SDL_FPoint> &b, SDL_Color colorB) {
    for (std::size_t i = 0; i < a.size(); ++i) {
        const std::size_t j = (i + 1) % a.size();
        vertices.push_back(vertex(a[i].x, a[i].y, colorA));
        vertices.push_back(vertex(a[j].x, a[j].y, colorA));
        vertices.push_back(vertex(b[i].x, b[i].y, colorB));
        vertices.push_back(vertex(b[i].x, b[i].y, colorB));
        vertices.push_back(vertex(a[j].x, a[j].y, colorA));
        vertices.push_back(vertex(b[j].x, b[j].y, colorB));
    }
}

// A filled polygon (points in order) with its rim: each edge pushed out by
// the rim, fading out (convex corners keep a tiny notch, invisible at 1 px)
void fillPolygon(SDL_Renderer *renderer, const std::vector<SDL_FPoint> &points, SDL_FPoint center, SDL_Color color) {
    std::vector<SDL_Vertex> vertices;
    const SDL_Color clear = withAlpha(color, 0);
    float area = 0;
    for (std::size_t i = 0; i < points.size(); ++i) {
        const SDL_FPoint &a = points[i], &b = points[(i + 1) % points.size()];
        vertices.push_back(vertex(center.x, center.y, color));
        vertices.push_back(vertex(a.x, a.y, color));
        vertices.push_back(vertex(b.x, b.y, color));
        area += a.x * b.y - b.x * a.y;
    }
    // Outward is to the left or right of an edge, depending on the winding
    const float side = area > 0 ? 1.0f : -1.0f;
    for (std::size_t i = 0; i < points.size(); ++i) {
        const SDL_FPoint &a = points[i], &b = points[(i + 1) % points.size()];
        const float dx = b.x - a.x, dy = b.y - a.y, length = std::hypot(dx, dy);
        if (length <= 0) continue;
        const float nx = side * dy / length * rim, ny = -side * dx / length * rim;
        vertices.push_back(vertex(a.x, a.y, color));
        vertices.push_back(vertex(b.x, b.y, color));
        vertices.push_back(vertex(a.x + nx, a.y + ny, clear));
        vertices.push_back(vertex(a.x + nx, a.y + ny, clear));
        vertices.push_back(vertex(b.x, b.y, color));
        vertices.push_back(vertex(b.x + nx, b.y + ny, clear));
    }
    draw(renderer, vertices);
}

int cornerSteps(SDL_Rect rect, float radius) {
    return std::clamp(static_cast<int>(std::min(radius, std::min(rect.w, rect.h) / 2.0f) / 2), 2, 16);
}

// The outline of a rounded rectangle, corner by corner, inset by `inset`
std::vector<SDL_FPoint> roundedOutline(SDL_Rect rect, float radius, float inset, int steps) {
    std::vector<SDL_FPoint> points;
    const float r = std::max(0.0f, std::min(radius, std::min(rect.w, rect.h) / 2.0f) - inset);
    const float left = rect.x + inset, top = rect.y + inset;
    const float right = rect.x + rect.w - inset, bottom = rect.y + rect.h - inset;
    const float cx[4] = {right - r, left + r, left + r, right - r};
    const float cy[4] = {top + r, top + r, bottom - r, bottom - r};
    for (int corner = 0; corner < 4; ++corner) {
        for (int step = 0; step <= steps; ++step) {
            const float angle = static_cast<float>(M_PI) / 2 * (corner + static_cast<float>(step) / steps);
            points.push_back({cx[corner] + r * std::cos(angle), cy[corner] - r * std::sin(angle)});
        }
    }
    return points;
}

} // namespace

void fillRect(SDL_Renderer *renderer, SDL_Rect rect, SDL_Color color) {
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(renderer, color.r, color.g, color.b, color.a);
    SDL_RenderFillRect(renderer, &rect);
}

void fillRounded(SDL_Renderer *renderer, SDL_Rect rect, int radius, SDL_Color color) {
    if (rect.w <= 0 || rect.h <= 0) return;
    if (radius <= 0) return fillRect(renderer, rect, color);
    // Solid up to half the rim inside the edge, fading out to half outside
    const int steps = cornerSteps(rect, static_cast<float>(radius));
    const std::vector<SDL_FPoint> solid = roundedOutline(rect, static_cast<float>(radius), rim / 2, steps);
    const std::vector<SDL_FPoint> fade = roundedOutline(rect, static_cast<float>(radius), -rim / 2, steps);
    std::vector<SDL_Vertex> vertices;
    vertices.reserve(solid.size() * 9);
    const SDL_FPoint center{rect.x + rect.w / 2.0f, rect.y + rect.h / 2.0f};
    for (std::size_t i = 0; i < solid.size(); ++i) {
        const SDL_FPoint &a = solid[i], &b = solid[(i + 1) % solid.size()];
        vertices.push_back(vertex(center.x, center.y, color));
        vertices.push_back(vertex(a.x, a.y, color));
        vertices.push_back(vertex(b.x, b.y, color));
    }
    band(vertices, solid, color, fade, withAlpha(color, 0));
    draw(renderer, vertices);
}

void outlineRounded(SDL_Renderer *renderer, SDL_Rect rect, int radius, int thickness, SDL_Color color) {
    if (rect.w <= 0 || rect.h <= 0 || thickness <= 0) return;
    // All outlines with the same number of points, so they pair up: the
    // ring solid between half a rim inside both of its edges, fading out
    // across them
    const int steps = cornerSteps(rect, static_cast<float>(radius));
    const float r = static_cast<float>(radius), t = static_cast<float>(thickness);
    const std::vector<SDL_FPoint> outerFade = roundedOutline(rect, r, -rim / 2, steps);
    const std::vector<SDL_FPoint> outer = roundedOutline(rect, r, rim / 2, steps);
    const std::vector<SDL_FPoint> inner = roundedOutline(rect, r, std::max(rim / 2, t - rim / 2), steps);
    const std::vector<SDL_FPoint> innerFade = roundedOutline(rect, r, t + rim / 2, steps);
    const SDL_Color clear = withAlpha(color, 0);
    std::vector<SDL_Vertex> vertices;
    band(vertices, outerFade, clear, outer, color);
    band(vertices, outer, color, inner, color);
    band(vertices, inner, color, innerFade, clear);
    draw(renderer, vertices);
}

void fillCircle(SDL_Renderer *renderer, int cx, int cy, int radius, SDL_Color color) {
    fillRounded(renderer, {cx - radius, cy - radius, radius * 2, radius * 2}, radius, color);
}

void fillStar(SDL_Renderer *renderer, int cx, int cy, int radius, SDL_Color color) {
    std::vector<SDL_FPoint> points;
    const float inner = radius * 0.45f;
    for (int i = 0; i < 10; ++i) {
        const float a = static_cast<float>(M_PI) * (-0.5f + i / 5.0f), r = i % 2 ? inner : radius;
        points.push_back({cx + r * std::cos(a), cy + r * std::sin(a)});
    }
    fillPolygon(renderer, points, {static_cast<float>(cx), static_cast<float>(cy)}, color);
}

void fillArrow(SDL_Renderer *renderer, int cx, int cy, int size, int dir, SDL_Color color) {
    const float half = size / 2.0f, tip = dir < 0 ? -half : half;
    const std::vector<SDL_FPoint> points{{cx + tip, static_cast<float>(cy)}, {cx - tip, cy - half}, {cx - tip, cy + half}};
    fillPolygon(renderer, points, {cx - tip / 3, static_cast<float>(cy)}, color);
}

void fillGradient(SDL_Renderer *renderer, SDL_Rect rect, SDL_Color top, SDL_Color bottom) {
    const float l = static_cast<float>(rect.x), r = static_cast<float>(rect.x + rect.w);
    const float t = static_cast<float>(rect.y), b = static_cast<float>(rect.y + rect.h);
    const SDL_Vertex vertices[6] = {vertex(l, t, top), vertex(r, t, top), vertex(l, b, bottom),
                                    vertex(r, t, top), vertex(r, b, bottom), vertex(l, b, bottom)};
    SDL_RenderGeometry(renderer, nullptr, vertices, 6, nullptr, 0);
}

namespace {

// The part of the image that differs from its top left pixel
SDL_Rect contentOf(const unsigned char *pixels, int w, int h) {
    const unsigned char *corner = pixels;
    auto differs = [&](int x, int y) {
        const unsigned char *p = pixels + (static_cast<std::size_t>(y) * w + x) * 4;
        return std::abs(p[0] - corner[0]) + std::abs(p[1] - corner[1]) + std::abs(p[2] - corner[2]) > 24;
    };
    int left = w, right = -1, top = h, bottom = -1;
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            if (!differs(x, y)) continue;
            left = std::min(left, x);
            right = std::max(right, x);
            top = std::min(top, y);
            bottom = std::max(bottom, y);
        }
    }
    if (right < 0) return {0, 0, w, h};
    // A little of the frame stays, so nothing sits right at the edge
    const int margin = std::max(2, w / 40);
    left = std::max(0, left - margin);
    top = std::max(0, top - margin);
    right = std::min(w - 1, right + margin);
    bottom = std::min(h - 1, bottom + margin);
    return {left, top, right - left + 1, bottom - top + 1};
}

} // namespace

SDL_Texture *loadImage(SDL_Renderer *renderer, const std::filesystem::path &path, int &w, int &h, bool trim) {
    int channels = 0;
    unsigned char *pixels = stbi_load(path.c_str(), &w, &h, &channels, 4);
    if (!pixels) return nullptr;
    SDL_Rect area{0, 0, w, h};
    if (trim) {
        // Only a frame around the picture, not into it (a mostly plain title)
        const SDL_Rect content = contentOf(pixels, w, h);
        if (content.w * 2 >= w && content.h * 2 >= h) area = content;
    }
    SDL_Texture *texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC, area.w, area.h);
    if (texture) {
        SDL_UpdateTexture(texture, nullptr, pixels + (static_cast<std::size_t>(area.y) * w + area.x) * 4, w * 4);
        SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND);
    }
    stbi_image_free(pixels);
    w = area.w;
    h = area.h;
    return texture;
}

SDL_Rect fitInto(SDL_Rect bounds, int w, int h) {
    if (w <= 0 || h <= 0) return bounds;
    const double scale = std::min(static_cast<double>(bounds.w) / w, static_cast<double>(bounds.h) / h);
    const int fw = static_cast<int>(w * scale), fh = static_cast<int>(h * scale);
    return {bounds.x + (bounds.w - fw) / 2, bounds.y + (bounds.h - fh) / 2, fw, fh};
}

} // namespace rd
