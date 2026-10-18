#include "osu_player/player_controller.hpp"

#include <algorithm>
#include <numeric>
#include <stdexcept>
#include <utility>

namespace osu_player {

PlayerController::PlayerController(Library library, std::unique_ptr<AudioPlayer> audio,
                                   const std::uint32_t random_seed)
    : library_{std::move(library)}, audio_{std::move(audio)}, random_{random_seed}
{
    if (!audio_) {
        throw std::invalid_argument{"playback controller requires an audio backend"};
    }
    queue_.resize(library_.tracks().size());
    failed_tracks_.resize(queue_.size(), false);
    std::iota(queue_.begin(), queue_.end(), std::size_t{0});
    if (!queue_.empty()) {
        state_ = PlayerState::stopped;
    }
}

void PlayerController::play()
{
    if (queue_.empty() || state_ == PlayerState::playing) {
        return;
    }
    if (state_ == PlayerState::paused && cursor_) {
        try {
            audio_->play();
            state_ = PlayerState::playing;
        } catch (const AudioOutputError&) {
            fail_session();
            throw;
        } catch (const std::runtime_error& error) {
            recover(error.what());
        }
        return;
    }
    // Stopped tracks restart; a completed/failed queue restarts at its beginning.
    const std::size_t position = state_ == PlayerState::stopped ? cursor_.value_or(0) : 0;
    std::fill(failed_tracks_.begin(), failed_tracks_.end(), false);
    start_from(position, false, repeat_ == RepeatMode::all);
}

void PlayerController::play_track(const std::size_t library_index)
{
    if (library_index >= library_.tracks().size()) {
        throw std::out_of_range{"track index is outside the library"};
    }
    const auto found = std::find(queue_.begin(), queue_.end(), library_index);
    failed_tracks_[library_index] = false; // Explicit selection retries this track.
    start_from(static_cast<std::size_t>(found - queue_.begin()), false, repeat_ == RepeatMode::all);
}

void PlayerController::pause()
{
    if (state_ == PlayerState::playing) {
        audio_->pause();
        state_ = PlayerState::paused;
    }
}

void PlayerController::stop()
{
    if (cursor_) {
        audio_->pause();
        state_ = PlayerState::stopped;
        try {
            audio_->stop();
        } catch (const AudioOutputError&) {
            fail_session();
            throw;
        } catch (const std::runtime_error& error) {
            // The stopped selection is retained; play() reloads it from scratch.
            record_error(*current_index(), error.what());
        }
    }
}

void PlayerController::next()
{
    if (!cursor_) {
        play();
        return;
    }
    if (*cursor_ + 1 < queue_.size()) {
        start_from(*cursor_ + 1, false, repeat_ == RepeatMode::all);
    } else if (repeat_ == RepeatMode::all) {
        reshuffle_cycle();
        start_from(0, false, true);
    } else {
        finish();
    }
}

void PlayerController::previous()
{
    if (!cursor_) {
        play();
        return;
    }
    if (*cursor_ > 0) {
        start_from(*cursor_ - 1, true, repeat_ == RepeatMode::all);
    } else if (repeat_ == RepeatMode::all) {
        start_from(queue_.size() - 1, true, true);
    } else {
        start_from(0, false, false); // At the head, restart the first track.
    }
}

void PlayerController::update()
{
    if (state_ != PlayerState::playing) {
        return;
    }
    try {
        audio_->update();
    } catch (const AudioOutputError&) {
        fail_session();
        throw;
    } catch (const std::runtime_error& error) {
        recover(error.what());
        return;
    }
    if (audio_->state() == PlaybackState::finished) {
        if (repeat_ == RepeatMode::one) {
            start_from(*cursor_, false, false);
        } else {
            next();
        }
    }
}

void PlayerController::seek(const AudioPlayer::Milliseconds target)
{
    if (!cursor_) {
        return;
    }
    const bool keep_paused = state_ != PlayerState::playing;
    try {
        audio_->seek(target);
        if (state_ == PlayerState::stopped || state_ == PlayerState::finished) {
            state_ = PlayerState::paused;
        }
    } catch (const AudioOutputError&) {
        fail_session();
        throw;
    } catch (const std::runtime_error& error) {
        recover(error.what(), keep_paused);
    }
}

void PlayerController::set_volume(const float volume) { audio_->set_volume(volume); }

void PlayerController::set_shuffle(const bool enabled)
{
    if (enabled == shuffle_) {
        return;
    }
    const auto current = current_index();
    shuffle_ = enabled;
    if (enabled) {
        const auto offset = cursor_ ? *cursor_ + 1 : 0;
        std::shuffle(queue_.begin() + static_cast<std::ptrdiff_t>(offset), queue_.end(), random_);
    } else {
        std::iota(queue_.begin(), queue_.end(), std::size_t{0});
        // Canonical queue order matches library indices directly.
        cursor_ = current;
    }
}

void PlayerController::set_repeat(const RepeatMode mode) noexcept { repeat_ = mode; }
const Library& PlayerController::library() const noexcept { return library_; }
const std::vector<std::size_t>& PlayerController::queue() const noexcept { return queue_; }

std::optional<std::size_t> PlayerController::current_index() const noexcept
{
    return cursor_ ? std::optional{queue_[*cursor_]} : std::nullopt;
}

const Track* PlayerController::current_track() const noexcept
{
    const auto index = current_index();
    return index ? &library_.tracks()[*index] : nullptr;
}

PlayerState PlayerController::state() const noexcept { return state_; }
bool PlayerController::shuffle() const noexcept { return shuffle_; }
RepeatMode PlayerController::repeat() const noexcept { return repeat_; }

AudioPlayer::Milliseconds PlayerController::position() const noexcept
{
    return cursor_ && state_ != PlayerState::stopped ? audio_->position() : AudioPlayer::Milliseconds{0};
}

std::optional<AudioPlayer::Milliseconds> PlayerController::duration() const noexcept
{
    return cursor_ ? audio_->duration() : std::nullopt;
}

float PlayerController::volume() const noexcept { return audio_->volume(); }

PlaybackErrors PlayerController::take_errors()
{
    return std::exchange(errors_, PlaybackErrors{});
}

void PlayerController::start_from(std::size_t position, const bool backwards, const bool wrap,
                                  const bool autoplay)
{
    audio_->pause(); // A failed load must not leave the previous source audible.
    cursor_.reset();
    state_ = PlayerState::stopped;
    for (std::size_t attempts = 0; attempts < queue_.size(); ++attempts) {
        const std::size_t index = queue_[position];
        if (!failed_tracks_[index]) {
            try {
                audio_->load(library_.tracks()[index].audio_path);
                if (autoplay) {
                    audio_->play();
                }
                cursor_ = position;
                state_ = autoplay ? PlayerState::playing : PlayerState::paused;
                return;
            } catch (const AudioOutputError&) {
                fail_session();
                throw;
            } catch (const std::runtime_error& error) {
                audio_->pause();
                record_error(index, error.what());
            }
        }
        if (backwards) {
            if (position == 0 && !wrap) {
                break;
            }
            position = position == 0 ? queue_.size() - 1 : position - 1;
        } else {
            if (position + 1 == queue_.size() && !wrap) {
                break;
            }
            position = position + 1 == queue_.size() ? 0 : position + 1;
        }
    }
    state_ = PlayerState::failed;
}

void PlayerController::finish()
{
    audio_->pause();
    state_ = PlayerState::finished;
}

void PlayerController::recover(const std::string& message, const bool keep_paused)
{
    record_error(*current_index(), message);
    // Errors never trigger repeat-one. Move on rather than retrying the same
    // broken stream forever; a transition visits each candidate at most once.
    if (*cursor_ + 1 < queue_.size()) {
        start_from(*cursor_ + 1, false, repeat_ == RepeatMode::all, !keep_paused);
    } else if (repeat_ == RepeatMode::all) {
        reshuffle_cycle();
        start_from(0, false, true, !keep_paused);
    } else {
        fail_session();
    }
}

void PlayerController::record_error(const std::size_t index, const std::string& message)
{
    failed_tracks_[index] = true;
    constexpr std::size_t error_limit = 32;
    if (errors_.tracks.size() == error_limit) {
        errors_.tracks.erase(errors_.tracks.begin());
        ++errors_.omitted;
    }
    errors_.tracks.push_back(TrackError{library_.tracks()[index].stable_id, message});
}

void PlayerController::reshuffle_cycle()
{
    if (!shuffle_ || queue_.size() < 2) {
        return;
    }
    const auto last = current_index();
    std::shuffle(queue_.begin(), queue_.end(), random_);
    if (last && queue_.front() == *last) {
        std::swap(queue_.front(), queue_.back());
    }
}

void PlayerController::fail_session()
{
    audio_->pause();
    cursor_.reset();
    state_ = PlayerState::failed;
}

}  // namespace osu_player
