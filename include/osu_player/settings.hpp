#pragma once

#include "osu_player/player_controller.hpp"

#include <filesystem>
#include <string>

namespace osu_player {

struct Settings {
    float volume{0.7F};
    bool shuffle{};
    RepeatMode repeat{RepeatMode::off};
    std::string search;
    bool discord_enabled{};
    std::string discord_application_id;
    bool lastfm_enabled{};
    bool operator==(const Settings&) const = default;
};

// Throws on unavailable config paths, malformed files, or I/O failures.
std::filesystem::path settings_path();
Settings load_settings(const std::filesystem::path& path);
void save_settings(const std::filesystem::path& path, const Settings& settings);

}  // namespace osu_player
