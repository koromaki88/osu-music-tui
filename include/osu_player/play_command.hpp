#pragma once

#include <filesystem>

namespace osu_player {

// Temporary line-command frontend until the ncurses controller is available.
int run_play_command(const std::filesystem::path& path);

}  // namespace osu_player
