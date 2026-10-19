#include "osu_player/discord_presence.hpp"

#include <json-c/json.h>
#include <sys/socket.h>
#include <sys/eventfd.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <poll.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <utility>

namespace osu_player {
namespace {
using namespace std::chrono_literals;
using Steady = std::chrono::steady_clock;
using Json = std::unique_ptr<json_object, decltype(&json_object_put)>;
constexpr std::uint32_t frame_limit = 64 * 1024;

json_object* property(json_object* object, const char* name)
{
    json_object* value{};
    json_object_object_get_ex(object, name, &value);
    return value;
}

Json parse(const std::string& text)
{
    std::unique_ptr<json_tokener, decltype(&json_tokener_free)> parser{json_tokener_new(), json_tokener_free};
    json_tokener_set_flags(parser.get(), JSON_TOKENER_STRICT | JSON_TOKENER_VALIDATE_UTF8);
    Json value{json_tokener_parse_ex(parser.get(), text.c_str(), static_cast<int>(text.size() + 1)), json_object_put};
    if (text.find('\0') != std::string::npos || json_tokener_get_error(parser.get()) != json_tokener_success ||
        !json_object_is_type(value.get(), json_type_object)) {
        throw std::runtime_error{"invalid Discord frame"};
    }
    return value;
}

bool string_is(json_object* value, const char* expected)
{
    return json_object_is_type(value, json_type_string) && std::string_view{json_object_get_string(value)} == expected;
}

// Bound UTF-8 by bytes (stricter than Discord's character limit), preserving complete
// codepoints and removing controls. Metadata from fixtures/files is not trusted.
std::string display_text(const std::string& text, const std::string& fallback)
{
    std::string result;
    for (std::size_t offset = 0; offset < text.size();) {
        const auto first = static_cast<unsigned char>(text[offset]);
        if (first < 0x20 || first == 0x7f) { ++offset; continue; }
        const std::size_t length = first < 0x80 ? 1 : first >= 0xc2 && first <= 0xdf ? 2 :
            first >= 0xe0 && first <= 0xef ? 3 : first >= 0xf0 && first <= 0xf4 ? 4 : 0;
        bool valid = length != 0 && offset + length <= text.size();
        for (std::size_t index = 1; valid && index < length; ++index) {
            const auto byte = static_cast<unsigned char>(text[offset + index]);
            valid = byte >= 0x80 && byte <= 0xbf;
        }
        if (valid && length >= 3) {
            const auto second = static_cast<unsigned char>(text[offset + 1]);
            valid = !(first == 0xe0 && second < 0xa0) && !(first == 0xed && second >= 0xa0) &&
                    !(first == 0xf0 && second < 0x90) && !(first == 0xf4 && second >= 0x90);
        }
        if (!valid) { ++offset; continue; }
        // Also exclude Unicode C1 controls.
        if (first == 0xc2 && static_cast<unsigned char>(text[offset + 1]) < 0xa0) { offset += length; continue; }
        if (result.size() + length > 128) { break; }
        result.append(text, offset, length);
        offset += length;
    }
    return result.empty() ? fallback : result;
}

struct Descriptor {
    int value{-1};
    ~Descriptor() { if (value >= 0) { close(value); } }
    Descriptor() = default;
    Descriptor(const Descriptor&) = delete;
    Descriptor& operator=(const Descriptor&) = delete;
};

using Cancelled = std::function<bool()>;

void wait_io(const int descriptor, const short events, const Steady::time_point deadline, const Cancelled& cancelled)
{
    while (!cancelled()) {
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Steady::now());
        if (remaining <= 0ms) { throw std::runtime_error{"Discord IPC timeout"}; }
        pollfd item{descriptor, events, 0};
        const int result = poll(&item, 1, static_cast<int>(std::clamp(remaining.count(), std::int64_t{1}, std::int64_t{25})));
        if (result < 0 && errno == EINTR) { continue; }
        if (result < 0) { throw std::runtime_error{"Discord IPC poll failed"}; }
        if (result > 0) {
            if (item.revents & events) { return; }
            if (item.revents & (POLLERR | POLLHUP | POLLNVAL)) { throw std::runtime_error{"Discord disconnected"}; }
        }
    }
    throw std::runtime_error{"Discord configuration changed"};
}

void transfer(const int descriptor, char* data, const std::size_t size, const bool sending,
              const Steady::time_point deadline, const Cancelled& cancelled)
{
    std::size_t offset{};
    while (offset < size) {
        if (cancelled()) { throw std::runtime_error{"Discord configuration changed"}; }
        if (Steady::now() >= deadline) { throw std::runtime_error{"Discord IPC timeout"}; }
        const auto count = sending ? send(descriptor, data + offset, size - offset, MSG_NOSIGNAL) :
                                     recv(descriptor, data + offset, size - offset, 0);
        if (count > 0) { offset += static_cast<std::size_t>(count); continue; }
        if (count < 0 && errno == EINTR) { continue; }
        if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            wait_io(descriptor, sending ? POLLOUT : POLLIN, deadline, cancelled);
            continue;
        }
        throw std::runtime_error{"Discord disconnected"};
    }
}

void send_frame(const int descriptor, const std::uint32_t opcode, const std::string& payload,
                const Steady::time_point deadline, const Cancelled& cancelled)
{
    if (payload.size() > frame_limit) { throw std::runtime_error{"Discord frame too large"}; }
    std::array<char, 8> header{};
    const auto size = static_cast<std::uint32_t>(payload.size());
    for (std::size_t index = 0; index < 4; ++index) {
        header[index] = static_cast<char>((opcode >> (index * 8)) & 0xff);
        header[index + 4] = static_cast<char>((size >> (index * 8)) & 0xff);
    }
    transfer(descriptor, header.data(), header.size(), true, deadline, cancelled);
    std::string data = payload;
    transfer(descriptor, data.data(), data.size(), true, deadline, cancelled);
}

struct Frame { std::uint32_t opcode; std::string payload; };

Frame read_frame(const int descriptor, const Steady::time_point deadline, const Cancelled& cancelled)
{
    std::array<char, 8> header{};
    transfer(descriptor, header.data(), header.size(), false, deadline, cancelled);
    std::uint32_t opcode{};
    std::uint32_t size{};
    for (std::size_t index = 0; index < 4; ++index) {
        opcode |= static_cast<std::uint32_t>(static_cast<unsigned char>(header[index])) << (index * 8);
        size |= static_cast<std::uint32_t>(static_cast<unsigned char>(header[index + 4])) << (index * 8);
    }
    if (size > frame_limit) { throw std::runtime_error{"Discord frame too large"}; }
    Frame frame{opcode, std::string(size, '\0')};
    transfer(descriptor, frame.payload.data(), frame.payload.size(), false, deadline, cancelled);
    return frame;
}

std::string activity_request(const std::optional<PlaybackSnapshot>& playback,
                             const std::chrono::system_clock::time_point time, const std::string& nonce)
{
    Json root{json_object_new_object(), json_object_put};
    json_object_object_add(root.get(), "cmd", json_object_new_string("SET_ACTIVITY"));
    json_object_object_add(root.get(), "nonce", json_object_new_string(nonce.c_str()));
    auto* args = json_object_new_object();
    json_object_object_add(root.get(), "args", args);
    json_object_object_add(args, "pid", json_object_new_int64(getpid()));
    const auto activity = discord_activity_json(playback, time);
    json_object_object_add(args, "activity", json_tokener_parse(activity.c_str()));
    return json_object_to_json_string_ext(root.get(), JSON_C_TO_STRING_PLAIN);
}

bool same_presence(const std::optional<PlaybackSnapshot>& first, const std::optional<PlaybackSnapshot>& second)
{
    if (!first || !second) { return !first && !second; }
    return first->session_id == second->session_id && first->track == second->track &&
           first->playing == second->playing && first->duration == second->duration;
}
}  // namespace

bool valid_discord_application_id(const std::string& value) noexcept
{
    if (value.size() < 17 || value.size() > 20 || value.front() == '0') { return false; }
    std::uint64_t id{};
    const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), id);
    return error == std::errc{} && end == value.data() + value.size() && id != 0;
}

std::vector<std::filesystem::path> discord_socket_paths()
{
    if (const char* override_path = std::getenv("OSU_PLAYER_DISCORD_SOCKET"); override_path && *override_path) {
        const std::filesystem::path path{override_path};
        return path.is_absolute() ? std::vector{path} : std::vector<std::filesystem::path>{};
    }
    std::vector<std::filesystem::path> roots;
    for (const char* name : {"XDG_RUNTIME_DIR", "TMPDIR", "TMP", "TEMP"}) {
        if (const char* value = std::getenv(name); value && *value && std::filesystem::path{value}.is_absolute()) {
            const std::filesystem::path root{value};
            if (std::find(roots.begin(), roots.end(), root) == roots.end()) { roots.push_back(root); }
        }
    }
    if (std::find(roots.begin(), roots.end(), std::filesystem::path{"/tmp"}) == roots.end()) { roots.emplace_back("/tmp"); }
    std::vector<std::filesystem::path> paths;
    for (const auto& root : roots) {
        for (int index = 0; index < 10; ++index) { paths.push_back(root / ("discord-ipc-" + std::to_string(index))); }
    }
    return paths;
}

std::string discord_activity_json(const std::optional<PlaybackSnapshot>& playback,
                                 const std::chrono::system_clock::time_point sampled_at)
{
    if (!playback) { return "null"; }
    const auto& track = playback->track;
    const auto title = display_text(track.title_unicode.empty() ? track.title : track.title_unicode, "Unknown title");
    const auto artist = display_text(track.artist_unicode.empty() ? track.artist : track.artist_unicode, "Unknown artist");
    const auto state = display_text((playback->playing ? "" : "Paused — ") + artist, "Paused");
    Json activity{json_object_new_object(), json_object_put};
    json_object_object_add(activity.get(), "type", json_object_new_int(2)); // Listening
    json_object_object_add(activity.get(), "details", json_object_new_string(title.c_str()));
    json_object_object_add(activity.get(), "state", json_object_new_string(state.c_str()));
    auto* assets = json_object_new_object();
    json_object_object_add(activity.get(), "assets", assets);
    json_object_object_add(assets, "large_image", json_object_new_string("osu_player"));
    json_object_object_add(assets, "large_text", json_object_new_string("osu! music player"));
    using namespace std::chrono_literals;
    if (playback->playing && playback->position >= 0ms && playback->position <= 7 * 24h) {
        const auto position = playback->duration ? std::min(playback->position, *playback->duration) : playback->position;
        const auto now = std::chrono::duration_cast<std::chrono::seconds>(sampled_at.time_since_epoch()).count();
        const auto start = now - std::chrono::duration_cast<std::chrono::seconds>(position).count();
        if (start > 0) {
            auto* timestamps = json_object_new_object();
            json_object_object_add(activity.get(), "timestamps", timestamps);
            json_object_object_add(timestamps, "start", json_object_new_int64(start));
            if (playback->duration && *playback->duration > 0ms && *playback->duration <= 7 * 24h) {
                json_object_object_add(timestamps, "end", json_object_new_int64(start +
                    std::chrono::duration_cast<std::chrono::seconds>(*playback->duration).count()));
            }
        }
    }
    if (track.online_set_id && *track.online_set_id > 0) {
        auto* buttons = json_object_new_array();
        auto* button = json_object_new_object();
        json_object_object_add(activity.get(), "buttons", buttons);
        json_object_array_add(buttons, button);
        json_object_object_add(button, "label", json_object_new_string("Open beatmap"));
        const auto url = "https://osu.ppy.sh/beatmapsets/" + std::to_string(*track.online_set_id);
        json_object_object_add(button, "url", json_object_new_string(url.c_str()));
    }
    return json_object_to_json_string_ext(activity.get(), JSON_C_TO_STRING_PLAIN);
}

struct DiscordPresence::Impl {
    DiscordOptions options;
    mutable std::mutex mutex;
    std::condition_variable condition;
    bool enabled{};
    std::string application_id;
    std::uint64_t generation{};
    std::uint64_t revision{};
    std::optional<PlaybackSnapshot> playback;
    Steady::time_point sampled_at{};
    std::string state{"off"};
    Descriptor wake;
    std::jthread worker; // Last member: joins before shared state is destroyed.

    explicit Impl(DiscordOptions value) : options{std::move(value)}
    {
        if (options.update_interval <= 0ms || options.io_timeout <= 0ms || options.reconnect_min <= 0ms ||
            options.reconnect_max < options.reconnect_min) { throw std::invalid_argument{"invalid Discord worker intervals"}; }
        wake.value = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
        if (wake.value < 0) { throw std::runtime_error{"cannot create Discord wake descriptor"}; }
        worker = std::jthread{[this](const std::stop_token stop) { run(stop); }};
    }

    ~Impl()
    {
        worker.request_stop();
        notify();
        condition.notify_all();
    }

    void notify() noexcept
    {
        const std::uint64_t value = 1;
        // A saturated counter is already readable, so EAGAIN loses no wakeup.
        while (write(wake.value, &value, sizeof(value)) < 0 && errno == EINTR) { }
    }

    void drain_wake() noexcept
    {
        std::uint64_t value{};
        while (read(wake.value, &value, sizeof(value)) < 0 && errno == EINTR) { }
    }

    bool cancelled(const std::stop_token stop, const std::uint64_t expected)
    {
        if (stop.stop_requested()) { return true; }
        const std::lock_guard lock{mutex};
        return generation != expected;
    }

    void status(const std::uint64_t expected, std::string value)
    {
        const std::lock_guard lock{mutex};
        if (generation == expected) { state = std::move(value); }
    }

    void connect_socket(Descriptor& socket, const std::filesystem::path& path, const Cancelled& cancel)
    {
        sockaddr_un address{};
        address.sun_family = AF_UNIX;
        const auto name = path.string();
        struct stat info{};
        if (!path.is_absolute() || name.size() >= sizeof(address.sun_path) || stat(name.c_str(), &info) != 0 ||
            !S_ISSOCK(info.st_mode) || info.st_uid != getuid()) { throw std::runtime_error{"Discord socket unavailable"}; }
        std::memcpy(address.sun_path, name.c_str(), name.size() + 1);
        socket.value = ::socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        if (socket.value < 0) { throw std::runtime_error{"Discord socket unavailable"}; }
        if (connect(socket.value, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) {
            if (errno != EINPROGRESS && errno != EAGAIN) { throw std::runtime_error{"Discord socket unavailable"}; }
            wait_io(socket.value, POLLOUT, Steady::now() + options.io_timeout, cancel);
            int error{};
            socklen_t size = sizeof(error);
            if (getsockopt(socket.value, SOL_SOCKET, SO_ERROR, &error, &size) != 0 || error != 0) {
                throw std::runtime_error{"Discord socket unavailable"};
            }
        }
        ucred peer{};
        socklen_t size = sizeof(peer);
        if (getsockopt(socket.value, SOL_SOCKET, SO_PEERCRED, &peer, &size) != 0 || peer.uid != getuid()) {
            throw std::runtime_error{"Discord socket owner mismatch"};
        }
    }

    // Control frames return a null Json after handling ping/pong. Callers waiting
    // for READY/ACK keep the same absolute deadline across control frames.
    Json receive(const int descriptor, const Steady::time_point deadline, const Cancelled& cancel)
    {
        auto frame = read_frame(descriptor, deadline, cancel);
        if (frame.opcode == 3) {
            send_frame(descriptor, 4, frame.payload, deadline, cancel);
            return Json{nullptr, json_object_put};
        }
        if (frame.opcode == 4) { return Json{nullptr, json_object_put}; }
        if (frame.opcode != 1) { throw std::runtime_error{"Discord closed IPC session"}; }
        auto value = parse(frame.payload);
        if (string_is(property(value.get(), "evt"), "ERROR")) { throw std::runtime_error{"Discord rejected request"}; }
        return value;
    }

    void session(const int descriptor, const std::string& id, const std::uint64_t expected,
                 const std::stop_token stop, bool& healthy)
    {
        const Cancelled cancel = [this, stop, expected] { return cancelled(stop, expected); };
        Json handshake{json_object_new_object(), json_object_put};
        json_object_object_add(handshake.get(), "v", json_object_new_int(1));
        json_object_object_add(handshake.get(), "client_id", json_object_new_string(id.c_str()));
        const auto deadline = Steady::now() + options.io_timeout;
        send_frame(descriptor, 0, json_object_to_json_string_ext(handshake.get(), JSON_C_TO_STRING_PLAIN), deadline, cancel);
        auto ready = receive(descriptor, deadline, cancel);
        while (!ready) { ready = receive(descriptor, deadline, cancel); }
        if (!string_is(property(ready.get(), "evt"), "READY") || !string_is(property(ready.get(), "cmd"), "DISPATCH")) {
            throw std::runtime_error{"invalid Discord handshake"};
        }
        status(expected, "connected");
        std::uint64_t sent_revision = std::numeric_limits<std::uint64_t>::max();
        std::uint64_t nonce_counter{};
        auto next_send = Steady::now();
        try {
            while (!cancel()) {
                std::optional<PlaybackSnapshot> latest;
                Steady::time_point sampled;
                std::uint64_t latest_revision{};
                bool sending{};
                int timeout = -1;
                {
                    const std::lock_guard lock{mutex};
                    latest_revision = revision;
                    if (latest_revision != sent_revision) {
                        const auto now = Steady::now();
                        sending = !playback || now >= next_send;
                        if (sending) {
                            // Copy metadata only when an actual RPC is due, never
                            // on progress updates or idle socket notifications.
                            latest = playback;
                            sampled = sampled_at;
                        } else {
                            const auto remaining = std::chrono::ceil<std::chrono::milliseconds>(next_send - now);
                            timeout = static_cast<int>(std::min(remaining.count(),
                                static_cast<std::chrono::milliseconds::rep>(std::numeric_limits<int>::max())));
                        }
                    }
                }
                // Clear immediately on stop, rather than leaking stale listening
                // activity for the normal update throttle interval.
                if (sending) {
                    if (latest && latest->playing && Steady::now() >= sampled) {
                        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(Steady::now() - sampled);
                        // Positions from files are bounded before arithmetic/rendering.
                        if (latest->position >= 0ms && latest->position <= 7 * 24h && elapsed <= 7 * 24h) {
                            latest->position += elapsed;
                            if (latest->duration) { latest->position = std::min(latest->position, *latest->duration); }
                        }
                    }
                    const auto nonce = "osu-player-" + std::to_string(++nonce_counter);
                    const auto ack_deadline = Steady::now() + options.io_timeout;
                    send_frame(descriptor, 1, activity_request(latest, std::chrono::system_clock::now(), nonce), ack_deadline, cancel);
                    while (true) {
                        const auto response = receive(descriptor, ack_deadline, cancel);
                        if (string_is(property(response.get(), "nonce"), nonce.c_str()) &&
                            string_is(property(response.get(), "cmd"), "SET_ACTIVITY")) { break; }
                    }
                    sent_revision = latest_revision;
                    healthy = true;
                    next_send = Steady::now() + options.update_interval;
                    // State may have changed while awaiting the ACK. Recheck it
                    // before deciding whether to block or wait for the throttle.
                    continue;
                }
                // No idle timer: wake only for IPC, configuration/shutdown, a
                // presence change, or the pending update's throttle deadline.
                std::array<pollfd, 2> items{{{descriptor, POLLIN, 0}, {wake.value, POLLIN, 0}}};
                const int result = poll(items.data(), items.size(), timeout);
                if (result < 0 && errno == EINTR) { continue; }
                if (result < 0 || (items[0].revents & (POLLERR | POLLHUP | POLLNVAL))) {
                    throw std::runtime_error{"Discord disconnected"};
                }
                if (items[1].revents & POLLIN) { drain_wake(); }
                if (items[0].revents & POLLIN) { (void)receive(descriptor, Steady::now() + options.io_timeout, cancel); }
            }
        } catch (...) {
            clear(descriptor);
            throw;
        }
        clear(descriptor);
    }

    void clear(const int descriptor) noexcept
    {
        try {
            send_frame(descriptor, 1, activity_request(std::nullopt, std::chrono::system_clock::now(), "osu-player-clear"),
                       Steady::now() + 100ms, [] { return false; });
        } catch (...) { } // Closing the socket is the fallback; no diagnostics over ncurses.
    }

    void run(const std::stop_token stop) noexcept
    {
        try {
            auto delay = options.reconnect_min;
            std::uint64_t previous_generation = std::numeric_limits<std::uint64_t>::max();
            while (!stop.stop_requested()) {
                std::string id;
                std::uint64_t expected{};
                {
                    std::unique_lock lock{mutex};
                    condition.wait(lock, [this, stop] {
                        return stop.stop_requested() || (enabled && valid_discord_application_id(application_id));
                    });
                    if (stop.stop_requested()) { return; }
                    id = application_id;
                    expected = generation;
                    state = "connecting";
                }
                if (expected != previous_generation) { delay = options.reconnect_min; previous_generation = expected; }
                const auto paths = options.socket_paths.empty() ? discord_socket_paths() : options.socket_paths;
                bool reached_socket{};
                bool healthy{};
                std::string failure{"waiting for Discord"};
                for (const auto& path : paths) {
                    if (cancelled(stop, expected)) { break; }
                    Descriptor descriptor;
                    try {
                        connect_socket(descriptor, path, [this, stop, expected] { return cancelled(stop, expected); });
                        reached_socket = true;
                        session(descriptor.value, id, expected, stop, healthy);
                    } catch (const std::exception& error) {
                        if (reached_socket) { failure = error.what(); break; }
                    }
                }
                if (cancelled(stop, expected)) { continue; }
                status(expected, failure);
                if (healthy) { delay = options.reconnect_min; }
                std::unique_lock lock{mutex};
                condition.wait_for(lock, delay, [this, stop, expected] { return stop.stop_requested() || generation != expected; });
                delay = std::min(delay * 2, options.reconnect_max);
            }
        } catch (...) {
            const std::lock_guard lock{mutex};
            state = "Discord worker unavailable";
        }
    }
};

DiscordPresence::DiscordPresence(DiscordOptions options) : impl_{std::make_unique<Impl>(std::move(options))} { }
DiscordPresence::~DiscordPresence() = default;

void DiscordPresence::configure(const bool enabled, std::string application_id)
{
    const std::lock_guard lock{impl_->mutex};
    if (impl_->enabled == enabled && impl_->application_id == application_id) { return; }
    impl_->enabled = enabled;
    impl_->application_id = std::move(application_id);
    ++impl_->generation;
    impl_->state = !enabled ? "off" : impl_->application_id.empty() ? "needs app ID" :
        !valid_discord_application_id(impl_->application_id) ? "invalid app ID" : "connecting";
    impl_->condition.notify_all();
    impl_->notify();
}

void DiscordPresence::publish(const PlaybackEvents& events, const std::optional<PlaybackSnapshot>& playback)
{
    const bool force = events.omitted != 0 || std::any_of(events.events.begin(), events.events.end(), [](const auto& event) {
        return event.type == PlaybackEventType::seeked;
    });
    const std::lock_guard lock{impl_->mutex};
    const bool changed = force || !same_presence(impl_->playback, playback);
    if (changed) {
        ++impl_->revision;
        impl_->playback = playback;
        if (impl_->enabled) { impl_->notify(); }
    } else if (playback) {
        // Only position is needed to anchor a future send. Do not repeatedly
        // copy owned metadata strings or unrelated listening-accounting state.
        impl_->playback->position = playback->position;
    }
    impl_->sampled_at = Steady::now();
}

std::string DiscordPresence::status() const
{
    const std::lock_guard lock{impl_->mutex};
    return impl_->state;
}
}  // namespace osu_player
