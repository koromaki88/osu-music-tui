#include "osu_player/library.hpp"

#include <algorithm>
#include <string>
#include <unordered_set>
#include <utility>

namespace osu_player {
namespace {

[[nodiscard]] std::string ascii_fold(const std::string_view text)
{
    std::string result;
    result.reserve(text.size());

    for (const char character : text) {
        if (character >= 'A' && character <= 'Z') {
            result.push_back(static_cast<char>(character + ('a' - 'A')));
        } else {
            result.push_back(character);
        }
    }

    return result;
}

[[nodiscard]] std::string deduplication_key(const Track& track)
{
    std::string result = track.beatmap_set_id;
    result.push_back('\0');
    result.append(track.audio_hash);
    return result;
}

[[nodiscard]] std::string searchable_text(const Track& track)
{
    std::string result;
    result.reserve(track.title.size() + track.title_unicode.size() + track.artist.size() +
                   track.artist_unicode.size() + track.creator.size() + 4);
    result.append(track.title).push_back('\n');
    result.append(track.title_unicode).push_back('\n');
    result.append(track.artist).push_back('\n');
    result.append(track.artist_unicode).push_back('\n');
    result.append(track.creator);
    return ascii_fold(result);
}

}  // namespace

Library::Library(std::vector<Track> tracks)
{
    std::unordered_set<std::string> seen;
    tracks_.reserve(tracks.size());

    for (Track& track : tracks) {
        if (seen.insert(deduplication_key(track)).second) {
            tracks_.push_back(std::move(track));
        }
    }

    std::ranges::sort(tracks_, [](const Track& left, const Track& right) {
        const std::string left_artist = ascii_fold(left.display_artist());
        const std::string right_artist = ascii_fold(right.display_artist());
        if (left_artist != right_artist) {
            return left_artist < right_artist;
        }

        const std::string left_title = ascii_fold(left.display_title());
        const std::string right_title = ascii_fold(right.display_title());
        if (left_title != right_title) {
            return left_title < right_title;
        }

        return left.stable_id < right.stable_id;
    });
}

const std::vector<Track>& Library::tracks() const noexcept
{
    return tracks_;
}

std::vector<std::size_t> Library::search(const std::string_view query) const
{
    const std::string needle = ascii_fold(query);
    std::vector<std::size_t> matches;

    for (std::size_t index = 0; index < tracks_.size(); ++index) {
        if (needle.empty() || searchable_text(tracks_[index]).find(needle) != std::string::npos) {
            matches.push_back(index);
        }
    }

    return matches;
}

}  // namespace osu_player
