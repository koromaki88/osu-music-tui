#pragma once

#include "osu_player/lastfm_client.hpp"

#include <deque>
#include <memory>
#include <vector>

namespace osu_player {
namespace lastfm_storage { class Directory; }

struct LastfmScrobble {
    std::string account;
    std::string session_id;
    LastfmParameters parameters; // Original structured metadata + original start timestamp only.
    unsigned int failures{};
    std::int64_t retry_at_ms{};
    bool operator==(const LastfmScrobble&) const = default;
};

std::string lastfm_account(std::string username);
std::optional<LastfmScrobble> lastfm_scrobble(const PlaybackSnapshot& playback, const std::string& account);
// Bound persisted wall-clock deadlines before converting once to a monotonic wait.
std::chrono::milliseconds lastfm_retry_delay(std::int64_t deadline_ms, std::int64_t now_ms) noexcept;

// Worker-only durable outbox. Exclusive nonblocking lock prevents two players
// draining the same queue. No credentials, paths, or audio/library hashes are stored.
class LastfmQueue {
public:
    static constexpr std::size_t entry_limit = 1024;
    static constexpr std::size_t receipt_limit = 4096;
    explicit LastfmQueue(std::filesystem::path path);
    ~LastfmQueue();
    LastfmQueue(const LastfmQueue&) = delete;
    LastfmQueue& operator=(const LastfmQueue&) = delete;
    bool enqueue(const LastfmScrobble& entry); // False means already queued/finished.
    std::optional<LastfmScrobble> front(const std::string& account) const;
    const LastfmScrobble* peek(const std::string& account) const noexcept;
    void finish(const LastfmScrobble& entry);
    void retry(const LastfmScrobble& entry, std::int64_t deadline_ms);
    std::size_t size(const std::string& account) const;

private:
    struct Receipt { std::string account; std::string session_id; };
    void save(const std::vector<LastfmScrobble>& entries, const std::deque<Receipt>& receipts);
    std::filesystem::path path_;
    std::unique_ptr<lastfm_storage::Directory> directory_;
    int lock_fd_{-1};
    std::vector<LastfmScrobble> entries_;
    std::deque<Receipt> receipts_;
};

}  // namespace osu_player
