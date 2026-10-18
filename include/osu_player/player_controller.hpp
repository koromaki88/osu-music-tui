#pragma once

#include "osu_player/audio_player.hpp"
#include "osu_player/library.hpp"

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
// stores library indices, not pointers that would be invalidated by reordering.
class PlayerController {
public:
    PlayerController(Library library, std::unique_ptr<AudioPlayer> audio,
                     std::uint32_t random_seed = std::random_device{}());

    void play();
    void play_track(std::size_t library_index);
    void pause();
    void stop();
    void next();
    void previous();
    void update();
    void seek(AudioPlayer::Milliseconds position);
    void set_volume(float volume);
    void set_shuffle(bool enabled);
    void set_repeat(RepeatMode mode) noexcept;

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
    [[nodiscard]] PlaybackErrors take_errors();

private:
    void start_from(std::size_t queue_position, bool backwards, bool wrap, bool autoplay = true);
    void finish();
    void recover(const std::string& message, bool keep_paused = false);
    void record_error(std::size_t library_index, const std::string& message);
    void reshuffle_cycle();
    void fail_session();

    Library library_;
    std::unique_ptr<AudioPlayer> audio_;
    std::vector<std::size_t> queue_;
    std::vector<bool> failed_tracks_;
    std::optional<std::size_t> cursor_;
    std::mt19937 random_;
    PlayerState state_{PlayerState::empty};
    RepeatMode repeat_{RepeatMode::off};
    bool shuffle_{};
    PlaybackErrors errors_;
};

}  // namespace osu_player
