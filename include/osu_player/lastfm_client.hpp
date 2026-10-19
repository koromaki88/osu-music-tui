#pragma once

#include "osu_player/playback_events.hpp"

#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>

namespace osu_player {

// Only these fixed categories and numeric codes may cross into UI diagnostics.
// Never surface arbitrary exception text, server bodies, paths, or curl error buffers.
enum class LastfmFailureKind {
    credential_format, credential_permissions, credential_io, signing,
    https_setup, https_transfer, response_format, http_status, queue
};
struct LastfmFailure : std::exception {
    LastfmFailureKind kind;
    int code{};
    explicit LastfmFailure(LastfmFailureKind value, int number = 0) : kind{value}, code{number} { }
};
std::string lastfm_failure_message(const LastfmFailure& failure);

// Format negotiation belongs in the URL as well as the signed-form request.
// Credentials and signatures remain exclusively in the POST body.
inline constexpr const char* lastfm_api_endpoint = "https://ws.audioscrobbler.com/2.0/?format=json";

using LastfmParameters = std::map<std::string, std::string>;
using LastfmCancelled = std::function<bool()>;
struct LastfmResponse { long http_status{}; std::string body; };
using LastfmTransport = std::function<LastfmResponse(const LastfmParameters&, const LastfmCancelled&)>;

struct LastfmCredentials {
    std::string api_key;
    std::string secret;
    std::string username;
    std::string session_key;
    bool operator==(const LastfmCredentials&) const = default;
};

bool valid_lastfm_key(const std::string& value) noexcept;
// Server-issued tokens/session keys are opaque, not application API keys.
bool valid_lastfm_token(const std::string& value) noexcept;
std::filesystem::path lastfm_credentials_path();
// Private, bounded, owner-checked, non-symlink file; malformed files are not replaced.
LastfmCredentials load_lastfm_credentials(const std::filesystem::path& path);
void save_lastfm_credentials(const std::filesystem::path& path, const LastfmCredentials& value);

std::string lastfm_signature(const LastfmParameters& parameters, const std::string& secret);
std::string lastfm_form(const LastfmParameters& parameters);
LastfmParameters lastfm_signed(LastfmParameters parameters, const LastfmCredentials& credentials);
std::optional<LastfmParameters> lastfm_now_playing(const PlaybackSnapshot& playback);
// Fixed HTTPS endpoint, verified TLS, no redirects, bounded body/deadlines. No logging.
LastfmResponse lastfm_https(const LastfmParameters& parameters, const LastfmCancelled& cancelled);

}  // namespace osu_player
