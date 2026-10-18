#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

namespace osu_player {

struct AudioMetadata {
    std::string container;
    std::string codec;
    std::int64_t duration_ms{};
    int sample_rate{};
    int channels{};
};

[[nodiscard]] AudioMetadata probe_audio(const std::filesystem::path& path);

}  // namespace osu_player
