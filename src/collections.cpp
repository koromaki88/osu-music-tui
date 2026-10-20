#include "osu_player/collections.hpp"
#include "osu_player/storage_path.hpp"

#include <json-c/json.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#include <sys/random.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <utility>

namespace osu_player {
namespace {
using Json = std::unique_ptr<json_object, decltype(&json_object_put)>;
constexpr std::size_t file_limit = 8 * 1024 * 1024;
constexpr std::size_t entry_limit = 10000;
std::string key(const TrackReference& track) { return track.id + '\0' + track.hash; }
std::string key(const Track& track) { return track.stable_id + '\0' + track.audio_hash; }
void require(const bool condition, const char* message) { if (!condition) { throw std::runtime_error{message}; } }
bool name_valid(const std::string& name)
{
    return !name.empty() && name.size() <= 128 && name.find_first_not_of(' ') != std::string::npos &&
        std::none_of(name.begin(), name.end(), [](const unsigned char c) { return c < 32 || c == 127; });
}
bool valid(const TrackReference& track)
{
    return !track.id.empty() && track.id.size() <= 4096 && track.id.find('\0') == std::string::npos && is_sha256(track.hash);
}
json_object* field(json_object* root, const char* name)
{
    json_object* result{}; json_object_object_get_ex(root, name, &result); return result;
}
std::string text(json_object* root, const char* name)
{
    auto* value = field(root, name);
    require(json_object_is_type(value, json_type_string), "invalid collections string");
    return {json_object_get_string(value), static_cast<std::size_t>(json_object_get_string_len(value))};
}
std::vector<TrackReference> read_tracks(json_object* array)
{
    require(json_object_is_type(array, json_type_array) && json_object_array_length(array) <= entry_limit, "invalid collection entries");
    std::vector<TrackReference> result;
    std::unordered_set<std::string> seen;
    for (std::size_t i = 0; i < json_object_array_length(array); ++i) {
        auto* value = json_object_array_get_idx(array, i);
        TrackReference track{text(value, "id"), text(value, "hash")};
        require(valid(track) && seen.insert(key(track)).second, "invalid or duplicate collection track");
        result.push_back(std::move(track));
    }
    return result;
}
void validate(Collections& data)
{
    require(data.favorites.size() <= entry_limit && data.playlists.size() <= 128, "collection capacity reached");
    std::size_t count = data.favorites.size();
    std::unordered_set<std::string> names;
    data.favorite_keys.clear();
    for (const auto& track : data.favorites) { require(valid(track), "invalid track reference"); data.favorite_keys.insert(key(track)); }
    for (const auto& list : data.playlists) {
        require(name_valid(list.name) && names.insert(list.name).second, "invalid or duplicate playlist name");
        require(list.tracks.size() <= entry_limit, "playlist capacity reached");
        count += list.tracks.size();
    }
    require(count <= 50000, "total collection capacity reached");
}
Collections parse(const std::string& content)
{
    if (content.empty()) { return {}; }
    require(content.find('\0') == std::string::npos, "invalid collections JSON");
    std::unique_ptr<json_tokener, decltype(&json_tokener_free)> parser{json_tokener_new(), json_tokener_free};
    json_tokener_set_flags(parser.get(), JSON_TOKENER_STRICT | JSON_TOKENER_VALIDATE_UTF8);
    Json root{json_tokener_parse_ex(parser.get(), content.c_str(), static_cast<int>(content.size() + 1)), json_object_put};
    auto* version = field(root.get(), "version");
    require(json_tokener_get_error(parser.get()) == json_tokener_success &&
        json_object_is_type(root.get(), json_type_object) && json_object_is_type(version, json_type_int) &&
        json_object_get_int64(version) == 1, "invalid or unsupported collections file");
    Collections data;
    data.favorites = read_tracks(field(root.get(), "favorites"));
    auto* lists = field(root.get(), "playlists");
    require(json_object_is_type(lists, json_type_array) && json_object_array_length(lists) <= 128, "invalid playlists");
    for (std::size_t i = 0; i < json_object_array_length(lists); ++i) {
        auto* list = json_object_array_get_idx(lists, i);
        data.playlists.push_back({text(list, "name"), read_tracks(field(list, "tracks"))});
    }
    validate(data); return data;
}
json_object* tracks_json(const std::vector<TrackReference>& tracks)
{
    auto* array = json_object_new_array();
    for (const auto& track : tracks) {
        auto* value = json_object_new_object();
        json_object_object_add(value, "id", json_object_new_string(track.id.c_str()));
        json_object_object_add(value, "hash", json_object_new_string(track.hash.c_str()));
        json_object_array_add(array, value);
    }
    return array;
}
std::string serialize(const Collections& data)
{
    Json root{json_object_new_object(), json_object_put};
    json_object_object_add(root.get(), "version", json_object_new_int(1));
    json_object_object_add(root.get(), "favorites", tracks_json(data.favorites));
    auto* lists = json_object_new_array();
    json_object_object_add(root.get(), "playlists", lists);
    for (const auto& list : data.playlists) {
        auto* value = json_object_new_object();
        json_object_object_add(value, "name", json_object_new_string(list.name.c_str()));
        json_object_object_add(value, "tracks", tracks_json(list.tracks));
        json_object_array_add(lists, value);
    }
    std::string result{json_object_to_json_string_ext(root.get(), JSON_C_TO_STRING_PLAIN)};
    require(result.size() <= file_limit, "collections file capacity reached");
    return result;
}
struct File {
    int fd{-1};
    explicit File(int value = -1) : fd{value} { }
    ~File() { if (fd >= 0) { close(fd); } }
    File(const File&) = delete;
    File& operator=(const File&) = delete;
};
void safe_file(const int fd)
{
    struct stat info{};
    require(fstat(fd, &info) == 0 && S_ISREG(info.st_mode) && info.st_uid == getuid() &&
        (info.st_mode & 077) == 0 && info.st_nlink == 1, "unsafe collections file owner/type/mode/link");
}
class Storage {
public:
    explicit Storage(const std::filesystem::path& path) : path_{path}
    {
        std::filesystem::create_directories(path.parent_path().parent_path());
        require(mkdir(path.parent_path().c_str(), 0700) == 0 || errno == EEXIST, "cannot create collections directory");
        directory_.fd = open(path.parent_path().c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        current();
        lock_.fd = openat(directory_.fd, ".lock", O_RDWR | O_CREAT | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC, 0600);
        require(lock_.fd >= 0, "cannot open collections lock"); safe_file(lock_.fd);
        require(flock(lock_.fd, LOCK_EX | LOCK_NB) == 0, "collections are locked by another player");
    }
    std::string read() const
    {
        current();
        File file{openat(directory_.fd, path_.filename().c_str(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC)};
        if (file.fd < 0 && errno == ENOENT) { return {}; }
        require(file.fd >= 0, "cannot open collections file"); safe_file(file.fd);
        std::string content;
        char buffer[4096];
        for (;;) {
            const auto bytes = ::read(file.fd, buffer, sizeof(buffer));
            if (bytes < 0 && errno == EINTR) { continue; }
            require(bytes >= 0, "cannot read collections file");
            if (bytes == 0) { break; }
            content.append(buffer, static_cast<std::size_t>(bytes));
            require(content.size() <= file_limit, "collections file too large");
        }
        require(!content.empty(), "empty collections file is invalid");
        return content;
    }
    void write(const std::string& content, const std::string& previous) const
    {
        require(read() == previous, "collections changed externally; restart to reload");
        // All staging operations stay relative to the validated directory handle.
        std::string temporary;
        File file;
        for (int attempt = 0; attempt < 32 && file.fd < 0; ++attempt) {
            std::array<unsigned char, 16> random{};
            std::size_t offset{};
            while (offset < random.size()) {
                const auto bytes = getrandom(random.data() + offset, random.size() - offset, 0);
                if (bytes < 0 && errno == EINTR) { continue; }
                require(bytes > 0, "cannot create random collections filename"); offset += static_cast<std::size_t>(bytes);
            }
            temporary = ".collections-";
            constexpr char hex[] = "0123456789abcdef";
            for (const auto byte : random) { temporary += hex[byte >> 4]; temporary += hex[byte & 15]; }
            file.fd = openat(directory_.fd, temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
            require(file.fd >= 0 || errno == EEXIST, "cannot create collections staging file");
        }
        require(file.fd >= 0, "cannot create collections staging file");
        try {
            current();
            struct stat staged{}, held{};
            require(fstat(file.fd, &staged) == 0 && fstatat(directory_.fd, temporary.c_str(), &held, AT_SYMLINK_NOFOLLOW) == 0 &&
                staged.st_dev == held.st_dev && staged.st_ino == held.st_ino, "collections directory changed");
            std::size_t offset{};
            while (offset < content.size()) {
                const auto bytes = ::write(file.fd, content.data() + offset, content.size() - offset);
                if (bytes < 0 && errno == EINTR) { continue; }
                require(bytes > 0, "cannot write collections file"); offset += static_cast<std::size_t>(bytes);
            }
            require(fsync(file.fd) == 0, "cannot flush collections file");
            require(read() == previous, "collections changed externally; restart to reload");
            current();
            require(renameat(directory_.fd, temporary.c_str(), directory_.fd, path_.filename().c_str()) == 0,
                "cannot replace collections file");
            require(fsync(directory_.fd) == 0, "cannot flush collections directory");
        } catch (...) { unlinkat(directory_.fd, temporary.c_str(), 0); throw; }
    }
private:
    void current() const
    {
        struct stat held{}, named{};
        require(directory_.fd >= 0 && fstat(directory_.fd, &held) == 0 && lstat(path_.parent_path().c_str(), &named) == 0 &&
            S_ISDIR(named.st_mode) && held.st_dev == named.st_dev && held.st_ino == named.st_ino && held.st_uid == getuid() &&
            (held.st_mode & 077) == 0, "unsafe or replaced collections directory");
    }
    std::filesystem::path path_;
    File directory_, lock_;
};
void edit(Collections& data, const CollectionEdit& command)
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
    validate(data);
}
} // namespace

bool Collections::favorite(const Track& track) const { return !favorite_keys.empty() && favorite_keys.contains(key(track)); }
std::vector<std::size_t> Collections::resolve(const Library& library, const bool favorites_view, const std::string& name) const
{
    const std::vector<TrackReference>* tracks = &favorites;
    if (!favorites_view) {
        const auto found = std::find_if(playlists.begin(), playlists.end(), [&](const auto& p) { return p.name == name; });
        if (found == playlists.end()) { return {}; }
        tracks = &found->tracks;
    }
    std::unordered_map<std::string, std::size_t> indices;
    indices.reserve(library.tracks().size());
    for (std::size_t i = 0; i < library.tracks().size(); ++i) { indices.emplace(key(library.tracks()[i]), i); }
    std::vector<std::size_t> result;
    for (const auto& track : *tracks) { if (const auto found = indices.find(key(track)); found != indices.end()) { result.push_back(found->second); } }
    return result;
}

struct CollectionsStore::Impl {
    std::mutex mutex;
    std::condition_variable condition;
    CollectionsStatus state{std::make_shared<Collections>(), false, false, 0, "Collections loading"};
    std::deque<CollectionEdit> edits;
    bool closing{};
    std::jthread worker;
    explicit Impl(std::filesystem::path path) : worker{[this, path = std::move(path)] { run(path); }} { }
    ~Impl()
    {
        { std::lock_guard lock{mutex}; closing = true; }
        condition.notify_all(); // jthread joins after accepted edits have been processed.
    }
    void run(const std::filesystem::path& path)
    {
        try {
            Storage storage{path};
            auto previous = storage.read();
            auto data = std::make_shared<Collections>(parse(previous));
            { std::lock_guard lock{mutex}; state = {data, true, true, 0, "Collections ready"}; }
            for (;;) {
                CollectionEdit command;
                {
                    std::unique_lock lock{mutex};
                    condition.wait(lock, [&] { return closing || !edits.empty(); });
                    if (edits.empty() && closing) { return; }
                    command = std::move(edits.front()); edits.pop_front();
                }
                try {
                    auto candidate = std::make_shared<Collections>(*data);
                    edit(*candidate, command);
                    auto content = serialize(*candidate);
                    (void)parse(content); // Reject invalid UTF-8 even in direct caller input.
                    storage.write(content, previous);
                    previous = std::move(content); data = std::move(candidate);
                    std::lock_guard lock{mutex};
                    state.data = data; --state.pending; state.message = "Collections saved";
                } catch (const std::exception& error) {
                    std::lock_guard lock{mutex}; --state.pending;
                    state.message = "Collections not saved: " + std::string{error.what()};
                }
            }
        } catch (const std::exception& error) {
            std::lock_guard lock{mutex}; state.ready = true; state.writable = false;
            state.message = "Collections unavailable: " + std::string{error.what()};
        }
    }
};
CollectionsStore::CollectionsStore(std::filesystem::path path) : impl_{std::make_unique<Impl>(std::move(path))} { }
CollectionsStore::~CollectionsStore() = default;
CollectionsStatus CollectionsStore::status() const { std::lock_guard lock{impl_->mutex}; return impl_->state; }
bool CollectionsStore::submit(CollectionEdit edit)
{
    std::lock_guard lock{impl_->mutex};
    if (!impl_->state.ready || !impl_->state.writable || impl_->closing) { return false; }
    if (impl_->state.pending >= 64) { impl_->state.message = "Collections busy: edit not accepted; retry"; return false; }
    impl_->edits.push_back(std::move(edit)); ++impl_->state.pending; impl_->state.message = "Collections saving";
    impl_->condition.notify_one(); return true;
}
} // namespace osu_player
