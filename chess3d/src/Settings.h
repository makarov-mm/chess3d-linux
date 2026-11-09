#pragma once

#include "Strings.h"

#include <string>

// The choices and the game in progress, kept between sessions in ~/.config/chess3d/chess3d.ini.
struct Settings {
    int theme = 0;
    int level = 3;
    int mode = 0;
    bool sound = true;
    bool panel = true;
    Language language = Language::English;
    int windowWidth = 0, windowHeight = 0; // when it is neither maximized nor full screen; 0 before the first run
    bool maximized = false;
    std::string game; // see chess::Game::Serialize

    static Settings Load();
    void Save() const;
};
