#include "UiPainter.h"

#include "Resources.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <utility>

namespace ui {

using namespace gl;
using math::Vec4;

Color Fade(Color color, float opacity)
{
    color.a = static_cast<std::uint8_t>(std::clamp(static_cast<float>(color.a) * opacity, 0.0f, 255.0f));
    return color;
}

Color Mix(Color a, Color b, float t)
{
    auto mix = [t](std::uint8_t x, std::uint8_t y) { return static_cast<std::uint8_t>(std::lround(math::Lerp(x, y, std::clamp(t, 0.0f, 1.0f)))); };
    return {mix(a.r, b.r), mix(a.g, b.g), mix(a.b, b.b), mix(a.a, b.a)};
}

Color FromLinear(math::Vec3 color, float alpha)
{
    auto encode = [](float c) { return static_cast<std::uint8_t>(std::lround(std::pow(std::clamp(c, 0.0f, 1.0f), 1 / 2.2f) * 255)); };
    return {.r = encode(color.x), .g = encode(color.y), .b = encode(color.z), .a = static_cast<std::uint8_t>(std::lround(std::clamp(alpha, 0.0f, 1.0f) * 255))};
}

Painter::Painter()
{
    using Stage = render::Program::Stage;
    m_program = render::Program("interface", {Stage{GL_VERTEX_SHADER, {"ui.vert"}}, Stage{GL_FRAGMENT_SHADER, {"common.glsl", "ui.frag"}}});
    m_buffer = CreateBuffer();
    m_vertexArray = CreateVertexArray();
    const GLuint array = m_vertexArray.Id();
    glVertexArrayVertexBuffer(array, 0, m_buffer.Id(), 0, sizeof(Vertex));

    constexpr GLint sizes[4] = {2, 2, 4, 4};
    constexpr GLenum types[4] = {GL_FLOAT, GL_FLOAT, GL_UNSIGNED_BYTE, GL_FLOAT};
    constexpr GLuint offsets[4] = {offsetof(Vertex, x), offsetof(Vertex, u), offsetof(Vertex, color), offsetof(Vertex, kind)};
    for (GLuint attribute = 0; attribute < 4; ++attribute) {
        glEnableVertexArrayAttrib(array, attribute);
        glVertexArrayAttribFormat(array, attribute, sizes[attribute], types[attribute], types[attribute] == GL_UNSIGNED_BYTE, offsets[attribute]);
        glVertexArrayAttribBinding(array, attribute, 0);
    }
}

void Painter::SetIcon(const resources::Image& icon)
{
    if (icon.Empty())
        return;
    // Premultiplied by alpha, so that the smaller mip levels do not darken at the edges with the color of the
    // transparent pixels; the shader divides it out again.
    std::vector<std::uint8_t> pixels = icon.rgba;
    for (std::size_t i = 0; i < pixels.size(); i += 4)
        for (std::size_t c = 0; c < 3; ++c)
            pixels[i + c] = static_cast<std::uint8_t>((pixels[i + c] * pixels[i + 3] + 127) / 255);
    int levels = 1;
    while ((std::max(icon.width, icon.height) >> levels) > 0)
        ++levels;
    m_icon = CreateTexture(GL_TEXTURE_2D);
    glTextureStorage2D(m_icon.Id(), levels, GL_RGBA8, icon.width, icon.height);
    glTextureSubImage2D(m_icon.Id(), 0, 0, 0, icon.width, icon.height, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
    glGenerateTextureMipmap(m_icon.Id());
    glTextureParameteri(m_icon.Id(), GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTextureParameteri(m_icon.Id(), GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTextureParameteri(m_icon.Id(), GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTextureParameteri(m_icon.Id(), GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
}

void Painter::Begin(int width, int height)
{
    m_width = width;
    m_height = height;
    m_vertices.clear();
}

void Painter::Quad(float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1, Color color, Vec4 shape)
{
    const Vertex a{.x = x0, .y = y0, .u = u0, .v = v0, .color = color, .kind = shape.x, .a = shape.y, .b = shape.z, .c = shape.w};
    const Vertex b{.x = x1, .y = y0, .u = u1, .v = v0, .color = color, .kind = shape.x, .a = shape.y, .b = shape.z, .c = shape.w};
    const Vertex c{.x = x1, .y = y1, .u = u1, .v = v1, .color = color, .kind = shape.x, .a = shape.y, .b = shape.z, .c = shape.w};
    const Vertex d{.x = x0, .y = y1, .u = u0, .v = v1, .color = color, .kind = shape.x, .a = shape.y, .b = shape.z, .c = shape.w};
    m_vertices.insert(m_vertices.end(), {a, b, c, a, c, d});
}

// A quad round a rectangle, its uv the position from the rectangle's center, for the distance shapes.
void Painter::Shape(Rect rect, float margin, Color color, Vec4 shape)
{
    const float halfWidth = rect.width / 2, halfHeight = rect.height / 2;
    Quad(rect.x - margin, rect.y - margin, rect.Right() + margin, rect.Bottom() + margin, -halfWidth - margin, -halfHeight - margin, halfWidth + margin,
         halfHeight + margin, color, shape);
}

void Painter::Fill(Rect rect, float radius, Color color)
{
    radius = std::min({radius, rect.width / 2, rect.height / 2});
    Shape(rect, 1, color, {.x = 3, .y = rect.width / 2, .z = rect.height / 2, .w = radius});
}

void Painter::Outline(Rect rect, float radius, Color color)
{
    radius = std::min({radius, rect.width / 2, rect.height / 2});
    Shape(rect, 1, color, {.x = 5, .y = rect.width / 2, .z = rect.height / 2, .w = radius});
}

void Painter::Shadow(Rect rect, float blur, Color color) { Shape(rect, blur * 1.5f, color, {.x = 4, .y = rect.width / 2, .z = rect.height / 2, .w = blur}); }

void Painter::Disc(float x, float y, float radius, Color color)
{
    Shape({.x = x - radius, .y = y - radius, .width = radius * 2, .height = radius * 2}, 1, color, {.x = 7, .y = radius});
}

void Painter::Spinner(float x, float y, float radius, float thickness, float angle, Color color)
{
    Shape({.x = x - radius, .y = y - radius, .width = radius * 2, .height = radius * 2}, thickness, color, {.x = 6, .y = radius, .z = thickness, .w = angle});
}

void Painter::Arrow(float x, float y, float halfWidth, float halfHeight, float direction, Color color)
{
    Shape({.x = x - halfWidth, .y = y - halfHeight, .width = halfWidth * 2, .height = halfHeight * 2}, 1, color,
          {.x = 8, .y = halfWidth, .z = halfHeight, .w = direction});
}

void Painter::Icon(Rect rect, float opacity)
{
    if (m_icon)
        Quad(rect.x, rect.y, rect.Right(), rect.Bottom(), 0, 0, 1, 1, Fade(Rgba(255, 255, 255), opacity), {.x = 9});
}

float Painter::Text(FontId id, float x, float y, std::string_view utf8, Color color)
{
    const text::Font& font = Font(id);
    const float kind = static_cast<float>(std::to_underlying(id));
    float pen = std::round(x);
    const float top = std::round(y);
    for (std::size_t i = 0; i < utf8.size();) {
        const text::Glyph& glyph = font.Find(text::NextCodepoint(utf8, i));
        const float left = std::round(pen + glyph.left), glyphTop = top + glyph.top;
        if (glyph.width > 0)
            Quad(left, glyphTop, left + glyph.width, glyphTop + glyph.height, glyph.u0, glyph.v0, glyph.u1, glyph.v1, color, {.x = kind});
        pen += glyph.advance;
    }
    return pen - std::round(x);
}

std::string Painter::Elided(FontId font, std::string_view utf8, float width) const
{
    if (Measure(font, utf8) <= width)
        return std::string(utf8);
    const text::Font& face = Font(font);
    const float room = width - face.Find(U'…').advance;
    float used = 0;
    std::size_t kept = 0; // up to the last character that fits and is not a space or a separator
    for (std::size_t i = 0; i < utf8.size();) {
        const char32_t codepoint = text::NextCodepoint(utf8, i);
        used += face.Find(codepoint).advance;
        if (used > room)
            break;
        if (codepoint != U' ' && codepoint != U'·')
            kept = i;
    }
    return std::string(utf8.substr(0, kept)) + "…";
}

void Painter::TextCentered(FontId font, Rect rect, std::string_view utf8, Color color)
{
    const float width = Measure(font, utf8);
    const float height = Font(font).Ascent();
    Text(font, rect.x + (rect.width - width) / 2, rect.y + (rect.height - height) / 2 - height * 0.04f, utf8, color);
}

void Painter::Flush()
{
    if (m_vertices.empty())
        return;
    glNamedBufferData(m_buffer.Id(), static_cast<GLsizeiptr>(m_vertices.size() * sizeof(Vertex)), m_vertices.data(), GL_STREAM_DRAW);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, m_width, m_height);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glEnable(GL_BLEND);
    glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    m_program.Set("uScreen", math::Vec2{static_cast<float>(m_width), static_cast<float>(m_height)});
    m_program.Use();
    for (GLuint i = 0; i < m_fonts.size(); ++i)
        glBindTextureUnit(8 + i, m_fonts[i] ? m_fonts[i]->Texture() : 0);
    glBindTextureUnit(11, m_icon.Id());
    glBindVertexArray(m_vertexArray.Id());
    glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(m_vertices.size()));
    glDisable(GL_BLEND);
    m_vertices.clear();
}

} // namespace ui
