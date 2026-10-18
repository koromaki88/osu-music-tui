#pragma once

#include <filesystem>

namespace osu_player {

// Temporary line-command frontend until the ncurses controller is available.
int run_play_command(const std::filesystem::path& path);
int run_queue_command(const std::filesystem::path& library_file);

}  // namespace osu_player
