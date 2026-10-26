// Filled shapes for SDL_Renderer (rounded rectangles, circles, gradients)
// and images loaded with stb_image.
#pragma once

#include <SDL.h>

#include <filesystem>

namespace rd {

void fillRect(SDL_Renderer *renderer, SDL_Rect rect, SDL_Color color);
void fillRounded(SDL_Renderer *renderer, SDL_Rect rect, int radius, SDL_Color color);
void outlineRounded(SDL_Renderer *renderer, SDL_Rect rect, int radius, int thickness, SDL_Color color);
void fillCircle(SDL_Renderer *renderer, int cx, int cy, int radius, SDL_Color color);
// A five-pointed star and a triangle pointing left (dir < 0) or right
void fillStar(SDL_Renderer *renderer, int cx, int cy, int radius, SDL_Color color);
void fillArrow(SDL_Renderer *renderer, int cx, int cy, int size, int dir, SDL_Color color);
// Vertical gradient from top to bottom
void fillGradient(SDL_Renderer *renderer, SDL_Rect rect, SDL_Color top, SDL_Color bottom);

// Loads a PNG/JPEG into a texture; nullptr if it cannot be read. w/h get its size.
// With `trim`, a border in one colour (an emulator's black frame) is cut off.
SDL_Texture *loadImage(SDL_Renderer *renderer, const std::filesystem::path &path, int &w, int &h, bool trim = false);

// The largest rectangle with aspect w:h that fits into bounds, centered
SDL_Rect fitInto(SDL_Rect bounds, int w, int h);

constexpr SDL_Color withAlpha(SDL_Color color, Uint8 alpha) { return {color.r, color.g, color.b, alpha}; }

} // namespace rd
