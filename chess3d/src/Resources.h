#pragma once

#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

// The files built into the executable by cmake/Embed.cmake: the shaders and the icon.
namespace resources {

struct File {
    std::string_view name;
    std::span<const unsigned char> data;
};

// All of them, in the generated Embedded.cpp.
std::span<const File> Files();

// The bytes of a file, by its name ("piece.frag"); throws when there is no such file.
std::span<const unsigned char> Data(std::string_view name);
std::string_view Text(std::string_view name);

// A picture: 8-bit RGBA, not premultiplied, the rows from the top.
struct Image {
    int width = 0, height = 0;
    std::vector<std::uint8_t> rgba;

    bool Empty() const { return rgba.empty(); }
};

// A PNG file built in, decoded; empty when it cannot be read.
Image Png(std::string_view name);

} // namespace resources
