#pragma once

#include "osu_player/track.hpp"

#include <cstddef>
#include <string_view>
#include <vector>

namespace osu_player {

class Library {
public:
    explicit Library(std::vector<Track> tracks = {});

    [[nodiscard]] const std::vector<Track>& tracks() const noexcept;
    [[nodiscard]] std::vector<std::size_t> search(std::string_view query) const;

private:
    std::vector<Track> tracks_;
};

}  // namespace osu_player
