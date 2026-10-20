#pragma once

#include "osu_player/audio_probe.hpp"

#include <condition_variable>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>

namespace osu_player {

// A single latest-request worker. Replacing/closing a panel cancels the old
// request without joining on the UI thread. Generations prevent stale delivery.
class TrackDetailsProbe {
public:
    struct Result {
        std::uint64_t generation{};
        std::optional<std::int64_t> duration;
        bool failed{};
    };
    TrackDetailsProbe() : worker_{[this](std::stop_token stop) { run(stop); }} { }
    ~TrackDetailsProbe()
    {
        worker_.request_stop();
        cancel();
        condition_.notify_all();
    }
    void request(const std::uint64_t generation, std::filesystem::path path)
    {
        std::lock_guard lock{mutex_};
        cancellation_.request_stop();
        cancellation_ = {};
        pending_ = Request{generation, std::move(path), cancellation_.get_token()};
        result_.reset();
        condition_.notify_one();
    }
    void cancel()
    {
        std::lock_guard lock{mutex_};
        cancellation_.request_stop();
        pending_.reset(); result_.reset();
    }
    std::optional<Result> poll()
    {
        std::lock_guard lock{mutex_};
        return std::exchange(result_, std::nullopt);
    }
private:
    struct Request { std::uint64_t generation; std::filesystem::path path; std::stop_token stop; };
    void run(const std::stop_token stop)
    {
        while (!stop.stop_requested()) {
            std::optional<Request> request;
            {
                std::unique_lock lock{mutex_};
                condition_.wait(lock, [&] { return stop.stop_requested() || pending_.has_value(); });
                if (stop.stop_requested()) { return; }
                request = std::exchange(pending_, std::nullopt);
            }
            Result result{request->generation, {}, false};
            try {
                const auto metadata = probe_audio(request->path, request->stop);
                if (metadata.duration_ms > 0) { result.duration = metadata.duration_ms; }
            } catch (const std::exception&) { result.failed = true; }
            std::lock_guard lock{mutex_};
            if (!request->stop.stop_requested() && !stop.stop_requested()) { result_ = result; }
        }
    }
    std::mutex mutex_;
    std::condition_variable condition_;
    std::stop_source cancellation_;
    std::optional<Request> pending_;
    std::optional<Result> result_;
    std::jthread worker_; // Joined before the synchronization state is destroyed.
};
} // namespace osu_player
