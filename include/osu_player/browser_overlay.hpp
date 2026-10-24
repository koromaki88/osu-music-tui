#pragma once

#include "osu_player/playback_events.hpp"

#include <memory>
#include <filesystem>
#include <optional>
#include <string>

namespace osu_player {

// Optional loopback-only OBS source. UI calls never perform socket or file I/O.
// Latest-value mailbox: no accumulated event history or unbounded client queues.
class BrowserOverlay {
public:
    explicit BrowserOverlay(std::filesystem::path config_path);
    ~BrowserOverlay();
    BrowserOverlay(const BrowserOverlay&) = delete;
    BrowserOverlay& operator=(const BrowserOverlay&) = delete;
    void configure(bool enabled);
    void publish(const std::optional<PlaybackSnapshot>& snapshot);
    [[nodiscard]] std::string status() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace osu_player
