#include "Resources.h"

#include <png.h>

#include <algorithm>
#include <stdexcept>
#include <string>

namespace resources {

std::span<const unsigned char> Data(std::string_view name)
{
    const auto files = Files();
    const auto found = std::ranges::find(files, name, &File::name);
    if (found == files.end())
        throw std::runtime_error("A file is missing from the program: " + std::string(name));
    return found->data;
}

std::string_view Text(std::string_view name)
{
    const auto data = Data(name);
    return {reinterpret_cast<const char*>(data.data()), data.size()};
}

Image Png(std::string_view name)
{
    const auto file = Data(name);
    png_image png{};
    png.version = PNG_IMAGE_VERSION;
    if (!png_image_begin_read_from_memory(&png, file.data(), file.size()))
        return {};
    png.format = PNG_FORMAT_RGBA;
    Image image{.width = static_cast<int>(png.width), .height = static_cast<int>(png.height), .rgba = std::vector<std::uint8_t>(PNG_IMAGE_SIZE(png))};
    if (!png_image_finish_read(&png, nullptr, image.rgba.data(), 0, nullptr)) {
        png_image_free(&png);
        return {};
    }
    return image;
}

} // namespace resources
