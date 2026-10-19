#include "osu_player/lastfm_presence.hpp"
#include "osu_player/lastfm_queue.hpp"
#include "osu_player/player_controller.hpp"
#include "fake_audio_player.hpp"
#include "../src/lastfm_storage.hpp"

#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {
using namespace osu_player;
using namespace std::chrono_literals;
void check(bool value, const char* message) { if (!value) { throw std::runtime_error{message}; } }
template<class Function> void rejects(Function function, const char* message)
{
    try { function(); } catch (const std::exception&) { return; }
    throw std::runtime_error{message};
}
template<class Function> void until(Function condition)
{
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (!condition()) {
        if (std::chrono::steady_clock::now() >= deadline) { throw std::runtime_error{"timed out waiting for fake scrobble worker"}; }
        std::this_thread::sleep_for(2ms);
    }
}
LastfmCredentials credentials(std::string name = "Test-User") { return {std::string(32, 'a'), std::string(32, 'b'), std::move(name), std::string(32, 'd')}; }
struct Temporary {
    std::filesystem::path root;
    Temporary()
    {
        const auto base = std::filesystem::is_directory("/tmp/opencode") ? std::filesystem::path{"/tmp/opencode"} : std::filesystem::temp_directory_path();
        std::string pattern = (base / "scrobbles-XXXXXX").string();
        std::vector<char> buffer(pattern.begin(), pattern.end()); buffer.push_back('\0');
        const auto result = mkdtemp(buffer.data());
        if (!result) { throw std::runtime_error{"cannot create test fixture"}; }
        root = result;
        save_lastfm_credentials(path(), credentials());
    }
    ~Temporary() { std::error_code error; std::filesystem::remove_all(root, error); }
    std::filesystem::path path() const { return root / "lastfm/credentials.json"; }
    std::filesystem::path queue() const { return root / "lastfm/scrobbles.json"; }
};
PlaybackSnapshot eligible(std::string session = "session:1")
{
    PlaybackSnapshot value;
    value.session_id = std::move(session); value.playing = false;
    value.started_at = std::chrono::system_clock::now() - 120s;
    value.duration = 120s; value.listened = 60s; value.listening_eligible = true;
    value.track.title = "Original 日本 title"; value.track.artist = "Original artist";
    value.track.title_unicode = "Other title"; value.track.artist_unicode = "Other artist";
    value.track.stable_id = "NEVER SEND LIBRARY ID"; value.track.audio_hash = "NEVER SEND HASH";
    return value;
}
PlaybackEvents events(const PlaybackSnapshot& value)
{
    return {{{PlaybackEventType::listening_eligible, PlaybackEndReason::none, {}, value}}, 0};
}
std::string contents(const std::filesystem::path& path)
{
    std::ifstream file{path}; return {std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
}
struct Fake {
    mutable std::mutex mutex;
    std::vector<LastfmParameters> calls;
    std::atomic<int> api_error{};
    std::atomic<int> curl_error{};
    std::atomic<int> ignored{};
    std::atomic<bool> malformed{};
    std::atomic<bool> blocked{};
    std::atomic<bool> cancelled{};
    std::atomic<bool> numeric_ack{};
    std::atomic<bool> array_ack{};
    std::atomic<bool> hold_reply{};
    std::atomic<bool> release_reply{};
    std::string response_override; // Immutable after starting a worker.
    LastfmResponse send(const LastfmParameters& params, const LastfmCancelled& cancel)
    {
        { const std::lock_guard lock{mutex}; calls.push_back(params); }
        check(params.at("api_sig") == lastfm_signature(params, credentials().secret), "scrobble signed with all original fields");
        if (params.at("method") == "track.updateNowPlaying") {
            return {200, "{\"nowplaying\":{\"ignoredMessage\":{\"code\":\"0\"}}}"};
        }
        check(params.at("method") == "track.scrobble", "only scrobble or Now Playing requests expected");
        check(params.size() == 9 && !params.contains("album") && !params.contains("chosenByUser"), "no invented album or local identifiers");
        if (!response_override.empty()) { return {200, response_override}; }
        if (blocked) {
            until([&] { return cancel(); }); cancelled = true;
            throw std::runtime_error{"DO NOT LOG SECRET"};
        }
        if (hold_reply) {
            until([&] { return release_reply || cancel(); });
            if (cancel()) { throw std::runtime_error{"DO NOT LOG SECRET"}; }
        }
        if (curl_error) { throw LastfmFailure{LastfmFailureKind::https_transfer, curl_error}; }
        if (api_error) { return {200, "{\"error\":" + std::to_string(api_error) + ",\"message\":\"DO NOT LOG SECRET\"}"}; }
        if (malformed) { return {200, "{\"scrobbles\":{\"@attr\":{\"accepted\":\"1\",\"ignored\":\"0\"}}}"}; }
        const std::string code = std::to_string(ignored);
        const auto number = [&](const std::string& value) { return numeric_ack ? value : "\"" + value + "\""; };
        return {200, "{\"scrobbles\":{\"@attr\":{\"accepted\":" + number(ignored ? "0" : "1") + ",\"ignored\":" +
            number(ignored ? "1" : "0") + "},\"scrobble\":" + (array_ack ? "[" : "") + "{\"timestamp\":" + number(params.at("timestamp")) +
            ",\"artist\":{\"#text\":\"DO NOT USE CORRECTION\",\"corrected\":\"1\"},\"ignoredMessage\":{\"code\":" + number(code) + ",\"#text\":\"DO NOT LOG SECRET\"}}" + (array_ack ? "]" : "") + "}}"};
    }
    LastfmTransport transport() { return [this](const auto& params, const auto& cancel) { return send(params, cancel); }; }
    std::size_t count() const { const std::lock_guard lock{mutex}; return calls.size(); }
    LastfmParameters call(std::size_t index) const { const std::lock_guard lock{mutex}; return calls.at(index); }
    std::vector<LastfmParameters> scrobbles() const
    {
        const std::lock_guard lock{mutex};
        std::vector<LastfmParameters> result;
        for (const auto& params : calls) { if (params.at("method") == "track.scrobble") { result.push_back(params); } }
        return result;
    }
};
constexpr LastfmTiming fast{1ms, 50ms};
void connected(LastfmPresence& presence) { presence.configure(true); until([&] { return presence.status().phase == LastfmPhase::connected; }); }

void eligibility_tests()
{
    auto value = eligible();
    auto entry = lastfm_scrobble(value, "Test-User");
    check(entry && entry->account == "test-user" && entry->parameters.at("artist") == "Original artist" &&
          entry->parameters.at("track") == "Original 日本 title" && entry->parameters.at("duration") == "120", "eligible paused/ended session uses original structured metadata");
    check(entry->parameters.at("timestamp") == std::to_string(std::chrono::duration_cast<std::chrono::seconds>(value.started_at.time_since_epoch()).count()), "timestamp is original start, not time queued/submitted");
    value.listened = 59999ms; check(!lastfm_scrobble(value, "test-user"), "half duration must actually be listened");
    value.duration = 1000s; value.listened = 240s; check(lastfm_scrobble(value, "test-user").has_value(), "four-minute threshold for long audio");
    value.duration = 30s; check(!lastfm_scrobble(value, "test-user"), "duration strictly over 30 seconds");
    value.duration.reset(); check(!lastfm_scrobble(value, "test-user"), "unknown duration not eligible");
    value = eligible(); value.listening_eligible = false; check(!lastfm_scrobble(value, "test-user"), "monitor eligibility required");
    value = eligible(); value.track.title = "\x1b[2J"; check(!lastfm_scrobble(value, "test-user"), "invalid metadata not scrobbled");
}
void queue_tests()
{
    Temporary temporary;
    const auto first = *lastfm_scrobble(eligible(), "Test-User");
    auto second = first; second.session_id = "session:2";
    {
        LastfmQueue queue{temporary.queue()};
        check(queue.enqueue(first) && !queue.enqueue(first), "pending duplicate session suppressed");
        rejects([&] { LastfmQueue duplicate{temporary.queue()}; }, "exclusive outbox lock prevents duplicate writers");
        check(queue.enqueue(second), "new repeat/play session is distinct");
        auto other = first; other.account = "another-user"; check(queue.enqueue(other), "different account remains associated");
        queue.retry(first, 1234567890123LL);
        check(queue.front("test-user")->failures == 1 && queue.front("test-user")->retry_at_ms == 1234567890123LL, "retry state persisted");
        queue.finish(first);
        check(queue.front("test-user")->session_id == "session:2" && !queue.enqueue(first), "finished receipt suppresses re-enqueue");
    }
    {
        LastfmQueue queue{temporary.queue()};
        check(queue.size("test-user") == 1 && queue.size("another-user") == 1 && !queue.enqueue(first), "account separation and receipts survive restart");
        check(queue.front("test-user") == second, "exact original payload survives restart");
    }
    struct stat info{}; check(stat(temporary.queue().c_str(), &info) == 0 && (info.st_mode & 0777) == 0600, "queue is private 0600");
    const auto content = contents(temporary.queue());
    check(content.find("NEVER SEND") == std::string::npos && content.find("api_key") == std::string::npos &&
          content.find("session_key") == std::string::npos && content.find("secret") == std::string::npos, "outbox excludes secrets and library identifiers");
    chmod(temporary.queue().c_str(), 0644);
    rejects([&] { LastfmQueue queue{temporary.queue()}; }, "exposed queue refused");
    chmod(temporary.queue().c_str(), 0600);
    { std::ofstream file{temporary.queue()}; file << "{\"version\":99}"; }
    rejects([&] { LastfmQueue queue{temporary.queue()}; }, "unsupported queue preserved");
    check(contents(temporary.queue()) == "{\"version\":99}", "invalid queue not overwritten");
    std::filesystem::remove(temporary.queue());
    const auto target = temporary.root / "target"; { std::ofstream file{target}; file << "untouched"; }
    std::filesystem::create_symlink(target, temporary.queue());
    rejects([&] { LastfmQueue queue{temporary.queue()}; }, "symlink queue refused");
    check(contents(target) == "untouched", "symlink target unchanged");
    std::filesystem::remove(temporary.queue());
    check(mkfifo(temporary.queue().c_str(), 0600) == 0, "create synthetic FIFO");
    rejects([&] { LastfmQueue queue{temporary.queue()}; }, "FIFO queue cannot hang");
}
void delivery_tests()
{
    Temporary temporary; Fake fake;
    const auto value = eligible();
    {
        LastfmPresence presence{temporary.path(), fake.transport(), fast};
        presence.publish(events(value), value); // Disabled: never collect.
        connected(presence);
        presence.publish({}, value); // No retrospective snapshot scrobble.
        std::this_thread::sleep_for(20ms); check(fake.count() == 0, "opt-in and no retroactive eligible snapshot");
        presence.publish(events(value), std::nullopt);
        until([&] { return presence.status().scrobble_status == "Scrobble accepted"; });
        check(fake.count() == 1 && presence.status().pending == 0, "eligible session submitted once");
        presence.publish(events(value), std::nullopt);
        std::this_thread::sleep_for(20ms); check(fake.count() == 1, "repeated eligibility does not duplicate");
        auto ignored = eligible("ignored"); fake.ignored = 2; presence.publish(events(ignored), std::nullopt);
        until([&] { return presence.status().scrobble_status == "Scrobble ignored (code 2)"; });
        fake.ignored = 0; fake.api_error = 6; presence.publish(events(eligible("rejected")), std::nullopt);
        until([&] { return presence.status().scrobble_status.find("Last.fm error 6") != std::string::npos; });
        check(presence.status().pending == 0 && fake.count() == 3, "ignored/permanent failures terminal, no retries");
        fake.api_error = 0;
    }
    {
        LastfmPresence presence{temporary.path(), fake.transport(), fast}; connected(presence);
        presence.publish(events(value), std::nullopt);
        std::this_thread::sleep_for(20ms); check(fake.count() == 3, "accepted receipt survives worker restart");
        PlaybackEvents lost; lost.omitted = 128; presence.publish(lost, value);
        std::this_thread::sleep_for(20ms); check(fake.count() == 3, "overflow never invents lost scrobbles");
    }
}
void capacity_tests()
{
    Temporary temporary;
    const auto first = *lastfm_scrobble(eligible(), "test-user");
    {
        std::ofstream file{temporary.queue()};
        file << "{\"version\":1,\"receipts\":[],\"pending\":[";
        for (std::size_t index = 0; index < LastfmQueue::entry_limit; ++index) {
            if (index) { file << ','; }
            file << "{\"account\":\"test-user\",\"session\":\"capacity:" << index
                 << "\",\"failures\":0,\"retry_at_ms\":0,\"method\":\"track.scrobble\",\"artist\":\"artist\",\"track\":\"title\",\"duration\":\"120\",\"timestamp\":\""
                 << first.parameters.at("timestamp") << "\"}";
        }
        file << "]}";
    }
    chmod(temporary.queue().c_str(), 0600);
    LastfmQueue queue{temporary.queue()};
    check(queue.size("test-user") == LastfmQueue::entry_limit, "bounded full queue can be restored");
    rejects([&] { queue.enqueue(first); }, "full outbox refuses new entry without deleting oldest");
    check(queue.size("test-user") == LastfmQueue::entry_limit, "full outbox preserves all previous scrobbles");
    queue.finish(*queue.front("test-user"));
    check(queue.enqueue(first), "delivery frees capacity for new entries");
}
void retry_restart_tests()
{
    Temporary temporary; Fake offline; offline.api_error = 16;
    const auto value = eligible("offline");
    {
        LastfmPresence presence{temporary.path(), offline.transport(), LastfmTiming{1ms, 100ms}}; connected(presence);
        presence.publish(events(value), std::nullopt);
        until([&] { return presence.status().scrobble_status.find("retry after Last.fm error 16") != std::string::npos; });
        check(presence.status().pending == 1 && offline.count() == 1, "transient error cached with backoff, no busy retry");
        presence.publish(events(eligible("newer")), std::nullopt);
        until([&] { return presence.status().pending == 2; });
        presence.configure(false);
    }
    Fake online;
    {
        LastfmPresence presence{temporary.path(), online.transport(), fast}; connected(presence);
        until([&] { return online.count() == 2 && presence.status().pending == 0; });
        check(online.call(0).at("timestamp") == offline.call(0).at("timestamp") &&
              online.call(0).at("track") == offline.call(0).at("track"), "retries retain original timestamp/metadata");
        check(online.call(1).at("timestamp") >= online.call(0).at("timestamp"), "old pending entry delivered before new one");
    }
    // Ambiguous response and transport failure must remain pending, not accepted.
    for (int kind = 0; kind < 2; ++kind) {
        Temporary fixture; Fake fake; fake.malformed = kind == 0; fake.curl_error = kind == 1 ? 28 : 0;
        {
            LastfmPresence presence{fixture.path(), fake.transport(), LastfmTiming{1ms, 100ms}}; connected(presence);
            presence.publish(events(eligible()), std::nullopt);
            until([&] { return presence.status().scrobble_status.starts_with("Scrobble cached;"); });
            check(presence.status().pending == 1, "missing ACK or timeout cannot consume entry");
            presence.configure(false);
        }
        LastfmQueue queue{fixture.queue()}; check(queue.size("test-user") == 1, "failed request durable across shutdown");
    }
}
void account_and_revocation_tests()
{
    Temporary temporary; const auto entry = *lastfm_scrobble(eligible("old-account"), "test-user");
    { LastfmQueue queue{temporary.queue()}; queue.enqueue(entry); }
    save_lastfm_credentials(temporary.path(), credentials("another-user"));
    Fake other;
    {
        LastfmPresence presence{temporary.path(), other.transport(), fast}; connected(presence);
        std::this_thread::sleep_for(30ms); check(other.count() == 0 && presence.status().pending == 0, "never send old-account backlog to new account");
        presence.publish(events(eligible("other-account")), std::nullopt);
        until([&] { return presence.status().scrobble_status == "Scrobble accepted"; });
    }
    save_lastfm_credentials(temporary.path(), credentials());
    Fake revoked; revoked.api_error = 9;
    {
        LastfmPresence presence{temporary.path(), revoked.transport(), fast}; presence.configure(true);
        until([&] { return presence.status().phase == LastfmPhase::needs_authorization; });
        check(presence.status().pending == 1 && load_lastfm_credentials(temporary.path()).session_key.empty(), "revocation pauses submission without losing queue");
    }
    save_lastfm_credentials(temporary.path(), credentials()); Fake restored;
    {
        LastfmPresence presence{temporary.path(), restored.transport(), fast}; connected(presence);
        until([&] { return presence.status().scrobble_status == "Scrobble accepted"; });
        check(restored.count() == 1, "reauthorized original account resumes its backlog");
    }
}
void live_retry_tests()
{
    for (const int code : {11, 29}) {
        Temporary temporary; Fake fake; fake.api_error = code;
        LastfmPresence presence{temporary.path(), fake.transport(), fast}; connected(presence);
        presence.publish(events(eligible()), std::nullopt);
        until([&] { return presence.status().scrobble_status.find("retry after Last.fm error " + std::to_string(code)) != std::string::npos; });
        check(fake.count() == 1, "transient/rate-limit API failure does not tight-loop");
        fake.api_error = 0;
        until([&] { return presence.status().scrobble_status == "Scrobble accepted"; });
        check(fake.count() == 2 && presence.status().pending == 0, "background retry eventually accepts without restart");
        check(fake.call(0) == fake.call(1), "retry request unchanged, including signature and original timestamp");
    }
    Temporary temporary; Fake fake; fake.numeric_ack = true; fake.array_ack = true;
    LastfmPresence presence{temporary.path(), fake.transport(), fast}; connected(presence);
    presence.publish(events(eligible()), std::nullopt);
    until([&] { return presence.status().scrobble_status == "Scrobble accepted"; });
    check(presence.status().pending == 0 && fake.count() == 1, "numeric JSON attributes and one-item scrobble arrays accepted");
}
void shutdown_and_safety_tests()
{
    Temporary temporary; Fake blocked; blocked.blocked = true;
    {
        LastfmPresence presence{temporary.path(), blocked.transport(), fast}; connected(presence);
        presence.publish(events(eligible()), std::nullopt);
        until([&] { return blocked.count() == 1; });
        presence.configure(false);
        until([&] { return blocked.cancelled.load(); });
    }
    { LastfmQueue queue{temporary.queue()}; check(queue.size("test-user") == 1, "disable cancels request but retains durable entry"); }
    Temporary flush; Fake fake; fake.blocked = true;
    {
        LastfmPresence presence{flush.path(), fake.transport(), fast}; connected(presence);
        presence.publish(events(eligible()), std::nullopt);
        // Immediate shutdown must flush the accepted mailbox before joining.
    }
    { LastfmQueue queue{flush.queue()}; check(queue.size("test-user") == 1, "normal shutdown persists final eligibility event"); }
    Temporary broken;
    { std::ofstream file{broken.queue()}; file << "{\"version\":99}"; }
    Fake broken_service;
    {
        LastfmPresence presence{broken.path(), broken_service.transport(), fast}; connected(presence);
        presence.publish(events(eligible()), std::nullopt);
        until([&] { return presence.status().dropped == 1; });
        check(presence.status().scrobble_status.starts_with("Scrobble queue:") && broken_service.count() == 0, "queue failure visible, unsaved entry never transmitted");
    }
    check(contents(broken.queue()) == "{\"version\":99}", "broken outbox preserved by worker");
    Temporary unwritable; Fake not_sent;
    {
        LastfmPresence presence{unwritable.path(), not_sent.transport(), fast}; connected(presence);
        // Force an atomic-rename failure independently of uid/umask/root privileges.
        std::filesystem::create_directory(unwritable.queue());
        presence.publish(events(eligible()), std::nullopt);
        until([&] { return presence.status().dropped == 1; });
        check(not_sent.count() == 0 && presence.status().scrobble_status.find("I/O failure") != std::string::npos, "disk failure never transmits an unsaved scrobble");
    }
}
void concurrent_control_tests()
{
    Temporary fixture; Fake fake; fake.hold_reply = true; fake.api_error = 9;
    {
        LastfmPresence presence{fixture.path(), fake.transport(), fast}; connected(presence);
        presence.publish(events(eligible()), std::nullopt);
        until([&] { return fake.count() == 1; });
        // Scrobble replies must survive unrelated playback revisions, including
        // an invalid-session reply received after a track change or stop.
        presence.publish({}, eligible("different-session"));
        fake.release_reply = true;
        until([&] { return presence.status().phase == LastfmPhase::needs_authorization; });
        std::this_thread::sleep_for(20ms);
        check(fake.count() == 1 && load_lastfm_credentials(fixture.path()).session_key.empty(), "track change cannot discard scrobble revocation and trigger another submission");
    }
    for (bool reenable : {false, true}) {
        Temporary temporary; Fake blocked; blocked.blocked = true;
        {
            LastfmPresence presence{temporary.path(), blocked.transport(), fast}; connected(presence);
            presence.publish(events(eligible()), std::nullopt);
            until([&] { return blocked.count() == 1; });
            presence.disconnect();
            if (reenable) { presence.configure(true); }
            // Immediate quit used to discard a pending disconnect action; rapid
            // re-enable used to replace it with load before credentials were cleared.
        }
        check(load_lastfm_credentials(temporary.path()).session_key.empty(), "disconnect persists even across immediate quit/re-enable");
        LastfmQueue queue{temporary.queue()};
        check(queue.size("test-user") == 1, "local disconnect still retains account-associated backlog");
    }
}
void controller_delivery_test()
{
    Temporary temporary; Fake service;
    LastfmPresence presence{temporary.path(), service.transport(), fast}; connected(presence);
    Track track;
    track.stable_id = "private-library-id"; track.beatmap_set_id = track.stable_id;
    track.audio_hash = std::string(64, 'a'); track.audio_path = "/never-opened/private/audio";
    track.artist = "Controller artist"; track.title = "Controller 日本 title";
    auto backend = std::make_unique<FakeAudioPlayer>();
    auto* audio = backend.get(); audio->duration_value = 120s;
    PlaybackTime time{std::chrono::steady_clock::now(), std::chrono::system_clock::now()};
    PlayerController player{Library{{track}}, std::move(backend), 12345, [&] { return time; }};
    auto publish = [&] {
        const auto batch = player.take_playback_events();
        presence.publish(batch, player.playback_snapshot());
        check(player.take_playback_events().events.empty(), "controller batch drained once before consumer fan-out");
    };
    auto tick = [&](std::chrono::milliseconds wall, std::chrono::milliseconds media) {
        time.monotonic += wall; time.wall += wall; audio->position_value += media;
        player.update(); publish();
    };
    player.play(); publish();
    const auto first = *player.playback_snapshot();
    tick(10s, 10s);
    player.pause(); publish(); tick(100s, 0ms);
    player.seek(80s); publish(); player.play(); publish();
    tick(20s, 20s); player.seek(0ms); publish(); tick(29s, 29s);
    check(player.playback_snapshot()->listened == 59s && service.scrobbles().empty(), "controller pause/seek time never manufactures eligibility");
    tick(1s, 1s);
    until([&] { return presence.status().scrobble_status == "Scrobble accepted"; });
    check(service.scrobbles().size() == 1 && service.scrobbles()[0].at("timestamp") ==
          std::to_string(std::chrono::duration_cast<std::chrono::seconds>(first.started_at.time_since_epoch()).count()), "real controller threshold reaches durable submission with original start time");
    player.pause(); publish(); player.play(); publish(); player.seek(0ms); publish(); tick(5s, 5s);
    check(service.scrobbles().size() == 1, "pause/resume/backward seek preserves session and cannot duplicate scrobble");
    player.stop(); publish(); player.play(); publish();
    check(player.playback_snapshot()->session_id != first.session_id, "stop/replay produces distinct session");
    // Let shutdown settle the final audio segment and produce eligibility itself.
    time.monotonic += 60s; time.wall += 60s; audio->position_value = 60s;
    player.shutdown(); publish();
    until([&] { return service.scrobbles().size() == 2 && presence.status().pending == 0; });
    check(service.scrobbles()[0].at("artist") == "Controller artist" &&
          service.scrobbles()[1].at("track") == "Controller 日本 title", "new-session shutdown eligibility reaches fake Last.fm without metadata guessing");
}
void storage_hardening_tests()
{
    Temporary temporary;
    const auto original = contents(temporary.path());
    const auto alias = temporary.root / "credential-alias";
    std::filesystem::create_hard_link(temporary.path(), alias);
    rejects([&] { (void)load_lastfm_credentials(temporary.path()); }, "hard-linked credentials refused");
    rejects([&] { save_lastfm_credentials(temporary.path(), credentials()); }, "hard-linked credentials never overwritten");
    check(contents(alias) == original, "credential hard-link target unchanged");
    std::filesystem::remove(alias);
    {
        lastfm_storage::Directory directory{temporary.path().parent_path(), false};
        auto file = directory.open_file("credentials.json", O_RDONLY);
        check((fcntl(file.fd, F_GETFD) & FD_CLOEXEC) != 0, "private descriptors cannot leak into executed child programs");
    }
    const auto entry = *lastfm_scrobble(eligible(), "test-user");
    {
        LastfmQueue queue{temporary.queue()};
        const auto target = temporary.root / "untouched";
        { std::ofstream file{target}; file << "preserve me"; }
        std::filesystem::create_symlink(target, temporary.queue());
        rejects([&] { queue.enqueue(entry); }, "outbox target replaced with symlink is refused before writing");
        check(contents(target) == "preserve me" && queue.size("test-user") == 0, "failed save cannot alter target or in-memory queue");
        std::filesystem::remove(temporary.queue());
    }
    const auto lock = std::filesystem::path{temporary.queue().string() + ".lock"};
    std::filesystem::create_hard_link(lock, alias);
    rejects([&] { LastfmQueue queue{temporary.queue()}; }, "hard-linked outbox lock refused");
    std::filesystem::remove(alias);
    {
        LastfmQueue queue{temporary.queue()}; queue.enqueue(entry);
    }
    std::filesystem::create_hard_link(temporary.queue(), alias);
    rejects([&] { LastfmQueue queue{temporary.queue()}; }, "hard-linked outbox data refused");
    std::filesystem::remove(alias);
    for (bool symlink : {false, true}) {
        Temporary replacement;
        LastfmQueue queue{replacement.queue()};
        const auto parent = replacement.path().parent_path();
        std::filesystem::rename(parent, replacement.root / "original-private-directory");
        const auto destination = replacement.root / "destination";
        std::filesystem::create_directory(destination);
        if (symlink) { std::filesystem::create_directory_symlink(destination, parent); }
        else { std::filesystem::create_directory(parent); std::filesystem::permissions(parent, std::filesystem::perms::owner_all); }
        rejects([&] { queue.enqueue(entry); }, "directory replacement cannot redirect live outbox writes");
        check(!std::filesystem::exists(destination / "scrobbles.json") && !std::filesystem::exists(parent / "scrobbles.json"), "replacement directory receives no private outbox content");
        check(queue.size("test-user") == 0, "directory replacement does not commit unsaved entry");
    }
}
void hostile_response_tests()
{
    for (const auto* code : {"0", "-1", "4294967305", "\"9\"", "true", "9.0"}) {
        Temporary temporary; Fake fake;
        fake.response_override = "{\"error\":" + std::string{code} + ",\"message\":\"DO NOT LOG SECRET\"}";
        LastfmPresence presence{temporary.path(), fake.transport(), LastfmTiming{1ms, 1000ms}};
        connected(presence); presence.publish(events(eligible()), std::nullopt);
        until([&] { return presence.status().scrobble_status.starts_with("Scrobble cached;"); });
        check(presence.status().pending == 1 && fake.count() == 1 && !load_lastfm_credentials(temporary.path()).session_key.empty(), "malformed API codes cannot consume outbox entry or revoke valid session");
        check(presence.status().scrobble_status.find("DO NOT LOG") == std::string::npos, "malformed server error text remains redacted");
    }
    {
        Temporary temporary; Fake fake;
        const auto value = eligible(); const auto entry = *lastfm_scrobble(value, "test-user");
        fake.response_override = "{\"scrobbles\":{\"@attr\":{\"accepted\":0,\"ignored\":1},\"scrobble\":{\"timestamp\":\"" +
            entry.parameters.at("timestamp") + "\",\"ignoredMessage\":{\"code\":\"00\"}}}}";
        LastfmPresence presence{temporary.path(), fake.transport(), LastfmTiming{1ms, 1000ms}};
        connected(presence); presence.publish(events(value), std::nullopt);
        until([&] { return presence.status().scrobble_status.starts_with("Scrobble cached;"); });
        check(presence.status().pending == 1, "noncanonical ignored code cannot consume a pending entry");
    }
    {
        Temporary temporary;
        const std::string duplicated = "{\"version\":1,\"pending\":[],\"receipts\":[{\"account\":\"test-user\",\"session\":\"id\"},{\"account\":\"test-user\",\"session\":\"id\"}]}";
        { std::ofstream file{temporary.queue()}; file << duplicated; }
        chmod(temporary.queue().c_str(), 0600);
        rejects([&] { LastfmQueue queue{temporary.queue()}; }, "duplicate receipt identities rejected during outbox load");
        check(contents(temporary.queue()) == duplicated, "malformed receipt file is preserved");
    }
    check(lastfm_retry_delay(1000, 1000) == 0ms && lastfm_retry_delay(500, 1000) == 0ms &&
          lastfm_retry_delay(1500, 1000) == 500ms, "ordinary persisted retry delays preserved");
    check(lastfm_retry_delay(4102444800000LL, 0) == 30min &&
          lastfm_retry_delay(std::numeric_limits<std::int64_t>::max(), std::numeric_limits<std::int64_t>::min()) == 30min,
          "future/rollback/extreme retry clocks bounded without signed overflow");
}
}  // namespace
int main()
{
    try {
        eligibility_tests(); queue_tests(); capacity_tests(); delivery_tests(); retry_restart_tests(); live_retry_tests(); account_and_revocation_tests(); shutdown_and_safety_tests(); concurrent_control_tests(); controller_delivery_test(); storage_hardening_tests(); hostile_response_tests();
        std::cout << "Last.fm scrobble eligibility, private durable queue, fake submission/retries, account isolation and shutdown tests passed (no network)\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << "Last.fm scrobble test failure: " << error.what() << '\n'; return 1; }
}
