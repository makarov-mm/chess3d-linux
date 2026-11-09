#include "Desktop.h"

#include "Resources.h"

#include <SDL.h>
#include <SDL_syswm.h>
#include <X11/Xlib.h>

#include <cstdlib>

namespace desktop {

float TextScale(SDL_Window* window)
{
    SDL_SysWMinfo info;
    SDL_VERSION(&info.version);
    if (!SDL_GetWindowWMInfo(window, &info) || info.subsystem != SDL_SYSWM_X11)
        return 1;
    const char* resource = XGetDefault(info.info.x11.display, "Xft", "dpi");
    if (!resource)
        return 1;
    const float dpi = std::strtof(resource, nullptr);
    return dpi >= 48 && dpi <= 480 ? dpi / 96 : 1;
}

void SetIcon(SDL_Window* window, const resources::Image& icon)
{
    if (icon.Empty())
        return;
    // SDL only reads the pixels.
    void* pixels = const_cast<std::uint8_t*>(icon.rgba.data());
    if (SDL_Surface* surface = SDL_CreateRGBSurfaceWithFormatFrom(pixels, icon.width, icon.height, 32, icon.width * 4, SDL_PIXELFORMAT_RGBA32)) {
        SDL_SetWindowIcon(window, surface);
        SDL_FreeSurface(surface);
    }
}

} // namespace desktop
