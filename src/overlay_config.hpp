#pragma once

#include <filesystem>
#include <string>

namespace osu_player::overlay_detail {
// Normalized, whitelisted style JSON only; raw config never enters HTML/CSS.
std::string default_style();
std::string parse_style(const std::string& content);
std::string load_style(const std::filesystem::path& path);
void create_style_if_missing(const std::filesystem::path& path);
} // namespace osu_player::overlay_detail
