#include "font.h"

#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"

#include <cmath>
#include <fstream>
#include <iterator>

namespace rd {

namespace {
constexpr int pageSize = 1024;
constexpr char32_t replacement = U'?';
}

struct Fonts::Face {
    std::vector<unsigned char> data;
    stbtt_fontinfo info{};
};

char32_t nextCodepoint(std::string_view text, std::size_t &i) {
    const auto byte = [&](std::size_t at) { return static_cast<unsigned char>(text[at]); };
    const unsigned char first = byte(i++);
    if (first < 0x80) return first;
    int extra = first >= 0xf0 ? 3 : first >= 0xe0 ? 2 : first >= 0xc0 ? 1 : 0;
    if (extra == 0) return replacement;
    char32_t codepoint = first & (0x3f >> extra);
    for (; extra > 0; --extra) {
        if (i >= text.size() || (byte(i) & 0xc0) != 0x80) return replacement;
        codepoint = (codepoint << 6) | (byte(i++) & 0x3f);
    }
    return codepoint;
}

Fonts::Fonts(SDL_Renderer *renderer) : renderer_(renderer) {}

Fonts::~Fonts() {
    for (SDL_Texture *page : pages_) SDL_DestroyTexture(page);
}

int Fonts::load(const std::filesystem::path &path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return -1;
    auto face = std::make_unique<Face>();
    face->data.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    if (!stbtt_InitFont(&face->info, face->data.data(), stbtt_GetFontOffsetForIndex(face->data.data(), 0)))
        return -1;
    faces_.push_back(std::move(face));
    return static_cast<int>(faces_.size()) - 1;
}

void Fonts::flush() {
    for (SDL_Texture *page : pages_) SDL_DestroyTexture(page);
    pages_.clear();
    sizes_.clear();
    shelfX_ = shelfY_ = shelfHeight_ = 0;
}

Fonts::Size &Fonts::size(int font, int px) {
    const long long key = (static_cast<long long>(font) << 32) | static_cast<unsigned>(px);
    auto found = sizes_.find(key);
    if (found != sizes_.end()) return found->second;
    Size &size = sizes_[key];
    const stbtt_fontinfo &info = faces_[static_cast<std::size_t>(font)]->info;
    // px is the em size, as font sizes usually are
    size.scale = stbtt_ScaleForMappingEmToPixels(&info, static_cast<float>(px));
    int ascent = 0, descent = 0, gap = 0;
    stbtt_GetFontVMetrics(&info, &ascent, &descent, &gap);
    size.ascent = static_cast<int>(std::lround(ascent * size.scale));
    size.lineHeight = static_cast<int>(std::lround((ascent - descent + gap) * size.scale));
    return size;
}

bool Fonts::place(int w, int h, int &page, SDL_Rect &rect) {
    if (w > pageSize || h > pageSize) return false;
    if (pages_.empty() || shelfX_ + w > pageSize) {
        shelfX_ = 0;
        shelfY_ += shelfHeight_ + 1;
        shelfHeight_ = 0;
    }
    if (pages_.empty() || shelfY_ + h > pageSize) {
        SDL_Texture *texture = SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC,
                                                 pageSize, pageSize);
        if (!texture) return false;
        SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND);
        std::vector<Uint32> clear(static_cast<std::size_t>(pageSize) * pageSize, 0x00ffffff);
        SDL_UpdateTexture(texture, nullptr, clear.data(), pageSize * 4);
        pages_.push_back(texture);
        shelfX_ = shelfY_ = shelfHeight_ = 0;
    }
    page = static_cast<int>(pages_.size()) - 1;
    rect = {shelfX_, shelfY_, w, h};
    shelfX_ += w + 1;
    shelfHeight_ = std::max(shelfHeight_, h);
    return true;
}

const Fonts::Glyph &Fonts::glyph(int font, int px, Size &size, char32_t codepoint) {
    (void)px;
    auto found = size.glyphs.find(codepoint);
    if (found != size.glyphs.end()) return found->second;
    const stbtt_fontinfo &info = faces_[static_cast<std::size_t>(font)]->info;
    Glyph &glyph = size.glyphs[codepoint];
    int index = stbtt_FindGlyphIndex(&info, static_cast<int>(codepoint));
    if (index == 0 && codepoint != replacement) index = stbtt_FindGlyphIndex(&info, static_cast<int>(replacement));
    int advance = 0, bearing = 0;
    stbtt_GetGlyphHMetrics(&info, index, &advance, &bearing);
    glyph.advance = advance * size.scale;
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    stbtt_GetGlyphBitmapBox(&info, index, size.scale, size.scale, &x0, &y0, &x1, &y1);
    const int w = x1 - x0, h = y1 - y0;
    if (w <= 0 || h <= 0) return glyph;
    std::vector<unsigned char> alpha(static_cast<std::size_t>(w) * h);
    stbtt_MakeGlyphBitmap(&info, alpha.data(), w, h, w, size.scale, size.scale, index);
    std::vector<Uint32> pixels(alpha.size());
    for (std::size_t i = 0; i < alpha.size(); ++i) pixels[i] = (static_cast<Uint32>(alpha[i]) << 24) | 0x00ffffff;
    if (!place(w, h, glyph.page, glyph.source)) return glyph;
    SDL_UpdateTexture(pages_[static_cast<std::size_t>(glyph.page)], &glyph.source, pixels.data(), w * 4);
    glyph.offsetX = x0;
    glyph.offsetY = y0;
    return glyph;
}

int Fonts::width(int font, int px, std::string_view text) {
    if (font < 0) return 0;
    Size &metrics = size(font, px);
    const stbtt_fontinfo &info = faces_[static_cast<std::size_t>(font)]->info;
    float x = 0;
    char32_t previous = 0;
    for (std::size_t i = 0; i < text.size();) {
        const char32_t codepoint = nextCodepoint(text, i);
        if (previous) x += stbtt_GetCodepointKernAdvance(&info, static_cast<int>(previous), static_cast<int>(codepoint)) * metrics.scale;
        x += glyph(font, px, metrics, codepoint).advance;
        previous = codepoint;
    }
    return static_cast<int>(std::ceil(x));
}

int Fonts::lineHeight(int font, int px) {
    return font < 0 ? px : size(font, px).lineHeight;
}

int Fonts::draw(int font, int px, int x, int y, std::string_view text, SDL_Color color, Align align) {
    if (font < 0 || text.empty()) return 0;
    const int total = width(font, px, text);
    if (align == Align::center) x -= total / 2;
    if (align == Align::right) x -= total;
    Size &metrics = size(font, px);
    const stbtt_fontinfo &info = faces_[static_cast<std::size_t>(font)]->info;
    float pen = static_cast<float>(x);
    char32_t previous = 0;
    for (std::size_t i = 0; i < text.size();) {
        const char32_t codepoint = nextCodepoint(text, i);
        if (previous) pen += stbtt_GetCodepointKernAdvance(&info, static_cast<int>(previous), static_cast<int>(codepoint)) * metrics.scale;
        const Glyph &g = glyph(font, px, metrics, codepoint);
        if (g.page >= 0) {
            SDL_Texture *page = pages_[static_cast<std::size_t>(g.page)];
            SDL_SetTextureColorMod(page, color.r, color.g, color.b);
            SDL_SetTextureAlphaMod(page, color.a);
            const SDL_Rect target{static_cast<int>(std::lround(pen)) + g.offsetX, y + metrics.ascent + g.offsetY,
                                  g.source.w, g.source.h};
            SDL_RenderCopy(renderer_, page, &g.source, &target);
        }
        pen += g.advance;
        previous = codepoint;
    }
    return total;
}

std::string Fonts::fit(int font, int px, std::string_view text, int maxWidth) {
    if (width(font, px, text) <= maxWidth) return std::string(text);
    const std::string_view ellipsis = "…";
    const int room = maxWidth - width(font, px, ellipsis);
    std::size_t end = 0;
    for (std::size_t i = 0; i < text.size();) {
        std::size_t next = i;
        nextCodepoint(text, next);
        if (width(font, px, text.substr(0, next)) > room) break;
        end = i = next;
    }
    return std::string(text.substr(0, end)) + std::string(ellipsis);
}

std::vector<std::string> Fonts::wrap(int font, int px, std::string_view text, int maxWidth) {
    std::vector<std::string> lines;
    std::string line;
    std::size_t start = 0;
    while (start <= text.size()) {
        std::size_t end = text.find(' ', start);
        if (end == std::string_view::npos) end = text.size();
        const std::string_view word = text.substr(start, end - start);
        const std::string candidate = line.empty() ? std::string(word) : line + " " + std::string(word);
        if (!line.empty() && width(font, px, candidate) > maxWidth) {
            lines.push_back(line);
            line = std::string(word);
        } else {
            line = candidate;
        }
        start = end + 1;
    }
    if (!line.empty()) lines.push_back(line);
    return lines;
}

} // namespace rd
