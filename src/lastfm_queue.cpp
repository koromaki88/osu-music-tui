#include "osu_player/lastfm_queue.hpp"

#include <json-c/json.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <memory>
#include <stdexcept>

namespace osu_player {
namespace {
using Json = std::unique_ptr<json_object, decltype(&json_object_put)>;
constexpr std::size_t byte_limit = 8 * 1024 * 1024;
struct File { int fd{-1}; ~File() { if (fd >= 0) { close(fd); } } };
[[noreturn]] void failure(const int code) { throw LastfmFailure{LastfmFailureKind::queue, code}; }
bool identity(const std::string& value)
{
    if (value.empty() || value.size() > 256) { return false; }
    return std::all_of(value.begin(), value.end(), [](const char byte) { return byte >= 0x21 && byte <= 0x7e; });
}
json_object* field(json_object* root, const char* name)
{
    json_object* value{}; json_object_object_get_ex(root, name, &value); return value;
}
std::string text(json_object* root, const char* name)
{
    auto* value = field(root, name);
    if (!json_object_is_type(value, json_type_string)) { failure(1); }
    return {json_object_get_string(value), static_cast<std::size_t>(json_object_get_string_len(value))};
}
std::int64_t number(json_object* root, const char* name)
{
    auto* value = field(root, name);
    if (!json_object_is_type(value, json_type_int)) { failure(1); }
    return json_object_get_int64(value);
}
void add(json_object* root, const char* name, const std::string& value)
{
    json_object_object_add(root, name, json_object_new_string_len(value.data(), static_cast<int>(value.size())));
}
void validate(const LastfmScrobble& entry)
{
    if (!identity(entry.account) || lastfm_account(entry.account) != entry.account || !identity(entry.session_id) ||
        entry.failures > 1000000 || entry.retry_at_ms < 0 || entry.retry_at_ms > 4102444800000LL) { failure(1); }
    // Round-trip through the metadata validator; accept no unexpected request keys.
    if (entry.parameters.size() != 5 || !entry.parameters.contains("artist") || !entry.parameters.contains("track") ||
        !entry.parameters.contains("timestamp") || !entry.parameters.contains("duration") ||
        !entry.parameters.contains("method") || entry.parameters.at("method") != "track.scrobble") { failure(1); }
    PlaybackSnapshot snapshot;
    snapshot.playing = true;
    snapshot.track.artist = entry.parameters.at("artist"); snapshot.track.title = entry.parameters.at("track");
    if (!lastfm_now_playing(snapshot)) { failure(1); }
    for (const auto* name : {"timestamp", "duration"}) {
        const auto& value = entry.parameters.at(name);
        if (value.empty() || value.size() > 12 || !std::all_of(value.begin(), value.end(), [](char c) { return c >= '0' && c <= '9'; })) { failure(1); }
        if (value.front() == '0' || (std::string{name} == "duration" && std::stoll(value) < 30)) { failure(1); }
    }
}
}  // namespace

std::string lastfm_account(std::string username)
{
    for (char& byte : username) { if (byte >= 'A' && byte <= 'Z') { byte = static_cast<char>(byte - 'A' + 'a'); } }
    return username;
}

std::optional<LastfmScrobble> lastfm_scrobble(const PlaybackSnapshot& playback, const std::string& account)
{
    if (!playback.listening_eligible || !playback.duration || *playback.duration <= std::chrono::seconds{30} ||
        playback.listened < std::min(*playback.duration / 2, std::chrono::milliseconds{std::chrono::minutes{4}}) ||
        !identity(playback.session_id) || !identity(account)) { return std::nullopt; }
    auto source = playback; source.playing = true; // Eligibility is valid even when ending/paused.
    auto parameters = lastfm_now_playing(source);
    if (!parameters) { return std::nullopt; }
    const auto timestamp = std::chrono::duration_cast<std::chrono::seconds>(playback.started_at.time_since_epoch()).count();
    if (timestamp <= 0 || timestamp > 4102444800LL) { return std::nullopt; }
    (*parameters)["method"] = "track.scrobble";
    (*parameters)["timestamp"] = std::to_string(timestamp);
    return LastfmScrobble{lastfm_account(account), playback.session_id, std::move(*parameters), 0, 0};
}

LastfmQueue::LastfmQueue(std::filesystem::path path) : path_{std::move(path)}
{
    // The credential writer creates this private directory. Never chmod user data.
    struct stat directory{};
    if (lstat(path_.parent_path().c_str(), &directory) != 0 || !S_ISDIR(directory.st_mode) ||
        directory.st_uid != getuid() || (directory.st_mode & 077) != 0) { failure(2); }
    File lock{open((path_.string() + ".lock").c_str(), O_RDWR | O_CREAT | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC, 0600)};
    struct stat info{};
    if (lock.fd < 0 || fstat(lock.fd, &info) != 0 || !S_ISREG(info.st_mode) || info.st_uid != getuid() ||
        (info.st_mode & 077) != 0) { failure(2); }
    if (flock(lock.fd, LOCK_EX | LOCK_NB) != 0) { failure(3); }
    File file{open(path_.c_str(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC)};
    if (file.fd < 0 && errno != ENOENT) { failure(2); }
    if (file.fd >= 0) {
        if (fstat(file.fd, &info) != 0 || !S_ISREG(info.st_mode) || info.st_uid != getuid() ||
            (info.st_mode & 077) != 0 || info.st_size < 0 || static_cast<std::uint64_t>(info.st_size) > byte_limit) { failure(2); }
        std::string content;
        std::array<char, 8192> buffer{};
        while (true) {
            const auto bytes = read(file.fd, buffer.data(), buffer.size());
            if (bytes < 0 && errno == EINTR) { continue; }
            if (bytes < 0) { failure(2); }
            if (bytes == 0) { break; }
            content.append(buffer.data(), static_cast<std::size_t>(bytes));
            if (content.size() > byte_limit) { failure(1); }
        }
        if (content.find('\0') != std::string::npos) { failure(1); }
        std::unique_ptr<json_tokener, decltype(&json_tokener_free)> parser{json_tokener_new(), json_tokener_free};
        json_tokener_set_flags(parser.get(), JSON_TOKENER_STRICT | JSON_TOKENER_VALIDATE_UTF8);
        Json root{json_tokener_parse_ex(parser.get(), content.c_str(), static_cast<int>(content.size() + 1)), json_object_put};
        if (json_tokener_get_error(parser.get()) != json_tokener_success || number(root.get(), "version") != 1) { failure(1); }
        auto* pending = field(root.get(), "pending"); auto* finished = field(root.get(), "receipts");
        if (!json_object_is_type(pending, json_type_array) || !json_object_is_type(finished, json_type_array) ||
            json_object_array_length(pending) > entry_limit || json_object_array_length(finished) > receipt_limit) { failure(1); }
        for (std::size_t index = 0; index < json_object_array_length(pending); ++index) {
            auto* item = json_object_array_get_idx(pending, index);
            const auto attempts = number(item, "failures");
            if (attempts < 0 || attempts > 1000000) { failure(1); }
            LastfmScrobble entry{text(item, "account"), text(item, "session"), {}, static_cast<unsigned int>(attempts), number(item, "retry_at_ms")};
            for (const auto* name : {"method", "artist", "track", "duration", "timestamp"}) { entry.parameters[name] = text(item, name); }
            validate(entry);
            if (std::any_of(entries_.begin(), entries_.end(), [&](const auto& old) { return old.account == entry.account && old.session_id == entry.session_id; })) { failure(1); }
            entries_.push_back(std::move(entry));
        }
        for (std::size_t index = 0; index < json_object_array_length(finished); ++index) {
            auto* item = json_object_array_get_idx(finished, index);
            Receipt receipt{text(item, "account"), text(item, "session")};
            if (!identity(receipt.account) || lastfm_account(receipt.account) != receipt.account || !identity(receipt.session_id)) { failure(1); }
            if (std::any_of(entries_.begin(), entries_.end(), [&](const auto& old) { return old.account == receipt.account && old.session_id == receipt.session_id; })) { failure(1); }
            receipts_.push_back(std::move(receipt));
        }
    }
    lock_fd_ = lock.fd; lock.fd = -1;
}

LastfmQueue::~LastfmQueue() { if (lock_fd_ >= 0) { close(lock_fd_); } }

void LastfmQueue::save(const std::vector<LastfmScrobble>& entries, const std::deque<Receipt>& receipts)
{
    Json root{json_object_new_object(), json_object_put};
    json_object_object_add(root.get(), "version", json_object_new_int(1));
    auto* pending = json_object_new_array(); auto* finished = json_object_new_array();
    json_object_object_add(root.get(), "pending", pending); json_object_object_add(root.get(), "receipts", finished);
    for (const auto& entry : entries) {
        auto* item = json_object_new_object(); json_object_array_add(pending, item);
        add(item, "account", entry.account); add(item, "session", entry.session_id);
        for (const auto& [key, value] : entry.parameters) { add(item, key.c_str(), value); }
        json_object_object_add(item, "failures", json_object_new_int64(entry.failures));
        json_object_object_add(item, "retry_at_ms", json_object_new_int64(entry.retry_at_ms));
    }
    for (const auto& receipt : receipts) {
        auto* item = json_object_new_object(); json_object_array_add(finished, item);
        add(item, "account", receipt.account); add(item, "session", receipt.session_id);
    }
    const std::string content = json_object_to_json_string_ext(root.get(), JSON_C_TO_STRING_PLAIN);
    if (content.size() > byte_limit) { failure(4); }
    auto pattern = (path_.parent_path() / ".scrobbles-XXXXXX").string();
    std::vector<char> staging(pattern.begin(), pattern.end()); staging.push_back('\0');
    File file{mkstemp(staging.data())};
    if (file.fd < 0) { failure(2); }
    try {
        std::size_t offset{};
        while (offset < content.size()) {
            const auto bytes = write(file.fd, content.data() + offset, content.size() - offset);
            if (bytes < 0 && errno == EINTR) { continue; }
            if (bytes <= 0) { failure(2); }
            offset += static_cast<std::size_t>(bytes);
        }
        if (fsync(file.fd) != 0 || rename(staging.data(), path_.c_str()) != 0) { failure(2); }
        File directory{open(path_.parent_path().c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC)};
        if (directory.fd < 0 || fsync(directory.fd) != 0) { failure(2); }
    } catch (...) { unlink(staging.data()); throw; }
}

bool LastfmQueue::enqueue(const LastfmScrobble& entry)
{
    validate(entry);
    const auto same = [&](const auto& old) { return old.account == entry.account && old.session_id == entry.session_id; };
    if (std::any_of(entries_.begin(), entries_.end(), same) || std::any_of(receipts_.begin(), receipts_.end(), same)) { return false; }
    if (entries_.size() >= entry_limit) { failure(4); }
    auto updated = entries_; updated.push_back(entry); save(updated, receipts_); entries_ = std::move(updated); return true;
}

std::optional<LastfmScrobble> LastfmQueue::front(const std::string& account) const
{
    const auto found = std::find_if(entries_.begin(), entries_.end(), [&](const auto& entry) { return entry.account == account; });
    return found == entries_.end() ? std::nullopt : std::optional{*found};
}
void LastfmQueue::finish(const LastfmScrobble& entry)
{
    auto updated = entries_;
    std::erase_if(updated, [&](const auto& old) { return old.account == entry.account && old.session_id == entry.session_id; });
    auto receipts = receipts_; receipts.push_back({entry.account, entry.session_id});
    while (receipts.size() > receipt_limit) { receipts.pop_front(); }
    save(updated, receipts); entries_ = std::move(updated); receipts_ = std::move(receipts);
}
void LastfmQueue::retry(const LastfmScrobble& entry, const std::int64_t deadline_ms)
{
    auto updated = entries_;
    for (auto& item : updated) {
        if (item.account == entry.account && item.session_id == entry.session_id) {
            item.failures = std::min(item.failures + 1, 1000000U); item.retry_at_ms = deadline_ms;
        }
    }
    save(updated, receipts_); entries_ = std::move(updated);
}
std::size_t LastfmQueue::size(const std::string& account) const
{
    return static_cast<std::size_t>(std::count_if(entries_.begin(), entries_.end(), [&](const auto& entry) { return entry.account == account; }));
}
}  // namespace osu_player
