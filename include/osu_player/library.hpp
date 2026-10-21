#pragma once

#include "osu_player/track.hpp"

#include <cstddef>
#include <span>
#include <string_view>
#include <vector>

namespace osu_player {

class Library {
public:
    explicit Library(std::vector<Track> tracks = {});

    [[nodiscard]] const std::vector<Track>& tracks() const noexcept;
    [[nodiscard]] std::vector<std::size_t> search(std::string_view query) const;
    // Preserve the supplied scope order; inspect only those tracks. Stale
    // out-of-range indices are omitted, as in the browser's previous filtering.
    [[nodiscard]] std::vector<std::size_t> search(std::string_view query, std::span<const std::size_t> scope) const;

private:
    std::vector<Track> tracks_;
};

}  // namespace osu_player
