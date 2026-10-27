#include "osu_player/player_controller.hpp"

#include <algorithm>
#include <cmath>
#include <numeric>
#include <stdexcept>
#include <utility>
#include <unordered_map>

namespace osu_player {

PlayerController::PlayerController(Library library, std::unique_ptr<AudioPlayer> audio,
                                   const std::uint32_t random_seed, PlaybackClock clock)
    : library_{std::move(library)}, audio_{std::move(audio)}, random_{random_seed}, playback_{std::move(clock)}
{
    if (!audio_) {
        throw std::invalid_argument{"playback controller requires an audio backend"};
    }
    volume_ = audio_->volume();
    queue_.resize(library_.tracks().size());
    failed_tracks_.resize(queue_.size(), false);
    std::iota(queue_.begin(), queue_.end(), std::size_t{0});
    queue_order_ = queue_;
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
            if (playback_.snapshot()) {
                playback_.resume(audio_->position(), audio_->duration());
            } else {
                playback_.begin(*current_track(), audio_->position(), audio_->duration());
            }
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
    if (found == queue_.end()) {
        std::vector<std::size_t> indices(library_.tracks().size());
        std::iota(indices.begin(), indices.end(), std::size_t{0});
        play_queue(std::move(indices), library_index, false);
        return;
    }
    failed_tracks_[library_index] = false; // Explicit selection retries this track.
    start_from(static_cast<std::size_t>(found - queue_.begin()), false, repeat_ == RepeatMode::all);
}

void PlayerController::play_queue(std::vector<std::size_t> indices, const std::size_t selected, const bool restricted)
{
    std::vector<bool> seen(library_.tracks().size(), false);
    for (const auto index : indices) {
        if (index >= seen.size() || seen[index]) { throw std::invalid_argument{"invalid playback queue"}; }
        seen[index] = true;
    }
    if (selected >= seen.size() || !seen[selected]) { throw std::invalid_argument{"selection is outside playback queue"}; }
    auto order = indices;
    std::size_t position = static_cast<std::size_t>(std::find(indices.begin(), indices.end(), selected) - indices.begin());
    if (shuffle_) {
        std::swap(indices.front(), indices[position]);
        std::shuffle(indices.begin() + 1, indices.end(), random_);
        position = 0;
    }
    // End the old session before changing the meaning of its cursor.
    audio_->pause();
    end_playback(PlaybackEndReason::replaced);
    cursor_.reset();
    queue_order_ = std::move(order);
    queue_ = std::move(indices);
    restricted_queue_ = restricted;
    std::fill(failed_tracks_.begin(), failed_tracks_.end(), false);
    start_from(position, false, repeat_ == RepeatMode::all);
}

void PlayerController::pause()
{
    if (state_ == PlayerState::playing) {
        audio_->pause();
        playback_.pause(audio_->position(), audio_->duration());
        state_ = PlayerState::paused;
    }
}

void PlayerController::stop()
{
    if (cursor_) {
        audio_->pause();
        end_playback(PlaybackEndReason::stopped);
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
    end_playback(PlaybackEndReason::skipped);
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
    end_playback(PlaybackEndReason::skipped);
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
    playback_.sample(audio_->position(), audio_->duration());
    if (audio_->state() == PlaybackState::finished) {
        end_playback(PlaybackEndReason::completed);
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
    playback_.sample(audio_->position(), audio_->duration());
    try {
        audio_->seek(target);
        playback_.seek(audio_->position(), audio_->duration());
        if (state_ == PlayerState::stopped || state_ == PlayerState::finished) {
            state_ = PlayerState::paused;
        }
    } catch (const AudioOutputError&) {
        if (const auto& snapshot = playback_.snapshot()) {
            playback_.end(PlaybackEndReason::output_error, snapshot->position, snapshot->duration);
        }
        fail_session();
        throw;
    } catch (const std::runtime_error& error) {
        if (const auto& snapshot = playback_.snapshot()) {
            playback_.end(PlaybackEndReason::track_error, snapshot->position, snapshot->duration);
        }
        recover(error.what(), keep_paused);
    }
}

void PlayerController::shutdown()
{
    audio_->pause();
    end_playback(PlaybackEndReason::shutdown);
    if (state_ == PlayerState::playing) { state_ = PlayerState::paused; }
}

void PlayerController::set_volume(const float volume)
{
    if (!std::isfinite(volume) || volume < 0.0F || volume > 1.0F) { throw std::invalid_argument{"invalid volume"}; }
    audio_->set_volume(muted_ ? 0.0F : volume);
    volume_ = volume;
}
void PlayerController::set_muted(const bool muted)
{
    if (muted == muted_) { return; }
    audio_->set_volume(muted ? 0.0F : volume_);
    muted_ = muted;
}

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
        queue_ = queue_order_;
        if (current) { cursor_ = static_cast<std::size_t>(std::find(queue_.begin(), queue_.end(), *current) - queue_.begin()); }
    }
}

void PlayerController::set_repeat(const RepeatMode mode) noexcept { repeat_ = mode; }

void PlayerController::replace_library(Library library)
{
    const auto identity = [](const Track& track) { return track.stable_id + '\0' + track.audio_hash; };
    std::unordered_map<std::string, std::size_t> indices;
    for (std::size_t index = 0; index < library.tracks().size(); ++index) {
        indices.emplace(identity(library.tracks()[index]), index);
    }
    std::vector<std::size_t> queue;
    std::vector<std::size_t> order;
    if (restricted_queue_) {
        for (const auto index : queue_order_) {
            const auto found = indices.find(identity(library_.tracks()[index]));
            if (found != indices.end()) { order.push_back(found->second); }
        }
    } else {
        order.resize(library.tracks().size());
        std::iota(order.begin(), order.end(), std::size_t{0});
    }
    queue.reserve(library.tracks().size());
    std::vector<bool> used(library.tracks().size(), false);
    std::optional<std::size_t> current;
    if (const auto* track = current_track()) {
        const auto found = indices.find(identity(*track));
        if (found != indices.end()) { current = found->second; }
    }
    if (shuffle_) {
        // Retain the surviving shuffled order, including the played prefix.
        for (const auto index : queue_) {
            const auto found = indices.find(identity(library_.tracks()[index]));
            if (found != indices.end() && !used[found->second]) {
                queue.push_back(found->second);
                used[found->second] = true;
            }
        }
    }
    const auto existing = queue.size();
    for (const auto index : order) {
        if (!used[index]) { queue.push_back(index); }
    }
    if (shuffle_) {
        std::shuffle(queue.begin() + static_cast<std::ptrdiff_t>(existing), queue.end(), random_);
    }
    std::optional<std::size_t> cursor;
    if (current) {
        cursor = static_cast<std::size_t>(std::find(queue.begin(), queue.end(), *current) - queue.begin());
    }
    std::vector<bool> failures(library.tracks().size(), false); // Indexed by library, not subset.
    if (!current) {
        audio_->pause();
        end_playback(PlaybackEndReason::removed);
    }
    library_ = std::move(library);
    queue_ = std::move(queue);
    queue_order_ = std::move(order);
    failed_tracks_ = std::move(failures);
    cursor_ = cursor;
    if (!current) { state_ = queue_.empty() ? PlayerState::empty : PlayerState::stopped; }
    else {
        playback_.sample(audio_->position(), audio_->duration());
        playback_.metadata(*current_track());
    }
}

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

float PlayerController::volume() const noexcept { return volume_; }
bool PlayerController::muted() const noexcept { return muted_; }

PlaybackErrors PlayerController::take_errors()
{
    return std::exchange(errors_, PlaybackErrors{});
}

const std::optional<PlaybackSnapshot>& PlayerController::playback_snapshot() const noexcept
{
    return playback_.snapshot();
}

PlaybackEvents PlayerController::take_playback_events() { return playback_.take_events(); }

void PlayerController::end_playback(const PlaybackEndReason reason)
{
    playback_.end(reason, audio_->position(), audio_->duration());
}

void PlayerController::start_from(std::size_t position, const bool backwards, const bool wrap,
                                  const bool autoplay)
{
    audio_->pause(); // A failed load must not leave the previous source audible.
    end_playback(PlaybackEndReason::replaced);
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
                if (autoplay) {
                    playback_.begin(library_.tracks()[index], audio_->position(), audio_->duration());
                }
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
    end_playback(PlaybackEndReason::track_error);
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
    end_playback(PlaybackEndReason::output_error);
    cursor_.reset();
    state_ = PlayerState::failed;
}

}  // namespace osu_player
