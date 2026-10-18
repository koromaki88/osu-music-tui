#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace osu_player {

[[nodiscard]] bool is_sha256(std::string_view value) noexcept;

[[nodiscard]] std::string normalize_sha256(std::string_view value);

[[nodiscard]] std::filesystem::path content_path(
    const std::filesystem::path& osu_data_directory,
    std::string_view hash);

[[nodiscard]] std::filesystem::path default_osu_data_directory();

}  // namespace osu_player
