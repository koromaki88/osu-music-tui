#pragma once

#include "osu_player/audio_player.hpp"
#include "osu_player/library.hpp"
#include "osu_player/playback_events.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <vector>

namespace osu_player {

enum class PlayerState { empty, stopped, playing, paused, finished, failed };
enum class RepeatMode { off, one, all };

struct TrackError {
    std::string stable_id;
    std::string message;
};

struct PlaybackErrors {
    std::vector<TrackError> tracks;
    std::size_t omitted{};
};

// Owns its library and backend. All methods run on one control thread. The queue
// stores library indices, remapped on refresh rather than retaining track pointers.
class PlayerController {
public:
    PlayerController(Library library, std::unique_ptr<AudioPlayer> audio,
                     std::uint32_t random_seed = std::random_device{}(), PlaybackClock clock = {});

    void play();
    void play_track(std::size_t library_index);
    // Explicitly start a new playback scope. Validation is transactional; next,
    // previous, repeat and shuffle remain within this ordered, unique subset.
    void play_queue(std::vector<std::size_t> indices, std::size_t selected, bool restricted = true);
    void pause();
    void stop();
    void next();
    void previous();
    void update();
    void seek(AudioPlayer::Milliseconds position);
    // Close the active play session on normal frontend exit, without rewinding.
    void shutdown();
    void set_volume(float volume);
    void set_muted(bool muted);
    void set_shuffle(bool enabled);
    void set_repeat(RepeatMode mode) noexcept;
    // Keeps the loaded stream untouched when stable ID + hash survives. If it
    // disappears, pause and clear selection (never autoplay a replacement).
    void replace_library(Library library);

    [[nodiscard]] const Library& library() const noexcept;
    [[nodiscard]] const std::vector<std::size_t>& queue() const noexcept;
    [[nodiscard]] std::optional<std::size_t> current_index() const noexcept;
    [[nodiscard]] const Track* current_track() const noexcept;
    [[nodiscard]] PlayerState state() const noexcept;
    [[nodiscard]] bool shuffle() const noexcept;
    [[nodiscard]] RepeatMode repeat() const noexcept;
    [[nodiscard]] AudioPlayer::Milliseconds position() const noexcept;
    [[nodiscard]] std::optional<AudioPlayer::Milliseconds> duration() const noexcept;
    [[nodiscard]] float volume() const noexcept;
    [[nodiscard]] bool muted() const noexcept;
    [[nodiscard]] PlaybackErrors take_errors();
    [[nodiscard]] const std::optional<PlaybackSnapshot>& playback_snapshot() const noexcept;
    [[nodiscard]] PlaybackEvents take_playback_events();

private:
    void start_from(std::size_t queue_position, bool backwards, bool wrap, bool autoplay = true);
    void finish();
    void recover(const std::string& message, bool keep_paused = false);
    void record_error(std::size_t library_index, const std::string& message);
    void reshuffle_cycle();
    void fail_session();
    void end_playback(PlaybackEndReason reason);

    Library library_;
    std::unique_ptr<AudioPlayer> audio_;
    std::vector<std::size_t> queue_;
    std::vector<std::size_t> queue_order_;
    bool restricted_queue_{};
    std::vector<bool> failed_tracks_;
    std::optional<std::size_t> cursor_;
    std::mt19937 random_;
    PlayerState state_{PlayerState::empty};
    RepeatMode repeat_{RepeatMode::off};
    bool shuffle_{};
    float volume_{0.7F};
    bool muted_{};
    PlaybackErrors errors_;
    PlaybackMonitor playback_;
};

}  // namespace osu_player
