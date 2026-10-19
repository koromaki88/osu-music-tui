#pragma once

#include "osu_player/playback_events.hpp"

#include <chrono>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace osu_player {

bool valid_discord_application_id(const std::string& value) noexcept;
std::vector<std::filesystem::path> discord_socket_paths();

// Pure rendering; returns an activity object or JSON null. No local paths/IDs/hashes
// are sent. sampled_at is the wall time corresponding to the supplied position.
std::string discord_activity_json(const std::optional<PlaybackSnapshot>& playback,
                                 std::chrono::system_clock::time_point sampled_at);

struct DiscordOptions {
    // Empty means discover local sockets. Explicit paths isolate fake-server tests.
    std::vector<std::filesystem::path> socket_paths;
    std::chrono::milliseconds update_interval{5000};
    std::chrono::milliseconds io_timeout{1000};
    std::chrono::milliseconds reconnect_min{1000};
    std::chrono::milliseconds reconnect_max{30000};
};

// Owns an IPC worker. Public methods only exchange bounded in-memory state; all
// socket I/O, reconnects and ACK handling happen on the worker. Disabled by default.
class DiscordPresence {
public:
    explicit DiscordPresence(DiscordOptions options = {});
    ~DiscordPresence();
    DiscordPresence(const DiscordPresence&) = delete;
    DiscordPresence& operator=(const DiscordPresence&) = delete;

    void configure(bool enabled, std::string application_id);
    // Drain once per control loop and publish the current authoritative snapshot.
    // This also resynchronizes presence after journal overflow. Seeks force a new
    // timer anchor; ordinary progress updates do not generate RPC traffic.
    void publish(const PlaybackEvents& events, const std::optional<PlaybackSnapshot>& playback);
    [[nodiscard]] std::string status() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace osu_player
