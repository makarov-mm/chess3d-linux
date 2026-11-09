#pragma once

struct SDL_Window;

namespace resources {
struct Image;
}

// What comes from the desktop rather than from SDL. Kept apart because the X11 headers bring macros such as None.
namespace desktop {

// The desktop's scaling of text and interfaces, from Xft.dpi on X11 as Cinnamon, GNOME, KDE and Xfce set it (1 at
// 96 dpi). Wayland scales the window's pixels instead, which SDL reports as a drawable larger than the window.
float TextScale(SDL_Window* window);

// The icon in the window's title bar and the task bar.
void SetIcon(SDL_Window* window, const resources::Image& icon);

} // namespace desktop
