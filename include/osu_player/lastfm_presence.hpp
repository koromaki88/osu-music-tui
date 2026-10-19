#pragma once

#include "osu_player/lastfm_client.hpp"

#include <memory>

namespace osu_player {

enum class LastfmPhase { off, loading, needs_credentials, needs_authorization, awaiting_authorization, connected, error };
struct LastfmStatus {
    LastfmPhase phase{LastfmPhase::off};
    std::string message{"off"};
    // Sensitive, short-lived authorization URL: only render in the setup panel.
    std::string authorization_url;
    std::size_t pending{};
    std::size_t dropped{};
    std::string scrobble_status{};
};

struct LastfmTiming {
    std::chrono::milliseconds request_interval{200};
    std::chrono::milliseconds retry_base{30000};
};

class LastfmPresence {
public:
    explicit LastfmPresence(std::filesystem::path credentials_path,
                            LastfmTransport transport = lastfm_https, LastfmTiming timing = {});
    ~LastfmPresence();
    LastfmPresence(const LastfmPresence&) = delete;
    LastfmPresence& operator=(const LastfmPresence&) = delete;

    void configure(bool enabled);
    void set_credentials(std::string api_key, std::string secret);
    void authorize();
    void complete_authorization();
    // Local disconnect: invalidate in-flight work, remove stored session, retain
    // application credentials. Revoke server-side access on Last.fm's website.
    void disconnect();
    void publish(const PlaybackEvents& events, const std::optional<PlaybackSnapshot>& playback);
    LastfmStatus status() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace osu_player
