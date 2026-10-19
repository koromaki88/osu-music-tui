#include "osu_player/lastfm_presence.hpp"
#include "osu_player/lastfm_queue.hpp"

#include <json-c/json.h>

#include <atomic>
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>

namespace osu_player {
namespace {
using Json = std::unique_ptr<json_object, decltype(&json_object_put)>;
using Steady = std::chrono::steady_clock;
std::int64_t wall_ms()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}
json_object* field(json_object* root, const char* name)
{
    json_object* result{};
    json_object_object_get_ex(root, name, &result);
    return result;
}
std::string text(json_object* root, const char* name, const int diagnostic)
{
    auto* value = field(root, name);
    if (!json_object_is_type(value, json_type_string)) { throw LastfmFailure{LastfmFailureKind::response_format, diagnostic}; }
    return {json_object_get_string(value), static_cast<std::size_t>(json_object_get_string_len(value))};
}
std::string numeric_text(json_object* root, const char* name)
{
    auto* value = field(root, name);
    if (json_object_is_type(value, json_type_int) && json_object_get_int64(value) >= 0) {
        return std::to_string(json_object_get_int64(value));
    }
    return text(root, name, 8);
}
struct ApiError { int code; };

Json response_json(const LastfmResponse& response)
{
    if (response.body.size() > 128 * 1024 || response.body.find('\0') != std::string::npos) {
        throw LastfmFailure{LastfmFailureKind::response_format, 1};
    }
    std::unique_ptr<json_tokener, decltype(&json_tokener_free)> parser{json_tokener_new(), json_tokener_free};
    json_tokener_set_flags(parser.get(), JSON_TOKENER_STRICT | JSON_TOKENER_VALIDATE_UTF8);
    Json root{json_tokener_parse_ex(parser.get(), response.body.c_str(), static_cast<int>(response.body.size() + 1)), json_object_put};
    if (json_tokener_get_error(parser.get()) != json_tokener_success) {
        if (response.http_status != 200) { throw LastfmFailure{LastfmFailureKind::http_status, static_cast<int>(response.http_status)}; }
        throw LastfmFailure{LastfmFailureKind::response_format, 1};
    }
    if (!json_object_is_type(root.get(), json_type_object)) { throw LastfmFailure{LastfmFailureKind::response_format, 2}; }
    if (auto* error = field(root.get(), "error")) {
        if (!json_object_is_type(error, json_type_int)) { throw LastfmFailure{LastfmFailureKind::response_format}; }
        throw ApiError{json_object_get_int(error)};
    }
    if (response.http_status != 200) { throw LastfmFailure{LastfmFailureKind::http_status, static_cast<int>(response.http_status)}; }
    return root;
}
}  // namespace

struct LastfmPresence::Impl {
    enum class Action { none, load, credentials, authorize, complete, disconnect };
    std::filesystem::path path;
    LastfmTransport transport;
    LastfmTiming timing;
    mutable std::mutex mutex;
    std::condition_variable condition;
    std::atomic<std::uint64_t> generation{};
    std::atomic<std::uint64_t> playback_revision{};
    bool enabled{};
    Action action{Action::none};
    bool disconnect_pending{}; // Irreversible local action; never replace with a later enable.
    LastfmCredentials pending_credentials;
    std::optional<PlaybackSnapshot> playback;
    LastfmStatus state;
    std::string bound_account;
    std::deque<LastfmScrobble> incoming;
    std::size_t pending_count{};
    std::size_t dropped_count{};
    std::string scrobble_message;
    // Worker-only state. Secrets never appear in status/errors/ordinary settings.
    LastfmCredentials credentials;
    std::string token;
    Steady::time_point token_deadline{};
    std::string attempted_session;
    std::unique_ptr<LastfmQueue> queue;
    bool queue_fault{};
    Steady::time_point next_request{};
    std::jthread worker; // Joins before shared members are destroyed.

    Impl(std::filesystem::path value, LastfmTransport sender, LastfmTiming delays)
        : path{std::move(value)}, transport{std::move(sender)}, timing{delays}
    {
        if (timing.request_interval < std::chrono::milliseconds{1} || timing.retry_base < std::chrono::milliseconds{1} ||
            timing.retry_base > std::chrono::minutes{30}) { throw std::invalid_argument{"invalid Last.fm timing"}; }
        worker = std::jthread{[this](const std::stop_token stop) { run(stop); }};
    }
    ~Impl()
    {
        const std::lock_guard lock{mutex};
        worker.request_stop(); ++generation; condition.notify_all();
    }

    void set_state(const std::uint64_t expected, const LastfmPhase phase, std::string message, std::string url = {})
    {
        const std::lock_guard lock{mutex};
        if (generation == expected) {
            state = {phase, std::move(message), std::move(url)};
            bound_account = phase == LastfmPhase::connected ? lastfm_account(credentials.username) : std::string{};
        }
    }

    void queue_status(std::string message)
    {
        const std::lock_guard lock{mutex};
        pending_count = queue ? queue->size(lastfm_account(credentials.username)) : 0;
        if (!message.empty()) { scrobble_message = std::move(message); }
    }

    void open_queue()
    {
        queue_fault = false;
        try {
            // Re-read after any prior failed atomic write: rename may have
            // succeeded even if directory fsync failed afterward.
            queue.reset();
            queue = std::make_unique<LastfmQueue>(path.parent_path() / "scrobbles.json");
            queue_status({});
        } catch (const LastfmFailure& failure) {
            queue_fault = true; queue_status(lastfm_failure_message(failure));
        }
    }

    void persist_incoming()
    {
        std::deque<LastfmScrobble> batch;
        { const std::lock_guard lock{mutex}; batch.swap(incoming); }
        for (const auto& entry : batch) {
            if (!queue || queue_fault) {
                // Preserve the original queue diagnostic (for example lock held
                // by another process), rather than replacing it with generic I/O.
                const std::lock_guard lock{mutex}; ++dropped_count; continue;
            }
            try {
                queue->enqueue(entry);
                queue_status({});
            } catch (const LastfmFailure& failure) {
                // Never send unsaved entries or overwrite an unparseable queue.
                if (failure.code != 4) { queue_fault = true; }
                { const std::lock_guard lock{mutex}; ++dropped_count; }
                queue_status(lastfm_failure_message(failure));
            }
        }
    }

    Json request(LastfmParameters parameters, const LastfmCancelled& cancelled)
    {
        {
            std::unique_lock lock{mutex};
            condition.wait_until(lock, next_request, cancelled);
        }
        if (cancelled()) { throw std::runtime_error{"Last.fm request cancelled"}; }
        next_request = Steady::now() + timing.request_interval;
        auto response = transport(lastfm_signed(std::move(parameters), credentials), cancelled);
        if (cancelled()) { throw std::runtime_error{"Last.fm request cancelled"}; }
        return response_json(response);
    }

    void begin(const std::uint64_t expected, const LastfmCancelled& cancelled)
    {
        if (!valid_lastfm_key(credentials.api_key)) {
            set_state(expected, LastfmPhase::needs_credentials, "needs API credentials"); return;
        }
        if (!credentials.session_key.empty()) { set_state(expected, LastfmPhase::connected, "connected"); return; }
        token.clear();
        const auto result = request({{"method", "auth.getToken"}}, cancelled);
        auto received = text(result.get(), "token", 3);
        if (!valid_lastfm_token(received)) { throw LastfmFailure{LastfmFailureKind::response_format, 4}; }
        token = std::move(received);
        token_deadline = Steady::now() + std::chrono::minutes{60};
        set_state(expected, LastfmPhase::awaiting_authorization, "authorize in browser, then Enter",
                  "https://www.last.fm/api/auth/?api_key=" + credentials.api_key + "&token=" + token);
    }

    void control(const Action selected, LastfmCredentials replacement, const std::uint64_t expected,
                 const LastfmCancelled& cancelled)
    {
        if (selected == Action::load) {
            token.clear(); attempted_session.clear();
            credentials = load_lastfm_credentials(path);
            if (!credentials.api_key.empty()) { open_queue(); }
            set_state(expected, credentials.api_key.empty() ? LastfmPhase::needs_credentials :
                      credentials.session_key.empty() ? LastfmPhase::needs_authorization : LastfmPhase::connected,
                      credentials.api_key.empty() ? "needs API credentials" :
                      credentials.session_key.empty() ? "ready to authorize" : "connected");
        } else if (selected == Action::credentials) {
            save_lastfm_credentials(path, replacement);
            credentials = std::move(replacement); attempted_session.clear(); token.clear();
            open_queue();
            if (!cancelled()) { begin(expected, cancelled); }
        } else if (selected == Action::disconnect) {
            token.clear(); attempted_session.clear();
            credentials = load_lastfm_credentials(path);
            if (!credentials.api_key.empty()) {
                credentials.session_key.clear(); credentials.username.clear();
                save_lastfm_credentials(path, credentials);
            }
            set_state(expected, LastfmPhase::off, "off (disconnected locally)");
        } else if (selected == Action::authorize) {
            begin(expected, cancelled);
        } else if (selected == Action::complete) {
            if (token.empty() || Steady::now() >= token_deadline) {
                token.clear(); set_state(expected, LastfmPhase::needs_authorization, "authorization expired; Enter to restart"); return;
            }
            const auto result = request({{"method", "auth.getSession"}, {"token", token}}, cancelled);
            auto updated = credentials;
            updated.session_key = text(field(result.get(), "session"), "key", 5);
            updated.username = text(field(result.get(), "session"), "name", 6);
            // The single worker orders writes against later disconnect/reconfigure
            // commands. Never hold the UI mailbox mutex across disk I/O.
            if (cancelled()) { return; }
            save_lastfm_credentials(path, updated);
            credentials = std::move(updated); token.clear(); attempted_session.clear();
            queue_status({});
            set_state(expected, LastfmPhase::connected, "connected");
        }
    }

    void defer(const LastfmScrobble& entry, const std::string& message)
    {
        const auto exponent = std::min(entry.failures, 6U);
        const auto delay = std::min(timing.retry_base * (1U << exponent), std::chrono::milliseconds{std::chrono::minutes{30}});
        queue->retry(entry, wall_ms() + delay.count());
        queue_status(message);
    }

    void scrobble(const LastfmScrobble& entry, const LastfmCancelled& cancelled)
    {
        auto parameters = entry.parameters; parameters["sk"] = credentials.session_key;
        try {
            const auto result = request(std::move(parameters), cancelled);
            auto* scrobbles = field(result.get(), "scrobbles");
            auto* item = field(scrobbles, "scrobble");
            if (json_object_is_type(item, json_type_array) && json_object_array_length(item) == 1) { item = json_object_array_get_idx(item, 0); }
            const auto code = numeric_text(field(item, "ignoredMessage"), "code");
            const auto accepted = numeric_text(field(scrobbles, "@attr"), "accepted");
            const auto ignored = numeric_text(field(scrobbles, "@attr"), "ignored");
            if (code.empty() || code.size() > 3 || !std::all_of(code.begin(), code.end(), [](char c) { return c >= '0' && c <= '9'; }) ||
                numeric_text(item, "timestamp") != entry.parameters.at("timestamp") ||
                (code == "0" ? accepted != "1" || ignored != "0" : accepted != "0" || ignored != "1")) {
                throw LastfmFailure{LastfmFailureKind::response_format, 8};
            }
            queue->finish(entry);
            queue_status(code == "0" ? "Scrobble accepted" : "Scrobble ignored (code " + code + ")");
        } catch (const ApiError& error) {
            if (error.code == 11 || error.code == 16 || error.code == 29) {
                defer(entry, "Scrobble cached; retry after Last.fm error " + std::to_string(error.code));
            } else if (error.code == 9 || error.code == 10 || error.code == 13 || error.code == 26 || error.code == 4) {
                throw; // Preserve account-linked entry while credentials are repaired.
            } else {
                queue->finish(entry);
                queue_status("Scrobble rejected (Last.fm error " + std::to_string(error.code) + "); not retried");
            }
        } catch (const LastfmFailure& failure) {
            if (failure.kind == LastfmFailureKind::queue) { throw; }
            if (failure.kind == LastfmFailureKind::signing || failure.kind == LastfmFailureKind::https_setup) { throw; }
            // An HTTP code alone is not a conclusive API rejection. Preserve
            // unparseable proxy/server responses rather than deleting user data.
            defer(entry, "Scrobble cached; " + lastfm_failure_message(failure));
        } catch (const std::exception&) {
            // Includes cancellation/ambiguous network failures: preserve original
            // timestamp and back off, never interpret an exception as acceptance.
            defer(entry, "Scrobble cached; request interrupted, retry scheduled");
        }
    }

    void now_playing(const PlaybackSnapshot& latest, const std::uint64_t expected, const LastfmCancelled& cancelled)
    {
        auto parameters = lastfm_now_playing(latest);
        if (!parameters) { set_state(expected, LastfmPhase::connected, "connected (invalid metadata; not sent)"); return; }
        (*parameters)["sk"] = credentials.session_key;
        const auto result = request(std::move(*parameters), cancelled);
        auto* now = field(result.get(), "nowplaying");
        auto* ignored = field(now, "ignoredMessage");
        const auto code = text(ignored, "code", 7);
        if (code.size() != 1 || code[0] < '0' || code[0] > '9') { throw LastfmFailure{LastfmFailureKind::response_format}; }
        // Never use returned corrections as future submission metadata.
        set_state(expected, LastfmPhase::connected, code == "0" ? "connected (Now Playing sent)" : "connected (Now Playing ignored)");
    }

    void run(const std::stop_token stop) noexcept
    {
        try {
            while (true) {
                Action selected{};
                LastfmCredentials replacement;
                std::optional<PlaybackSnapshot> latest;
                std::optional<LastfmScrobble> submission;
                std::uint64_t expected{};
                std::uint64_t expected_playback{};
                {
                    std::unique_lock lock{mutex};
                    while (true) {
                        if (stop.stop_requested() || disconnect_pending || action != Action::none || !incoming.empty()) { break; }
                        const bool connected = enabled && state.phase == LastfmPhase::connected;
                        if (connected && playback && playback->playing && attempted_session != playback->session_id) { break; }
                        auto front = connected && queue && !queue_fault ? queue->front(bound_account) : std::nullopt;
                        if (front && front->retry_at_ms <= wall_ms() && Steady::now() >= next_request) { submission = std::move(front); break; }
                        if (front) {
                            const auto delay = std::chrono::milliseconds{std::max<std::int64_t>(0, front->retry_at_ms - wall_ms())};
                            condition.wait_until(lock, std::max(next_request, Steady::now() + delay));
                        } else { condition.wait(lock); }
                    }
                    if (stop.stop_requested()) {
                        const bool disconnect = std::exchange(disconnect_pending, false);
                        const auto final_generation = generation.load();
                        lock.unlock();
                        persist_incoming();
                        if (disconnect) { control(Action::disconnect, {}, final_generation, [] { return true; }); }
                        return;
                    }
                    expected = generation; expected_playback = playback_revision;
                    selected = std::exchange(disconnect_pending, false) ? Action::disconnect : std::exchange(action, Action::none);
                    if (selected == Action::credentials) { replacement = std::move(pending_credentials); }
                    if (selected == Action::none && !submission && incoming.empty() && playback) {
                        latest = playback;
                        // No retries for failed/ignored Now Playing, including after seek,
                        // resume or journal overflow. This is not the scrobble queue.
                        attempted_session = playback->session_id;
                    }
                }
                const bool playback_request = latest.has_value();
                const LastfmCancelled cancelled = [this, stop, expected, expected_playback, playback_request] {
                    return stop.stop_requested() || generation != expected ||
                        (playback_request && playback_revision != expected_playback);
                };
                try {
                    persist_incoming();
                    if (cancelled() && selected != Action::disconnect) { continue; }
                    if (selected != Action::none) { control(selected, std::move(replacement), expected, cancelled); }
                    else if (submission) {
                        const LastfmCancelled scrobble_cancelled = [this, stop, expected] { return stop.stop_requested() || generation != expected; };
                        scrobble(*submission, scrobble_cancelled);
                    }
                    else if (latest) { now_playing(*latest, expected, cancelled); }
                } catch (const ApiError& error) {
                    if (cancelled()) { continue; }
                    if (error.code == 9) {
                        credentials.session_key.clear(); credentials.username.clear();
                        try {
                            save_lastfm_credentials(path, credentials);
                            set_state(expected, LastfmPhase::needs_authorization, "session revoked; Enter to authorize");
                        } catch (...) { set_state(expected, LastfmPhase::error, "cannot remove revoked session from credential file"); }
                    } else if (selected == Action::complete && error.code == 14) {
                        set_state(expected, LastfmPhase::awaiting_authorization, "not authorized yet; approve in browser, then Enter",
                                  "https://www.last.fm/api/auth/?api_key=" + credentials.api_key + "&token=" + token);
                    } else if (selected == Action::complete && (error.code == 4 || error.code == 15)) {
                        token.clear(); set_state(expected, LastfmPhase::needs_authorization, "authorization expired; Enter to restart");
                    } else if (error.code == 10) {
                        set_state(expected, LastfmPhase::error, "Last.fm error 10: invalid API key; c to replace credentials");
                    } else if (error.code == 13) {
                        set_state(expected, LastfmPhase::error, "Last.fm error 13: invalid signature; check matching key/secret with c");
                    } else if (error.code == 26) {
                        set_state(expected, LastfmPhase::error, "Last.fm error 26: API key suspended; contact Last.fm");
                    } else if (error.code == 4) {
                        set_state(expected, LastfmPhase::error, "Last.fm error 4: authentication denied; check application permissions");
                    } else {
                        set_state(expected, latest ? LastfmPhase::connected : LastfmPhase::error,
                                  latest ? "connected (Now Playing failed; not retried)" :
                                  "Last.fm error " + std::to_string(error.code) + ": API request failed; Enter to retry authorization");
                    }
                } catch (const LastfmFailure& failure) {
                    if (submission && failure.kind == LastfmFailureKind::queue) { queue_fault = true; queue_status(lastfm_failure_message(failure)); }
                    if (!cancelled()) {
                        set_state(expected, latest ? LastfmPhase::connected : LastfmPhase::error,
                                  (latest ? "Now Playing: " : "") + lastfm_failure_message(failure));
                    }
                } catch (const std::exception&) {
                    if (!cancelled()) {
                        // Neither network exceptions, server messages nor local path
                        // errors are surfaced verbatim; they can contain credentials.
                        set_state(expected, latest ? LastfmPhase::connected : LastfmPhase::error,
                                  latest ? "connected (Now Playing failed; not retried)" : "Last.fm request/credential file error");
                    }
                }
            }
        } catch (...) {
            const std::lock_guard lock{mutex}; state = {LastfmPhase::error, "Last.fm worker unavailable", {}};
        }
    }
};

LastfmPresence::LastfmPresence(std::filesystem::path path, LastfmTransport transport, LastfmTiming timing)
    : impl_{std::make_unique<Impl>(std::move(path), std::move(transport), timing)} { }
LastfmPresence::~LastfmPresence() = default;

void LastfmPresence::configure(const bool enabled)
{
    const std::lock_guard lock{impl_->mutex};
    if (impl_->enabled == enabled) { return; }
    impl_->enabled = enabled; ++impl_->generation;
    impl_->bound_account.clear();
    impl_->action = enabled ? Impl::Action::load : Impl::Action::none;
    impl_->pending_credentials = {};
    impl_->state = {enabled ? LastfmPhase::loading : LastfmPhase::off, enabled ? "loading" : "off", {}};
    impl_->condition.notify_all();
}
void LastfmPresence::set_credentials(std::string api_key, std::string secret)
{
    if (!valid_lastfm_key(api_key) || !valid_lastfm_key(secret)) { return; }
    const std::lock_guard lock{impl_->mutex};
    if (!impl_->enabled) { return; }
    ++impl_->generation;
    impl_->bound_account.clear();
    impl_->pending_credentials = {std::move(api_key), std::move(secret), {}, {}};
    impl_->action = Impl::Action::credentials;
    impl_->state = {LastfmPhase::loading, "requesting authorization", {}};
    impl_->condition.notify_all();
}
void LastfmPresence::authorize()
{
    const std::lock_guard lock{impl_->mutex};
    if (!impl_->enabled || impl_->action != Impl::Action::none || impl_->state.phase == LastfmPhase::loading) { return; }
    ++impl_->generation; impl_->action = Impl::Action::authorize;
    impl_->state = {LastfmPhase::loading, "requesting authorization", {}};
    impl_->condition.notify_all();
}
void LastfmPresence::complete_authorization()
{
    const std::lock_guard lock{impl_->mutex};
    if (!impl_->enabled || impl_->state.phase != LastfmPhase::awaiting_authorization || impl_->action != Impl::Action::none) { return; }
    ++impl_->generation; impl_->action = Impl::Action::complete;
    impl_->state.phase = LastfmPhase::loading; impl_->state.message = "checking authorization";
    impl_->condition.notify_all();
}
void LastfmPresence::disconnect()
{
    const std::lock_guard lock{impl_->mutex};
    impl_->enabled = false; ++impl_->generation; impl_->action = Impl::Action::none;
    impl_->disconnect_pending = true;
    impl_->bound_account.clear();
    impl_->pending_credentials = {};
    impl_->state = {LastfmPhase::loading, "disconnecting", {}};
    impl_->condition.notify_all();
}
void LastfmPresence::publish(const PlaybackEvents& events, const std::optional<PlaybackSnapshot>& playback)
{
    const std::lock_guard lock{impl_->mutex};
    if (impl_->enabled && !impl_->bound_account.empty()) {
        // Only explicit surviving eligibility events. Never reconstruct omitted
        // listening history or retroactively scrobble an already-eligible snapshot.
        for (const auto& event : events.events) {
            if (event.type != PlaybackEventType::listening_eligible) { continue; }
            const auto entry = lastfm_scrobble(event.playback, impl_->bound_account);
            if (!entry) { impl_->scrobble_message = "Scrobble skipped: invalid metadata, timestamp or listening eligibility"; continue; }
            if (impl_->incoming.size() >= PlaybackMonitor::event_limit) { ++impl_->dropped_count; }
            else { impl_->incoming.push_back(*entry); }
        }
        if (events.omitted) { impl_->scrobble_message = "Playback events omitted; missing scrobbles not reconstructed"; }
        if (!impl_->incoming.empty()) { impl_->condition.notify_all(); }
    }
    const auto& previous = impl_->playback;
    const bool changed = previous.has_value() != playback.has_value() || (previous && playback &&
        (previous->session_id != playback->session_id || previous->playing != playback->playing ||
         previous->track != playback->track || previous->duration != playback->duration));
    if (changed) {
        impl_->playback = playback; ++impl_->playback_revision;
        impl_->condition.notify_all();
    }
}
LastfmStatus LastfmPresence::status() const
{
    const std::lock_guard lock{impl_->mutex};
    auto result = impl_->state;
    result.pending = impl_->pending_count + impl_->incoming.size(); result.dropped = impl_->dropped_count;
    result.scrobble_status = impl_->scrobble_message;
    return result;
}
}  // namespace osu_player
