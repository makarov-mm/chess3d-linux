#include "Text.h"

#include <ft2build.h>
#include FT_FREETYPE_H
#include <fontconfig/fontconfig.h>

#include <algorithm>
#include <cmath>
#include <compare>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>

namespace text {

using namespace gl;

namespace {

// The typefaces asked of fontconfig, the first installed one taken: the ones of the Windows version, those of
// Linux desktops nearest to them, and then whatever the system calls sans-serif or serif.
constexpr const char* InterfaceFamilies[] = {"Segoe UI", "Selawik", "Noto Sans", "Open Sans", "Ubuntu", "Cantarell", "DejaVu Sans", "sans-serif"};
constexpr const char* LabelFamilies[] = {"Georgia", "Gelasio", "Noto Serif", "DejaVu Serif", "Liberation Serif", "serif"};

template <auto Destroy>
struct Deleter {
    template <typename T>
    void operator()(T* object) const
    {
        Destroy(object);
    }
};

using Pattern = std::unique_ptr<FcPattern, Deleter<&FcPatternDestroy>>;
using CharSet = std::unique_ptr<FcCharSet, Deleter<&FcCharSetDestroy>>;
using Face = std::unique_ptr<FT_FaceRec, Deleter<&FT_Done_Face>>;

FT_Library Library()
{
    struct Instance {
        FT_Library library = nullptr;
        Instance()
        {
            if (FT_Init_FreeType(&library) != 0)
                throw std::runtime_error("FreeType could not be started.");
        }
        ~Instance() { FT_Done_FreeType(library); }
    };
    static const Instance instance;
    return instance.library;
}

struct FontFile {
    std::string path;
    int index = 0; // of the face in the file, and of a named instance of a variable font

    auto operator<=>(const FontFile&) const = default;
};

// The installed font that best matches the typefaces and the weight, among those with the character if one is given.
std::optional<FontFile> Locate(std::span<const char* const> families, int weight, char32_t character = 0)
{
    const Pattern pattern(FcPatternCreate());
    for (const char* family : families)
        FcPatternAddString(pattern.get(), FC_FAMILY, reinterpret_cast<const FcChar8*>(family));
    FcPatternAddInteger(pattern.get(), FC_WEIGHT, weight);
    FcPatternAddInteger(pattern.get(), FC_SLANT, FC_SLANT_ROMAN);
    FcPatternAddBool(pattern.get(), FC_OUTLINE, FcTrue);
    FcPatternAddBool(pattern.get(), FC_COLOR, FcFalse);
    if (character != 0) {
        const CharSet characters(FcCharSetCreate());
        FcCharSetAddChar(characters.get(), character);
        FcPatternAddCharSet(pattern.get(), FC_CHARSET, characters.get());
    }
    FcConfigSubstitute(nullptr, pattern.get(), FcMatchPattern);
    FcDefaultSubstitute(pattern.get());

    FcResult result = FcResultNoMatch;
    const Pattern match(FcFontMatch(nullptr, pattern.get(), &result));
    FcChar8* path = nullptr;
    if (!match || FcPatternGetString(match.get(), FC_FILE, 0, &path) != FcResultMatch)
        return std::nullopt;
    int index = 0;
    FcPatternGetInteger(match.get(), FC_INDEX, 0, &index);
    return FontFile{reinterpret_cast<const char*>(path), index};
}

// The faces of a typeface and of the fonts that fill in the characters it lacks, each opened once, at one pixel size.
class FaceSet {
public:
    FaceSet(std::span<const char* const> families, int weight, int pixelHeight) : m_families(families), m_weight(weight), m_pixelHeight(pixelHeight)
    {
        if (const auto file = Locate(families, weight))
            Open(*file);
        if (m_order.empty())
            throw std::runtime_error("No usable font is installed (such as the fonts-dejavu-core package).");
    }

    FT_Face Main() const { return m_order.front(); }

    // The face that has the character and the glyph's index in it; no face when no installed font has it.
    std::pair<FT_Face, FT_UInt> Find(char32_t character)
    {
        for (FT_Face face : m_order)
            if (const FT_UInt index = FT_Get_Char_Index(face, character))
                return {face, index};
        if (const auto file = Locate(m_families, m_weight, character))
            if (FT_Face face = Open(*file))
                if (const FT_UInt index = FT_Get_Char_Index(face, character))
                    return {face, index};
        return {nullptr, 0};
    }

private:
    FT_Face Open(const FontFile& file)
    {
        if (const auto found = m_faces.find(file); found != m_faces.end())
            return found->second.get();
        FT_Face face = nullptr;
        if (FT_New_Face(Library(), file.path.c_str(), file.index, &face) != 0)
            return nullptr;
        Face owned(face);
        if (FT_Set_Pixel_Sizes(face, 0, static_cast<FT_UInt>(m_pixelHeight)) != 0)
            return nullptr;
        m_faces.emplace(file, std::move(owned));
        m_order.push_back(face);
        return face;
    }

    std::span<const char* const> m_families;
    int m_weight;
    int m_pixelHeight;
    std::map<FontFile, Face> m_faces;
    std::vector<FT_Face> m_order; // the typeface's own first
};

struct Bitmap {
    int width = 0, height = 0;
    int left = 0, top = 0; // from the pen, and up from the baseline
    int advance = 0;
    std::vector<std::uint8_t> coverage;
};

// A glyph drawn with anti-aliasing and light hinting, which keeps the shapes and sets them on the pixel rows.
std::optional<Bitmap> Render(FT_Face face, FT_UInt index)
{
    if (FT_Load_Glyph(face, index, FT_LOAD_RENDER | FT_LOAD_TARGET_LIGHT | FT_LOAD_NO_BITMAP) != 0)
        return std::nullopt;
    const FT_GlyphSlot slot = face->glyph;
    const FT_Bitmap& source = slot->bitmap;
    if (source.pixel_mode != FT_PIXEL_MODE_GRAY && source.width > 0)
        return std::nullopt;

    Bitmap bitmap{
        .width = static_cast<int>(source.width),
        .height = static_cast<int>(source.rows),
        .left = slot->bitmap_left,
        .top = slot->bitmap_top,
        .advance = static_cast<int>(std::lround(static_cast<double>(slot->advance.x) / 64)),
        .coverage = std::vector<std::uint8_t>(static_cast<std::size_t>(source.width) * source.rows),
    };
    for (int y = 0; y < bitmap.height; ++y) {
        // A negative pitch counts the rows from the bottom.
        const int row = source.pitch >= 0 ? y : bitmap.height - 1 - y;
        const unsigned char* from = source.buffer + static_cast<std::ptrdiff_t>(row) * std::abs(source.pitch);
        std::copy_n(from, bitmap.width, bitmap.coverage.begin() + static_cast<std::ptrdiff_t>(y) * bitmap.width);
    }
    return bitmap;
}

std::vector<char32_t> Codepoints()
{
    std::vector<char32_t> codepoints;
    for (char32_t c = 0x20; c < 0x7F; ++c)
        codepoints.push_back(c);
    for (char32_t c = 0x410; c <= 0x44F; ++c)
        codepoints.push_back(c);
    for (char32_t c = 0x2654; c <= 0x265F; ++c)
        codepoints.push_back(c); // the chess figurines
    for (const char32_t c : {0x401,  0x451,  0x2014, 0x2013, 0x2026, 0x2022, 0x00B7, 0x2190, 0x2192, 0x2191, 0x2193, 0x00A0, 0x00A9,
                             0x00D7, 0x00BD, 0x00AB, 0x00BB, 0x2116, 0x2197, 0x21B6, 0x21B7, 0x25B2, 0x25BC, 0x25C0, 0x25B6})
        codepoints.push_back(c);
    return codepoints;
}

} // namespace

char32_t NextCodepoint(std::string_view text, std::size_t& index)
{
    const auto byte = [&](std::size_t i) { return static_cast<unsigned char>(text[i]); };
    const unsigned char first = byte(index++);
    if (first < 0x80)
        return first;
    int extra = first >= 0xF0 ? 3 : first >= 0xE0 ? 2 : first >= 0xC0 ? 1 : 0;
    char32_t codepoint = first & (0x3F >> extra);
    while (extra-- > 0 && index < text.size())
        codepoint = (codepoint << 6) | (byte(index++) & 0x3F);
    return codepoint;
}

Font::Font(float pixelHeight, Weight weight)
{
    const int height = std::max(6, static_cast<int>(std::lround(pixelHeight)));
    FaceSet faces(InterfaceFamilies, weight == Weight::SemiBold ? FC_WEIGHT_SEMIBOLD : FC_WEIGHT_REGULAR, height);
    const FT_Size_Metrics& metrics = faces.Main()->size->metrics;
    const int ascent = static_cast<int>(std::ceil(static_cast<double>(metrics.ascender) / 64));
    const int descent = static_cast<int>(std::ceil(static_cast<double>(-metrics.descender) / 64));
    m_ascent = static_cast<float>(ascent);
    m_lineHeight = static_cast<float>(ascent + descent);

    struct Item {
        char32_t codepoint;
        Bitmap bitmap;
        int x = 0, y = 0;
    };
    std::vector<Item> items;
    for (const char32_t codepoint : Codepoints()) {
        const auto [face, index] = faces.Find(codepoint);
        if (!face)
            continue;
        if (auto bitmap = Render(face, index))
            items.push_back({codepoint, std::move(*bitmap)});
    }

    // Shelves of glyphs, each with a pixel of room round it, so that filtering does not reach the next one.
    constexpr int Pad = 1;
    const auto isBlank = [](const Item& item) { return item.bitmap.width == 0 || item.bitmap.height == 0; };
    int atlasWidth = 128, atlasHeight = 0;
    for (;; atlasWidth *= 2) {
        int x = 0, y = 0, shelf = 0;
        for (Item& item : items) {
            if (isBlank(item))
                continue;
            const int cellWidth = item.bitmap.width + 2 * Pad, cellHeight = item.bitmap.height + 2 * Pad;
            if (x + cellWidth > atlasWidth) {
                x = 0;
                y += shelf;
                shelf = 0;
            }
            item.x = x;
            item.y = y;
            x += cellWidth;
            shelf = std::max(shelf, cellHeight);
        }
        atlasHeight = std::max(1, y + shelf);
        if (atlasHeight <= atlasWidth)
            break;
    }

    std::vector<std::uint8_t> atlas(static_cast<std::size_t>(atlasWidth) * static_cast<std::size_t>(atlasHeight));
    for (const Item& item : items) {
        for (int y = 0; y < item.bitmap.height; ++y) {
            const auto from = item.bitmap.coverage.begin() + static_cast<std::ptrdiff_t>(y) * item.bitmap.width;
            std::copy_n(from, item.bitmap.width, atlas.begin() + static_cast<std::ptrdiff_t>(item.y + Pad + y) * atlasWidth + item.x + Pad);
        }
    }

    m_texture = CreateTexture(GL_TEXTURE_2D);
    glTextureStorage2D(m_texture.Id(), 1, GL_R8, atlasWidth, atlasHeight);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTextureSubImage2D(m_texture.Id(), 0, 0, 0, atlasWidth, atlasHeight, GL_RED, GL_UNSIGNED_BYTE, atlas.data());
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTextureParameteri(m_texture.Id(), GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTextureParameteri(m_texture.Id(), GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTextureParameteri(m_texture.Id(), GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTextureParameteri(m_texture.Id(), GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    const float w = static_cast<float>(atlasWidth), h = static_cast<float>(atlasHeight);
    for (const Item& item : items) {
        Glyph glyph;
        glyph.advance = static_cast<float>(item.bitmap.advance);
        if (!isBlank(item)) {
            const int cellWidth = item.bitmap.width + 2 * Pad, cellHeight = item.bitmap.height + 2 * Pad;
            glyph.u0 = static_cast<float>(item.x) / w;
            glyph.v0 = static_cast<float>(item.y) / h;
            glyph.u1 = static_cast<float>(item.x + cellWidth) / w;
            glyph.v1 = static_cast<float>(item.y + cellHeight) / h;
            glyph.width = static_cast<float>(cellWidth);
            glyph.height = static_cast<float>(cellHeight);
            glyph.left = static_cast<float>(item.bitmap.left - Pad);
            glyph.top = static_cast<float>(ascent - item.bitmap.top - Pad);
        }
        m_glyphs.emplace(item.codepoint, glyph);
    }
    if (const auto question = m_glyphs.find(U'?'); question != m_glyphs.end())
        m_fallback = question->second;
}

const Glyph& Font::Find(char32_t codepoint) const
{
    const auto found = m_glyphs.find(codepoint);
    return found != m_glyphs.end() ? found->second : m_fallback;
}

float Font::Measure(std::string_view utf8) const
{
    float width = 0;
    for (std::size_t i = 0; i < utf8.size();)
        width += Find(NextCodepoint(utf8, i)).advance;
    return width;
}

std::vector<std::uint8_t> RenderBoardLabels(int size, float extent, float inner)
{
    std::vector<std::uint8_t> coverage(static_cast<std::size_t>(size) * static_cast<std::size_t>(size));
    const float pixelsPerUnit = static_cast<float>(size) / (2 * extent);
    const int height = static_cast<int>(0.3f * pixelsPerUnit);
    const float middle = (inner + extent) / 2 - 0.02f;
    auto toPixel = [&](float world) { return static_cast<int>(std::lround((world + extent) * pixelsPerUnit)); };

    FaceSet faces(LabelFamilies, FC_WEIGHT_SEMIBOLD, height);
    // Half the height of the capitals, in world units, by which the characters are centered on their points.
    float capHeight = 0.7f * static_cast<float>(height);
    if (FT_Load_Char(faces.Main(), 'H', FT_LOAD_TARGET_LIGHT | FT_LOAD_NO_BITMAP) == 0)
        capHeight = static_cast<float>(faces.Main()->glyph->metrics.horiBearingY) / 64;
    const float halfCap = capHeight * 0.5f / pixelsPerUnit;

    auto draw = [&](char32_t character, float x, float z, bool forWhite) {
        const auto [face, index] = faces.Find(character);
        const auto bitmap = face ? Render(face, index) : std::nullopt;
        if (!bitmap)
            return;
        // Centered on the point: the baseline below it for White, above it for Black, who sees it turned half round
        // about the middle of its baseline.
        const int originX = toPixel(x), baseline = toPixel(z + (forWhite ? halfCap : -halfCap));
        const int left = originX - bitmap->advance / 2 + bitmap->left, top = baseline - bitmap->top;
        for (int row = 0; row < bitmap->height; ++row) {
            for (int column = 0; column < bitmap->width; ++column) {
                int px = left + column, py = top + row;
                if (!forWhite) {
                    px = 2 * originX - 1 - px;
                    py = 2 * baseline - 1 - py;
                }
                if (px < 0 || py < 0 || px >= size || py >= size)
                    continue;
                std::uint8_t& target = coverage[static_cast<std::size_t>(py) * static_cast<std::size_t>(size) + static_cast<std::size_t>(px)];
                target = std::max(target, bitmap->coverage[static_cast<std::size_t>(row * bitmap->width + column)]);
            }
        }
    };

    // The files from White's left, the ranks from White's side, read the right way up from White's side and turned
    // for Black's.
    for (int i = 0; i < 8; ++i) {
        const float offset = static_cast<float>(i) - 3.5f;
        draw(static_cast<char32_t>(U'a' + i), offset, middle, true);
        draw(static_cast<char32_t>(U'a' + i), offset, -middle, false);
        draw(static_cast<char32_t>(U'1' + i), -middle, -offset, true);
        draw(static_cast<char32_t>(U'1' + i), middle, -offset, false);
    }
    return coverage;
}

} // namespace text
