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

// Compare folded bytes directly: sorting must not allocate two artist/title
// strings for each of its O(n log n) comparisons. Unsigned bytes preserve the
// existing std::string ordering for non-ASCII UTF-8 as well as ASCII prefixes.
[[nodiscard]] int compare_folded(const std::string_view left, const std::string_view right)
{
    const auto fold = [](const char character) {
        const auto byte = static_cast<unsigned char>(character);
        return byte >= 'A' && byte <= 'Z' ? static_cast<unsigned char>(byte + ('a' - 'A')) : byte;
    };
    const auto common = std::min(left.size(), right.size());
    for (std::size_t index = 0; index < common; ++index) {
        const auto a = fold(left[index]);
        const auto b = fold(right[index]);
        if (a != b) { return a < b ? -1 : 1; }
    }
    return left.size() == right.size() ? 0 : left.size() < right.size() ? -1 : 1;
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
        if (const auto artist = compare_folded(left.display_artist(), right.display_artist()); artist != 0) {
            return artist < 0;
        }

        if (const auto title = compare_folded(left.display_title(), right.display_title()); title != 0) {
            return title < 0;
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
