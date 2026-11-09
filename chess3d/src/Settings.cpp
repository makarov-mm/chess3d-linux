#include "Settings.h"

#include "Themes.h"

#include <algorithm>
#include <charconv>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <string_view>
#include <system_error>

namespace fs = std::filesystem;

namespace {

using Values = std::map<std::string, std::string, std::less<>>;

// In the configuration folder of the XDG base directories, ~/.config unless set otherwise.
fs::path FilePath()
{
    fs::path folder;
    if (const char* config = std::getenv("XDG_CONFIG_HOME"); config && config[0] == '/')
        folder = config;
    else if (const char* home = std::getenv("HOME"); home && home[0] != '\0')
        folder = fs::path(home) / ".config";
    else
        folder = ".";
    return folder / "chess3d" / "chess3d.ini";
}

std::string_view Trim(std::string_view text)
{
    const auto first = text.find_first_not_of(" \t\r");
    if (first == std::string_view::npos)
        return {};
    return text.substr(first, text.find_last_not_of(" \t\r") - first + 1);
}

// The lines "Key=Value"; the section and comments are passed over.
Values Read(const fs::path& path)
{
    Values values;
    std::ifstream file(path);
    for (std::string line; std::getline(file, line);) {
        const std::string_view text = Trim(line);
        const auto equals = text.find('=');
        if (equals == std::string_view::npos || text.starts_with('[') || text.starts_with(';') || text.starts_with('#'))
            continue;
        values.insert_or_assign(std::string(Trim(text.substr(0, equals))), std::string(Trim(text.substr(equals + 1))));
    }
    return values;
}

int ReadInt(const Values& values, std::string_view key, int fallback)
{
    const auto found = values.find(key);
    if (found == values.end())
        return fallback;
    int value = fallback;
    const std::string& text = found->second;
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    return error == std::errc{} ? value : fallback;
}

// The language of the messages, from the variables gettext reads, in its order.
Language SystemLanguage()
{
    for (const char* name : {"LANGUAGE", "LC_ALL", "LC_MESSAGES", "LANG"}) {
        const char* value = std::getenv(name);
        if (!value || value[0] == '\0')
            continue;
        const std::string_view language(value);
        return language.starts_with("ru") || language.starts_with("uk") || language.starts_with("be") ? Language::Russian : Language::English;
    }
    return Language::English;
}

} // namespace

Settings Settings::Load()
{
    const Values values = Read(FilePath());
    Settings settings;
    settings.theme = std::clamp(ReadInt(values, "Theme", 0), 0, ThemeCount - 1);
    settings.level = std::clamp(ReadInt(values, "Level", 3), 1, 5);
    settings.mode = std::clamp(ReadInt(values, "Mode", 0), 0, 3);
    settings.sound = ReadInt(values, "Sound", 1) != 0;
    settings.panel = ReadInt(values, "Panel", 1) != 0;
    const int language = ReadInt(values, "Language", -1);
    settings.language = language < 0 ? SystemLanguage() : language == 1 ? Language::Russian : Language::English;
    settings.windowWidth = std::max(0, ReadInt(values, "WindowWidth", 0));
    settings.windowHeight = std::max(0, ReadInt(values, "WindowHeight", 0));
    settings.maximized = ReadInt(values, "Maximized", 0) != 0;
    if (const auto game = values.find("Game"); game != values.end())
        settings.game = game->second;
    return settings;
}

void Settings::Save() const
{
    // Written beside the file and then put in its place, so that a crash leaves the old file whole.
    const fs::path path = FilePath();
    std::error_code error;
    fs::create_directories(path.parent_path(), error);
    fs::path temporary = path;
    temporary += ".new";
    {
        std::ofstream file(temporary, std::ios::trunc);
        file << "[Chess3D]\n";
        file << "Theme=" << theme << '\n';
        file << "Level=" << level << '\n';
        file << "Mode=" << mode << '\n';
        file << "Sound=" << (sound ? 1 : 0) << '\n';
        file << "Panel=" << (panel ? 1 : 0) << '\n';
        file << "Language=" << (language == Language::Russian ? 1 : 0) << '\n';
        if (windowWidth > 0 && windowHeight > 0) {
            file << "WindowWidth=" << windowWidth << '\n';
            file << "WindowHeight=" << windowHeight << '\n';
            file << "Maximized=" << (maximized ? 1 : 0) << '\n';
        }
        file << "Game=" << game << '\n';
        file.close();
        if (!file) {
            fs::remove(temporary, error);
            return;
        }
    }
    fs::rename(temporary, path, error);
}
