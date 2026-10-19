#pragma once

#include "osu_player/track.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace osu_player {

struct PlaybackTime {
    std::chrono::steady_clock::time_point monotonic;
    std::chrono::system_clock::time_point wall;
};

using PlaybackClock = std::function<PlaybackTime()>;

// Copied metadata, deliberately excluding content-store paths and filenames.
struct PlaybackMetadata {
    std::string stable_id;
    std::string audio_hash;
    std::string title;
    std::string title_unicode;
    std::string artist;
    std::string artist_unicode;
    std::optional<std::int64_t> online_set_id;
    bool operator==(const PlaybackMetadata&) const = default;
};

struct PlaybackSnapshot {
    std::string session_id;
    PlaybackMetadata track;
    std::chrono::system_clock::time_point started_at;
    std::chrono::milliseconds position{};
    std::optional<std::chrono::milliseconds> duration;
    std::chrono::milliseconds listened{};
    bool playing{};
    // Timing eligibility only: metadata, authorization, and submission are separate.
    bool listening_eligible{};
};

enum class PlaybackEventType { started, progress, paused, resumed, seeked, metadata_changed,
                               listening_eligible, ended };
enum class PlaybackEndReason { none, completed, skipped, replaced, stopped, track_error,
                               output_error, removed, shutdown };

struct PlaybackEvent {
    PlaybackEventType type;
    PlaybackEndReason reason{PlaybackEndReason::none};
    PlaybackTime time;
    PlaybackSnapshot playback;
};

struct PlaybackEvents {
    std::vector<PlaybackEvent> events;
    std::size_t omitted{};
};

// Control-thread-only journal; no callbacks, sockets, disk I/O, or worker threads.
// Consumers drain once and fan out copies to their own workers. Progress coalesces;
// overflow is explicit, never silently treated as a complete listening history.
class PlaybackMonitor {
public:
    using Milliseconds = std::chrono::milliseconds;
    static constexpr std::size_t event_limit = 128;

    explicit PlaybackMonitor(PlaybackClock clock = {});
    void begin(const Track& track, Milliseconds position, std::optional<Milliseconds> duration);
    void sample(Milliseconds position, std::optional<Milliseconds> duration);
    void pause(Milliseconds position, std::optional<Milliseconds> duration);
    void resume(Milliseconds position, std::optional<Milliseconds> duration);
    // Call sample before the backend seek, then rebase afterward: skipped media
    // time must never be counted as listening time.
    void seek(Milliseconds position, std::optional<Milliseconds> duration);
    void metadata(const Track& track);
    void end(PlaybackEndReason reason, Milliseconds position, std::optional<Milliseconds> duration);

    [[nodiscard]] const std::optional<PlaybackSnapshot>& snapshot() const noexcept;
    [[nodiscard]] PlaybackEvents take_events();

private:
    void sample_at(PlaybackTime now, Milliseconds position, std::optional<Milliseconds> duration);
    void emit(PlaybackEventType type, PlaybackTime time, PlaybackEndReason reason = PlaybackEndReason::none);
    void eligibility(PlaybackTime time);

    PlaybackClock clock_;
    std::string instance_id_;
    std::uint64_t sequence_{};
    std::optional<PlaybackSnapshot> current_;
    std::chrono::steady_clock::time_point last_sample_{};
    std::chrono::steady_clock::duration listened_{};
    std::chrono::steady_clock::duration uncredited_elapsed_{};
    std::deque<PlaybackEvent> events_;
    std::size_t omitted_{};
};

}  // namespace osu_player
