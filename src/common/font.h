// Text for SDL_Renderer from TrueType fonts (stb_truetype). Glyphs are
// rasterized on first use per pixel size and kept in texture atlases, so a
// frame only copies rectangles. Strings are UTF-8.
#pragma once

#include <SDL.h>

#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace rd {

enum class Align { left, center, right };

class Fonts {
public:
    explicit Fonts(SDL_Renderer *renderer);
    ~Fonts();
    Fonts(const Fonts &) = delete;
    Fonts &operator=(const Fonts &) = delete;

    // Returns the font's id, or -1 if the file cannot be read
    int load(const std::filesystem::path &path);

    // Draws a single line; y is the top of the line. Returns its width.
    int draw(int font, int px, int x, int y, std::string_view text, SDL_Color color,
             Align align = Align::left);
    int width(int font, int px, std::string_view text);
    int lineHeight(int font, int px);
    // Shortens text with "…" so that it fits into maxWidth
    std::string fit(int font, int px, std::string_view text, int maxWidth);
    // Breaks text into lines of at most maxWidth (at spaces)
    std::vector<std::string> wrap(int font, int px, std::string_view text, int maxWidth);

    // Drops all rasterized glyphs (after the renderer lost its textures)
    void flush();

private:
    struct Glyph {
        int page = -1;
        SDL_Rect source{};
        int offsetX = 0, offsetY = 0;
        float advance = 0;
    };
    struct Face;
    struct Size {
        float scale = 0;
        int ascent = 0, lineHeight = 0;
        std::unordered_map<char32_t, Glyph> glyphs;
    };

    Size &size(int font, int px);
    const Glyph &glyph(int font, int px, Size &size, char32_t codepoint);
    bool place(int w, int h, int &page, SDL_Rect &rect);

    SDL_Renderer *renderer_;
    std::vector<std::unique_ptr<Face>> faces_;
    std::unordered_map<long long, Size> sizes_;
    std::vector<SDL_Texture *> pages_;
    int shelfX_ = 0, shelfY_ = 0, shelfHeight_ = 0;
};

// Decodes one UTF-8 sequence at text[i] and advances i
char32_t nextCodepoint(std::string_view text, std::size_t &i);

} // namespace rd
