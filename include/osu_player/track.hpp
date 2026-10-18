#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

namespace osu_player {

struct Track {
    std::string stable_id;
    std::string beatmap_set_id;
    std::optional<std::int64_t> online_set_id;
    std::string title;
    std::string title_unicode;
    std::string artist;
    std::string artist_unicode;
    std::string creator;
    std::string audio_filename;
    std::string audio_hash;
    std::filesystem::path audio_path;
    std::optional<std::int64_t> duration_ms;
    std::optional<double> bpm;
    std::optional<std::string> date_added;

    [[nodiscard]] const std::string& display_title() const noexcept;
    [[nodiscard]] const std::string& display_artist() const noexcept;
};

}  // namespace osu_player
