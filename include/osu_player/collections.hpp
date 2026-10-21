#pragma once

#include "osu_player/library.hpp"

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace osu_player {
namespace collection_limits {
inline constexpr std::size_t file_bytes = 8 * 1024 * 1024;
inline constexpr std::size_t entries = 10000;
inline constexpr std::size_t playlists = 128;
inline constexpr std::size_t name_bytes = 128;
inline constexpr std::size_t total_entries = 50000;
inline constexpr std::size_t pending_edits = 64;
}
struct TrackReference {
    std::string id;
    std::string hash;
    bool operator==(const TrackReference&) const = default;
};
struct Playlist {
    std::string name;
    std::vector<TrackReference> tracks;
};
struct Collections {
    std::vector<TrackReference> favorites;
    std::vector<Playlist> playlists;
    [[nodiscard]] bool favorite(const Track& track) const;
    [[nodiscard]] std::vector<std::size_t> resolve(const Library& library, bool favorites_view, const std::string& name) const;
    // Index uses positions, not string_views, so immutable snapshot copies remain
    // self-contained. Rebuild after a validated mutation, before publication.
    void rebuild_favorite_index();
private:
    std::vector<std::size_t> favorite_order_;
};
enum class CollectionAction { favorite, create, rename, erase, add, remove, earlier, later };
struct CollectionEdit {
    CollectionAction action;
    std::string name;
    std::string replacement;
    TrackReference track;
};
struct CollectionsStatus {
    std::shared_ptr<const Collections> data;
    bool ready{};
    bool writable{};
    std::size_t pending{};
    std::string message;
};
// Owned worker, bounded edit mailbox and one private, exclusively locked file.
// Mutations become visible only after their atomic save succeeds.
class CollectionsStore {
public:
    explicit CollectionsStore(std::filesystem::path path);
    ~CollectionsStore();
    CollectionsStore(const CollectionsStore&) = delete;
    CollectionsStore& operator=(const CollectionsStore&) = delete;
    [[nodiscard]] CollectionsStatus status() const;
    bool submit(CollectionEdit edit);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace osu_player
