#include "osu_player/playback_events.hpp"

#include <algorithm>
#include <iomanip>
#include <random>
#include <sstream>
#include <utility>

namespace osu_player {
namespace {
PlaybackTime real_time()
{
    return {std::chrono::steady_clock::now(), std::chrono::system_clock::now()};
}

std::string instance_id()
{
    // Independent of shuffle's seeded RNG. Distinguishes controllers/process runs
    // without relying on a mutable wall clock or persisting counters to disk.
    std::random_device random;
    std::ostringstream result;
    result << std::hex << std::setfill('0');
    for (int index = 0; index < 4; ++index) {
        result << std::setw(8) << static_cast<std::uint32_t>(random());
    }
    return result.str();
}

PlaybackMetadata copy_metadata(const Track& track)
{
    return {track.stable_id, track.audio_hash, track.title, track.title_unicode,
            track.artist, track.artist_unicode, track.online_set_id};
}

std::optional<std::chrono::milliseconds> valid_duration(std::optional<std::chrono::milliseconds> duration)
{
    return duration && duration->count() > 0 ? duration : std::nullopt;
}
}  // namespace

PlaybackMonitor::PlaybackMonitor(PlaybackClock clock)
    : clock_{clock ? std::move(clock) : PlaybackClock{real_time}}, instance_id_{instance_id()}
{
}

void PlaybackMonitor::begin(const Track& track, const Milliseconds position, const std::optional<Milliseconds> duration)
{
    // The controller closes the old session before loading a replacement.
    const auto now = clock_();
    current_ = PlaybackSnapshot{instance_id_ + ":" + std::to_string(++sequence_), copy_metadata(track),
                                now.wall, std::max(position, Milliseconds{0}), valid_duration(duration),
                                Milliseconds{0}, true, false};
    last_sample_ = now.monotonic;
    listened_ = {};
    uncredited_elapsed_ = {};
    emit(PlaybackEventType::started, now);
}

void PlaybackMonitor::sample_at(const PlaybackTime now, const Milliseconds position,
                                const std::optional<Milliseconds> duration)
{
    if (!current_) { return; }
    const auto next_position = std::max(position, Milliseconds{0});
    if (current_->playing && now.monotonic >= last_sample_) {
        // SDL reports consumed samples in bursts. Keep elapsed allowance across
        // unchanged position samples instead of losing half the listening time
        // when the polling interval is shorter than an audio-device buffer.
        uncredited_elapsed_ += now.monotonic - last_sample_;
        const auto advanced = std::max(next_position - current_->position, Milliseconds{0});
        if (advanced > Milliseconds{0}) {
            // Compare before converting: malformed media can report positions
            // too large to represent as steady-clock nanoseconds.
            const auto credit = advanced > std::chrono::duration_cast<Milliseconds>(uncredited_elapsed_)
                ? uncredited_elapsed_
                : std::chrono::duration_cast<std::chrono::steady_clock::duration>(advanced);
            listened_ += credit;
            // Carry elapsed allowance across device-buffer jitter, but never
            // carry excess media movement forward to credit future silence.
            uncredited_elapsed_ -= credit;
        } else if (next_position < current_->position) {
            uncredited_elapsed_ = {};
        }
    } else {
        uncredited_elapsed_ = {};
    }
    last_sample_ = now.monotonic;
    current_->position = next_position;
    current_->duration = valid_duration(duration);
    current_->listened = std::chrono::duration_cast<Milliseconds>(listened_);
    eligibility(now);
}

void PlaybackMonitor::sample(const Milliseconds position, const std::optional<Milliseconds> duration)
{
    if (!current_) { return; }
    const auto now = clock_();
    sample_at(now, position, duration);
    emit(PlaybackEventType::progress, now);
}

void PlaybackMonitor::pause(const Milliseconds position, const std::optional<Milliseconds> duration)
{
    if (!current_ || !current_->playing) { return; }
    const auto now = clock_();
    sample_at(now, position, duration);
    current_->playing = false;
    uncredited_elapsed_ = {};
    emit(PlaybackEventType::paused, now);
}

void PlaybackMonitor::resume(const Milliseconds position, const std::optional<Milliseconds> duration)
{
    if (!current_ || current_->playing) { return; }
    const auto now = clock_();
    sample_at(now, position, duration); // Rebase without crediting paused time.
    current_->playing = true;
    uncredited_elapsed_ = {};
    emit(PlaybackEventType::resumed, now);
}

void PlaybackMonitor::seek(const Milliseconds position, const std::optional<Milliseconds> duration)
{
    if (!current_) { return; }
    const auto now = clock_();
    last_sample_ = now.monotonic;
    uncredited_elapsed_ = {};
    current_->position = std::max(position, Milliseconds{0});
    current_->duration = valid_duration(duration);
    eligibility(now);
    emit(PlaybackEventType::seeked, now);
}

void PlaybackMonitor::metadata(const Track& track)
{
    if (!current_) { return; }
    auto metadata = copy_metadata(track);
    if (metadata != current_->track) {
        current_->track = std::move(metadata);
        emit(PlaybackEventType::metadata_changed, clock_());
    }
}

void PlaybackMonitor::end(const PlaybackEndReason reason, const Milliseconds position,
                         const std::optional<Milliseconds> duration)
{
    if (!current_) { return; }
    const auto now = clock_();
    sample_at(now, position, duration);
    current_->playing = false;
    emit(PlaybackEventType::ended, now, reason);
    current_.reset();
}

void PlaybackMonitor::eligibility(const PlaybackTime time)
{
    using namespace std::chrono_literals;
    if (!current_->listening_eligible && current_->duration && *current_->duration > 30s &&
        listened_ >= std::chrono::duration_cast<std::chrono::steady_clock::duration>(
            std::min(*current_->duration, std::chrono::duration_cast<Milliseconds>(8min))) / 2) {
        current_->listening_eligible = true;
        emit(PlaybackEventType::listening_eligible, time);
    }
}

void PlaybackMonitor::emit(const PlaybackEventType type, const PlaybackTime time, const PlaybackEndReason reason)
{
    PlaybackEvent event{type, reason, time, *current_};
    if (type == PlaybackEventType::progress && !events_.empty() &&
        events_.back().type == type && events_.back().playback.session_id == current_->session_id) {
        events_.back() = std::move(event);
        return;
    }
    if (events_.size() == event_limit) {
        events_.pop_front();
        ++omitted_;
    }
    events_.push_back(std::move(event));
}

const std::optional<PlaybackSnapshot>& PlaybackMonitor::snapshot() const noexcept { return current_; }

PlaybackEvents PlaybackMonitor::take_events()
{
    PlaybackEvents result;
    result.events.reserve(events_.size());
    while (!events_.empty()) {
        result.events.push_back(std::move(events_.front()));
        events_.pop_front();
    }
    result.omitted = std::exchange(omitted_, 0);
    return result;
}
}  // namespace osu_player
