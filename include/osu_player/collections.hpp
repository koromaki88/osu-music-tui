#pragma once

#include "osu_player/library.hpp"

#include <filesystem>
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

namespace osu_player {
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
    std::unordered_set<std::string> favorite_keys;
    [[nodiscard]] bool favorite(const Track& track) const;
    [[nodiscard]] std::vector<std::size_t> resolve(const Library& library, bool favorites_view, const std::string& name) const;
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
