#pragma once

#include "osu_player/collections.hpp"

namespace osu_player::collection_detail {

Collections decode_collections(const std::string& content);
std::string encode_collections(const Collections& data);
void validate_collections(Collections& data);
void apply_collection_edit(Collections& data, const CollectionEdit& command);

// Storage owns the private directory/lock. Format and filesystem details never
// enter the worker mailbox or the UI.
class CollectionFile {
public:
    explicit CollectionFile(const std::filesystem::path& path);
    ~CollectionFile();
    CollectionFile(const CollectionFile&) = delete;
    CollectionFile& operator=(const CollectionFile&) = delete;
    std::string read() const;
    void write(const std::string& content, const std::string& previous) const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace osu_player::collection_detail
