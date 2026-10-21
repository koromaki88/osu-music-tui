#include "collections_file.hpp"
#include "osu_player/storage_path.hpp"

#include <algorithm>
#include <limits>
#include <numeric>
#include <stdexcept>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace osu_player {
namespace {
void require(const bool condition, const char* message) { if (!condition) { throw std::runtime_error{message}; } }

// Direct API edits need the same UTF-8 guarantee as decoded JSON, without
// serializing and parsing a second copy of the entire collections file.
bool valid_utf8(const std::string_view text)
{
    std::size_t offset{};
    while (offset < text.size()) {
        const auto first = static_cast<unsigned char>(text[offset++]);
        if (first < 0x80) { continue; }
        int remaining{};
        char32_t value{}, minimum{};
        if (first >= 0xC2 && first <= 0xDF) { remaining = 1; value = first & 0x1FU; minimum = 0x80; }
        else if (first >= 0xE0 && first <= 0xEF) { remaining = 2; value = first & 0x0FU; minimum = 0x800; }
        else if (first >= 0xF0 && first <= 0xF4) { remaining = 3; value = first & 0x07U; minimum = 0x10000; }
        else { return false; }
        for (int i = 0; i < remaining; ++i) {
            if (offset >= text.size()) { return false; }
            const auto byte = static_cast<unsigned char>(text[offset++]);
            if ((byte & 0xC0U) != 0x80U) { return false; }
            value = (value << 6U) | (byte & 0x3FU);
        }
        if (value < minimum || value > 0x10FFFF || (value >= 0xD800 && value <= 0xDFFF)) { return false; }
    }
    return true;
}
bool name_valid(const std::string& name)
{
    return !name.empty() && name.size() <= collection_limits::name_bytes && name.find_first_not_of(' ') != std::string::npos &&
        std::none_of(name.begin(), name.end(), [](const unsigned char c) { return c < 32 || c == 127; }) && valid_utf8(name);
}
bool valid(const TrackReference& track)
{
    return !track.id.empty() && track.id.size() <= 4096 && track.id.find('\0') == std::string::npos &&
        is_sha256(track.hash) && valid_utf8(track.id);
}
using ReferenceView = std::pair<std::string_view, std::string_view>;
ReferenceView view(const TrackReference& track) { return {track.id, track.hash}; }
ReferenceView view(const Track& track) { return {track.stable_id, track.audio_hash}; }
struct ReferenceHash {
    std::size_t operator()(const ReferenceView& track) const noexcept
    {
        const auto a = std::hash<std::string_view>{}(track.first);
        const auto b = std::hash<std::string_view>{}(track.second);
        return a ^ (b + 0x9e3779b9U + (a << 6U) + (a >> 2U));
    }
};
} // namespace

void Collections::rebuild_favorite_index()
{
    favorite_order_.resize(favorites.size());
    std::iota(favorite_order_.begin(), favorite_order_.end(), std::size_t{0});
    std::ranges::sort(favorite_order_, [&](const auto a, const auto b) { return view(favorites[a]) < view(favorites[b]); });
}
bool Collections::favorite(const Track& track) const
{
    const auto query = view(track);
    const auto found = std::lower_bound(favorite_order_.begin(), favorite_order_.end(), query,
        [&](const auto index, const auto& wanted) { return view(favorites[index]) < wanted; });
    return found != favorite_order_.end() && view(favorites[*found]) == query;
}
std::vector<std::size_t> Collections::resolve(const Library& library, const bool favorites_view, const std::string& name) const
{
    const std::vector<TrackReference>* tracks = &favorites;
    if (!favorites_view) {
        const auto found = std::find_if(playlists.begin(), playlists.end(), [&](const auto& p) { return p.name == name; });
        if (found == playlists.end()) { return {}; }
        tracks = &found->tracks;
    }
    if (tracks->empty()) { return {}; }
    constexpr auto missing = std::numeric_limits<std::size_t>::max();
    // Index the requested list, not every library track. Views live only for
    // this call and refer to the immutable owning snapshot.
    std::unordered_map<ReferenceView, std::size_t, ReferenceHash> indices;
    indices.reserve(tracks->size());
    for (const auto& track : *tracks) { indices.emplace(view(track), missing); }
    for (std::size_t i = 0; i < library.tracks().size(); ++i) {
        const auto found = indices.find(view(library.tracks()[i]));
        if (found != indices.end() && found->second == missing) { found->second = i; }
    }
    std::vector<std::size_t> result;
    result.reserve(tracks->size());
    for (const auto& track : *tracks) {
        const auto found = indices.find(view(track));
        if (found->second != missing) { result.push_back(found->second); }
    }
    return result;
}

namespace collection_detail {
void validate_collections(Collections& data)
{
    require(data.favorites.size() <= collection_limits::entries && data.playlists.size() <= collection_limits::playlists, "collection capacity reached");
    std::size_t count = data.favorites.size();
    std::unordered_set<std::string_view> names;
    const auto validate_tracks = [](const std::vector<TrackReference>& tracks) {
        std::unordered_set<ReferenceView, ReferenceHash> seen;
        seen.reserve(tracks.size());
        for (const auto& track : tracks) { require(valid(track) && seen.insert(view(track)).second, "invalid or duplicate collection track"); }
    };
    validate_tracks(data.favorites);
    for (const auto& list : data.playlists) {
        require(name_valid(list.name) && names.insert(list.name).second, "invalid or duplicate playlist name");
        require(list.tracks.size() <= collection_limits::entries, "playlist capacity reached");
        count += list.tracks.size();
        require(count <= collection_limits::total_entries, "total collection capacity reached");
        validate_tracks(list.tracks);
    }
    data.rebuild_favorite_index();
}
void apply_collection_edit(Collections& data, const CollectionEdit& command)
{
    if (command.action == CollectionAction::favorite) {
        require(valid(command.track), "no valid track selected");
        const auto found = std::find(data.favorites.begin(), data.favorites.end(), command.track);
        if (found == data.favorites.end()) { data.favorites.push_back(command.track); }
        else { data.favorites.erase(found); }
    } else if (command.action == CollectionAction::create) {
        require(name_valid(command.name) && std::none_of(data.playlists.begin(), data.playlists.end(),
            [&](const auto& p) { return p.name == command.name; }), "invalid or duplicate playlist name");
        data.playlists.push_back({command.name, {}});
    } else {
        const auto found = std::find_if(data.playlists.begin(), data.playlists.end(), [&](const auto& p) { return p.name == command.name; });
        require(found != data.playlists.end(), "playlist no longer exists");
        if (command.action == CollectionAction::erase) { data.playlists.erase(found); }
        else if (command.action == CollectionAction::rename) {
            require(name_valid(command.replacement) && std::none_of(data.playlists.begin(), data.playlists.end(),
                [&](const auto& p) { return p.name == command.replacement && &p != &*found; }), "invalid or duplicate playlist name");
            found->name = command.replacement;
        } else {
            require(valid(command.track), "no valid track selected");
            auto& tracks = found->tracks;
            const auto item = std::find(tracks.begin(), tracks.end(), command.track);
            if (command.action == CollectionAction::add) { if (item == tracks.end()) { tracks.push_back(command.track); } }
            else if (item != tracks.end()) {
                if (command.action == CollectionAction::remove) { tracks.erase(item); }
                else if (command.action == CollectionAction::earlier && item != tracks.begin()) { std::iter_swap(item, item - 1); }
                else if (command.action == CollectionAction::later && item + 1 != tracks.end()) { std::iter_swap(item, item + 1); }
            }
        }
    }
    validate_collections(data);
}
} // namespace collection_detail
} // namespace osu_player
