#pragma once

#include <chrono>
#include <filesystem>
#include <memory>
#include <optional>

namespace osu_player {

enum class PlaybackState { empty, ready, playing, paused, finished };

// Control-thread interface: call update() regularly (roughly every 10 ms) while
// playing. The backend owns its decoder/device; no UI or Realm types cross here.
class AudioPlayer {
public:
    using Milliseconds = std::chrono::milliseconds;
    virtual ~AudioPlayer() = default;

    virtual void load(const std::filesystem::path& path) = 0;
    virtual void play() = 0;
    virtual void pause() = 0;
    virtual void stop() = 0;
    virtual void seek(Milliseconds position) = 0;
    virtual void set_volume(float volume) = 0;  // [0, 1], finite
    virtual void update() = 0;

    [[nodiscard]] virtual PlaybackState state() const noexcept = 0;
    [[nodiscard]] virtual Milliseconds position() const noexcept = 0;
    [[nodiscard]] virtual std::optional<Milliseconds> duration() const noexcept = 0;
    [[nodiscard]] virtual float volume() const noexcept = 0;
};

[[nodiscard]] std::unique_ptr<AudioPlayer> make_audio_player();

}  // namespace osu_player
