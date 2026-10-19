#pragma once

#include "osu_player/audio_player.hpp"

#include <cmath>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

class FakeAudioPlayer final : public osu_player::AudioPlayer {
public:
    void load(const std::filesystem::path& path) override
    {
        attempts.push_back(path.string());
        if (fail_output_load) {
            throw osu_player::AudioOutputError{"fake output device failure"};
        }
        if (bad_loads.contains(path.string())) {
            throw std::runtime_error{"fake load failure"};
        }
        source = path.string();
        playback = osu_player::PlaybackState::ready;
        position_value = Milliseconds{0};
    }

    void play() override
    {
        ++play_calls;
        if (bad_plays.contains(source)) {
            throw std::runtime_error{"fake play failure"};
        }
        playback = osu_player::PlaybackState::playing;
    }

    void pause() override
    {
        if (playback == osu_player::PlaybackState::playing) {
            playback = osu_player::PlaybackState::paused;
        }
    }

    void stop() override
    {
        if (fail_stop) {
            throw std::runtime_error{"fake stop failure"};
        }
        playback = osu_player::PlaybackState::ready;
        position_value = Milliseconds{0};
    }

    void seek(const Milliseconds target) override
    {
        if (fail_seek) {
            throw std::runtime_error{"fake seek failure"};
        }
        position_value = target;
        if (playback == osu_player::PlaybackState::finished) {
            playback = osu_player::PlaybackState::paused;
        }
    }

    void set_volume(const float value) override
    {
        if (!std::isfinite(value) || value < 0.0F || value > 1.0F) {
            throw std::invalid_argument{"invalid fake volume"};
        }
        volume_value = value;
    }

    void update() override
    {
        ++update_calls;
        if (fail_output_update) {
            throw osu_player::AudioOutputError{"fake output device failure"};
        }
        if (bad_updates.contains(source)) {
            throw std::runtime_error{"fake decode failure"};
        }
        if (finish_on_update) {
            playback = osu_player::PlaybackState::finished;
            finish_on_update = false;
        }
    }

    [[nodiscard]] osu_player::PlaybackState state() const noexcept override { return playback; }
    [[nodiscard]] Milliseconds position() const noexcept override { return position_value; }
    [[nodiscard]] std::optional<Milliseconds> duration() const noexcept override { return duration_value; }
    [[nodiscard]] float volume() const noexcept override { return volume_value; }

    std::string source;
    std::vector<std::string> attempts;
    std::set<std::string> bad_loads;
    std::set<std::string> bad_plays;
    std::set<std::string> bad_updates;
    std::size_t play_calls{};
    std::size_t update_calls{};
    bool finish_on_update{};
    bool fail_seek{};
    bool fail_stop{};
    bool fail_output_load{};
    bool fail_output_update{};
    osu_player::PlaybackState playback{osu_player::PlaybackState::empty};
    Milliseconds position_value{};
    std::optional<Milliseconds> duration_value{Milliseconds{1000}};
    float volume_value{0.7F};
};
