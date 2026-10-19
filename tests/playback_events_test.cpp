#include "osu_player/player_controller.hpp"
#include "fake_audio_player.hpp"

#include <algorithm>
#include <chrono>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {
using namespace std::chrono_literals;
using namespace osu_player;

void check(const bool condition, const char* message)
{
    if (!condition) { throw std::runtime_error{std::string{"FAIL: "} + message}; }
}

Library library(const std::size_t count = 2)
{
    std::vector<Track> tracks;
    for (std::size_t index = 0; index < count; ++index) {
        Track track;
        track.stable_id = "track-" + std::to_string(index);
        track.beatmap_set_id = track.stable_id;
        track.audio_hash = std::string(64, static_cast<char>('a' + index));
        track.title = "Title " + std::to_string(index);
        track.title_unicode = "日本語";
        track.artist = "Artist";
        track.artist_unicode = "作曲家";
        track.online_set_id = 123;
        track.audio_path = "/private/content/" + track.stable_id;
        tracks.push_back(std::move(track));
    }
    return Library{std::move(tracks)};
}

struct Fixture {
    PlaybackTime time{std::chrono::steady_clock::time_point{10s}, std::chrono::system_clock::time_point{100s}};
    FakeAudioPlayer* audio;
    PlayerController player;

    explicit Fixture(const std::size_t count = 2) : Fixture{count, std::make_unique<FakeAudioPlayer>()} { }
    Fixture(const std::size_t count, std::unique_ptr<FakeAudioPlayer> backend)
        : audio{backend.get()}, player{library(count), std::move(backend), 12345, [this] { return time; }}
    {
        audio->duration_value = 120s;
    }

    void advance(const std::chrono::milliseconds clock, const std::chrono::milliseconds media)
    {
        time.monotonic += clock;
        time.wall += clock;
        audio->position_value += media;
    }
    void tick(const std::chrono::milliseconds clock, const std::chrono::milliseconds media)
    {
        advance(clock, media);
        player.update();
    }
};

std::size_t count(const PlaybackEvents& batch, const PlaybackEventType type)
{
    return static_cast<std::size_t>(std::count_if(batch.events.begin(), batch.events.end(),
        [type](const auto& event) { return event.type == type; }));
}

const PlaybackEvent& ended(const PlaybackEvents& batch, const PlaybackEndReason reason)
{
    const auto found = std::find_if(batch.events.begin(), batch.events.end(), [reason](const auto& event) {
        return event.type == PlaybackEventType::ended && event.reason == reason;
    });
    check(found != batch.events.end(), "expected session end reason");
    return *found;
}

void test_transport_and_seek_accounting()
{
    Fixture fixture;
    auto& player = fixture.player;
    check(!player.playback_snapshot() && player.take_playback_events().events.empty(), "no startup session/autoplay");
    player.play();
    const auto first = *player.playback_snapshot();
    check(first.started_at == fixture.time.wall && first.playing && first.listened == 0ms,
          "start records wall timestamp and zero listening");
    check(first.track.title_unicode == "日本語" && first.track.artist_unicode == "作曲家" && first.track.online_set_id == 123,
          "events retain structured Unicode metadata");
    player.play();
    fixture.tick(10s, 10s);
    fixture.advance(2s, 2s); // Pause must account for time since the last update.
    player.pause();
    check(player.playback_snapshot()->listened == 12s && !player.playback_snapshot()->playing,
          "pause settles the final consumed audio segment");
    fixture.advance(100s, 0ms);
    player.update();
    player.seek(80s);
    check(player.playback_snapshot()->listened == 12s && player.playback_snapshot()->position == 80s,
          "paused seek credits neither waiting nor skipped media");
    player.play();
    check(player.playback_snapshot()->session_id == first.session_id, "resume and seek keep the same play session");
    fixture.tick(20s, 20s);
    fixture.advance(2s, 2s);
    player.seek(110s);
    check(player.playback_snapshot()->listened == 34s, "playing seek settles old position before rebasing");
    fixture.tick(1s, 1s);
    player.seek(0ms);
    check(player.playback_snapshot()->listened == 35s, "backward seek never subtracts or invents listening");
    const auto batch = player.take_playback_events();
    check(count(batch, PlaybackEventType::started) == 1 && count(batch, PlaybackEventType::paused) == 1 &&
          count(batch, PlaybackEventType::resumed) == 1 && count(batch, PlaybackEventType::seeked) == 3,
          "transport emits ordered explicit events without duplicate start");
    check(player.take_playback_events().events.empty(), "event drain is destructive");
    fixture.advance(3s, 3s);
    player.stop();
    const auto stopped = player.take_playback_events();
    check(ended(stopped, PlaybackEndReason::stopped).playback.listened == 38s && !player.playback_snapshot(),
          "stop finalizes listening before backend rewind");
    player.stop();
    check(player.take_playback_events().events.empty(), "repeated stop has no duplicate terminal event");
    player.seek(50s);
    check(!player.playback_snapshot(), "seeking a stopped selection is not a listen");
    player.play();
    check(player.playback_snapshot()->session_id != first.session_id && player.playback_snapshot()->listened == 0ms,
          "play from a stopped/seeked selection begins a fresh listening session");
    player.shutdown();
    check(ended(player.take_playback_events(), PlaybackEndReason::shutdown).playback.position == 50s,
          "shutdown retains final media position");
    check(!player.playback_snapshot() && player.state() == PlayerState::paused, "shutdown pauses without rewinding");
}

void test_clock_and_consumed_media_limits()
{
    Fixture fixture;
    fixture.player.play();
    fixture.tick(0ms, 60s);
    check(fixture.player.playback_snapshot()->listened == 0ms, "position jump alone cannot count listening");
    fixture.tick(20s, 0ms);
    check(fixture.player.playback_snapshot()->listened == 0ms, "clock time alone cannot count an underrun");
    fixture.tick(10s, 1s);
    fixture.tick(1s, 10s);
    check(fixture.player.playback_snapshot()->listened == 11s,
          "delayed media reports credit only observed media within accumulated playing time");
    fixture.time.wall += 24h;
    fixture.tick(1s, 1s);
    fixture.time.wall -= 48h;
    fixture.tick(1s, 1s);
    check(fixture.player.playback_snapshot()->listened == 13s &&
          fixture.player.playback_snapshot()->started_at == std::chrono::system_clock::time_point{100s},
          "wall-clock corrections do not affect elapsed listening or original start timestamp");
    fixture.tick(1s, -20s);
    check(fixture.player.playback_snapshot()->listened == 13s, "backward backend discontinuities earn no credit");
    fixture.time.monotonic -= 10s;
    fixture.audio->position_value += 1s;
    fixture.player.update();
    check(fixture.player.playback_snapshot()->listened == 13s, "injected backward monotonic time never subtracts credit");

    Fixture jumped;
    jumped.player.play();
    jumped.tick(1s, 60s);
    check(jumped.player.playback_snapshot()->listened == 1s, "media movement cannot exceed available monotonic time");
    jumped.tick(10s, 0ms);
    check(jumped.player.playback_snapshot()->listened == 1s, "excess media movement never credits later silence");

    PlaybackTime time{};
    PlaybackMonitor monitor{[&time] { return time; }};
    monitor.begin(library(1).tracks().front(), 0ms, 120s);
    for (int index = 1; index <= 1000; ++index) {
        time.monotonic += 1500us;
        monitor.sample(std::chrono::milliseconds{index * 2}, 120s);
    }
    check(monitor.snapshot()->listened == 1500ms, "submillisecond clock fractions are not rounded away at every sample");

    Fixture buffered;
    buffered.player.play();
    for (int index = 0; index < 1000; ++index) {
        buffered.tick(10ms, index % 2 == 0 ? 0ms : 20ms);
    }
    check(buffered.player.playback_snapshot()->listened == 10s,
          "bursty audio-device consumption is credited across polling intervals");

    Fixture jitter;
    jitter.player.play();
    jitter.tick(30ms, 20ms);
    jitter.tick(10ms, 20ms);
    check(jitter.player.playback_snapshot()->listened == 40ms,
          "elapsed allowance survives irregular polling without discarding consumed audio");
}

void test_eligibility_boundaries()
{
    const std::vector<std::chrono::milliseconds> durations{-1ms, 0ms, 30s, 30s + 1ms, 120s, 600s};
    for (const auto duration : durations) {
        Fixture fixture{1};
        fixture.audio->duration_value = duration;
        fixture.player.play();
        if (duration <= 30s) {
            fixture.tick(240s, 240s);
            check(!fixture.player.playback_snapshot()->listening_eligible, "duration must be known and strictly over 30 seconds");
            continue;
        }
        const auto threshold = std::min(duration / 2, std::chrono::duration_cast<std::chrono::milliseconds>(4min));
        fixture.tick(threshold, threshold);
        if (duration == 30s + 1ms) {
            check(!fixture.player.playback_snapshot()->listening_eligible, "half-duration is not rounded down to whole milliseconds");
            fixture.tick(1ms, 1ms);
        }
        check(fixture.player.playback_snapshot()->listening_eligible, "half-duration or four-minute threshold qualifies");
        fixture.tick(10s, 10s);
        fixture.player.pause();
        fixture.player.play();
        fixture.player.seek(0ms);
        fixture.tick(10s, 10s);
        check(count(fixture.player.take_playback_events(), PlaybackEventType::listening_eligible) == 1,
              "eligibility is announced only once per session despite resume/seek");
    }
    Fixture long_track{1};
    long_track.audio->duration_value = 600s;
    long_track.player.play();
    long_track.tick(240s - 1ms, 240s - 1ms);
    check(!long_track.player.playback_snapshot()->listening_eligible, "long track does not qualify before four minutes");
    long_track.tick(1ms, 1ms);
    check(long_track.player.playback_snapshot()->listening_eligible, "four-minute boundary qualifies exactly");

    Fixture unknown{1};
    unknown.audio->duration_value.reset();
    unknown.player.play();
    unknown.tick(70s, 70s);
    check(!unknown.player.playback_snapshot()->listening_eligible, "unknown duration does not qualify speculatively");
    unknown.audio->duration_value = 120s;
    unknown.player.update();
    check(unknown.player.playback_snapshot()->listening_eligible && unknown.player.playback_snapshot()->listened == 70s,
          "duration learned later can qualify accumulated real listening");

    Fixture enormous{1};
    enormous.audio->duration_value = std::chrono::milliseconds::max();
    enormous.player.play();
    enormous.time.monotonic += 240s;
    enormous.audio->position_value = std::chrono::milliseconds::max();
    enormous.player.update();
    check(enormous.player.playback_snapshot()->listened == 240s && enormous.player.playback_snapshot()->listening_eligible,
          "extreme reported duration/position are capped before narrowing clock conversions");
}

void test_repeat_navigation_and_owned_snapshots()
{
    Fixture fixture{1};
    fixture.audio->duration_value = 60s;
    fixture.player.set_repeat(RepeatMode::one);
    fixture.player.play();
    const auto first = *fixture.player.playback_snapshot();
    fixture.advance(60s, 60s);
    fixture.audio->finish_on_update = true;
    fixture.player.update();
    const auto batch = fixture.player.take_playback_events();
    check(ended(batch, PlaybackEndReason::completed).playback.listened == 60s,
          "EOF settles old track before repeat loads and resets backend position");
    check(count(batch, PlaybackEventType::started) == 2 && count(batch, PlaybackEventType::listening_eligible) == 1 &&
          batch.events.back().type == PlaybackEventType::started,
          "EOF completion precedes the next session even for the same track");
    check(fixture.player.playback_snapshot()->session_id != first.session_id && fixture.player.playback_snapshot()->listened == 0ms,
          "repeat-one creates a distinct listen");
    fixture.player.play_track(0);
    check(ended(fixture.player.take_playback_events(), PlaybackEndReason::replaced).playback.track.stable_id == "track-0",
          "explicit replay ends the prior session");
    fixture.player.set_repeat(RepeatMode::all);
    const auto repeated = fixture.player.playback_snapshot()->session_id;
    fixture.player.next();
    check(fixture.player.playback_snapshot()->session_id != repeated, "repeat-all wrapping starts a new session");
    check(ended(fixture.player.take_playback_events(), PlaybackEndReason::skipped).playback.session_id == repeated,
          "manual next is distinguished from natural EOF");
    fixture.player.set_repeat(RepeatMode::off);
    fixture.player.next();
    check(!fixture.player.playback_snapshot(), "manual next past queue end closes the active session");
    check(count(fixture.player.take_playback_events(), PlaybackEventType::ended) == 1, "no double terminal event");

    Fixture another{1};
    another.player.play();
    check(another.player.playback_snapshot()->session_id != first.session_id, "controllers have independent session namespaces");
    check(first.track.title == "Title 0" && first.listened == 0ms, "snapshot copies never mutate with later playback");
}

void test_refresh_and_preferences()
{
    Fixture fixture;
    fixture.player.play();
    fixture.tick(10s, 10s);
    const auto original = *fixture.player.playback_snapshot();
    (void)fixture.player.take_playback_events();
    fixture.player.set_volume(0.5F);
    fixture.player.set_shuffle(true);
    fixture.player.set_repeat(RepeatMode::all);
    check(fixture.player.take_playback_events().events.empty(), "preferences do not fabricate playback transitions");
    auto tracks = library().tracks();
    tracks[0].title = "Updated title";
    fixture.player.replace_library(Library{tracks});
    check(fixture.player.playback_snapshot()->session_id == original.session_id &&
          fixture.player.playback_snapshot()->listened == 10s, "refresh retains session and accumulated listening");
    check(count(fixture.player.take_playback_events(), PlaybackEventType::metadata_changed) == 1,
          "surviving track metadata is updated without a fresh start");
    check(original.track.title == "Title 0", "queued metadata owns values rather than library pointers");
    auto changed = fixture.player.library().tracks();
    for (auto& track : changed) { if (track.stable_id == original.track.stable_id) { track.audio_hash = std::string(64, 'z'); } }
    fixture.player.replace_library(Library{changed});
    check(!fixture.player.playback_snapshot(), "changed content hash cannot inherit an old listening session");
    check(ended(fixture.player.take_playback_events(), PlaybackEndReason::removed).playback.session_id == original.session_id,
          "refresh removal finalizes the original track");
    fixture.player.replace_library(Library{});
    check(fixture.player.take_playback_events().events.empty(), "empty refresh with no active session creates no events");
}

void test_errors_and_paused_recovery()
{
    Fixture recovering;
    recovering.player.play();
    recovering.tick(5s, 5s);
    const auto failed_id = recovering.player.playback_snapshot()->session_id;
    recovering.audio->bad_updates.insert(recovering.audio->source);
    recovering.player.update();
    const auto recovered = recovering.player.take_playback_events();
    check(ended(recovered, PlaybackEndReason::track_error).playback.session_id == failed_id &&
          recovering.player.playback_snapshot()->session_id != failed_id &&
          recovering.player.playback_snapshot()->listened == 0ms &&
          recovered.events.back().type == PlaybackEventType::started,
          "decoder recovery ends the failed listen before starting a clean replacement session");

    Fixture fixture;
    fixture.audio->bad_loads.insert("/private/content/track-0");
    fixture.player.play();
    check(fixture.player.playback_snapshot()->track.stable_id == "track-1" &&
          count(fixture.player.take_playback_events(), PlaybackEventType::started) == 1,
          "failed loads never become listening sessions");
    fixture.tick(10s, 10s);
    fixture.audio->bad_updates.insert(fixture.audio->source);
    fixture.player.update();
    check(!fixture.player.playback_snapshot() &&
          ended(fixture.player.take_playback_events(), PlaybackEndReason::track_error).playback.listened == 10s,
          "terminal decode failure ends the prior session once with correct cause");

    Fixture output;
    output.player.play();
    output.audio->fail_output_update = true;
    bool caught{};
    try { output.player.update(); } catch (const AudioOutputError&) { caught = true; }
    check(caught && !output.player.playback_snapshot(), "output error closes active session before propagating");
    (void)ended(output.player.take_playback_events(), PlaybackEndReason::output_error);

    Fixture paused;
    paused.player.play();
    paused.tick(5s, 5s);
    paused.player.pause();
    paused.audio->fail_seek = true;
    paused.player.seek(80s);
    const auto events = paused.player.take_playback_events();
    check(ended(events, PlaybackEndReason::track_error).playback.listened == 5s &&
          count(events, PlaybackEventType::seeked) == 0, "failed seek never counts skipped time or emits a successful seek");
    check(!paused.player.playback_snapshot() && paused.player.state() == PlayerState::paused,
          "loaded paused replacement is not a listen until actually played");
    paused.player.play();
    check(paused.player.playback_snapshot()->track.stable_id == "track-1" &&
          paused.player.playback_snapshot()->listened == 0ms, "resume of recovered selection starts its own session");

    Fixture broken;
    broken.audio->bad_plays.insert("/private/content/track-0");
    broken.audio->bad_plays.insert("/private/content/track-1");
    broken.player.play();
    check(broken.player.take_playback_events().events.empty(), "failed playback attempts do not announce starts");

    Fixture invalid;
    invalid.player.play();
    const auto before = invalid.player.playback_snapshot()->session_id;
    (void)invalid.player.take_playback_events();
    caught = false;
    try { invalid.player.play_track(500); } catch (const std::out_of_range&) { caught = true; }
    check(caught && invalid.player.playback_snapshot()->session_id == before &&
          invalid.player.take_playback_events().events.empty(), "rejected selection does not end the current listen");

    Fixture backwards;
    backwards.player.play_track(1);
    (void)backwards.player.take_playback_events();
    backwards.player.previous();
    check(ended(backwards.player.take_playback_events(), PlaybackEndReason::skipped).playback.track.stable_id == "track-1" &&
          backwards.player.playback_snapshot()->track.stable_id == "track-0", "previous retains the ended track's identity");
}

void test_event_bounds()
{
    Fixture fixture;
    fixture.player.play();
    for (int index = 0; index < 10'001; ++index) { fixture.tick(1ms, 1ms); }
    auto events = fixture.player.take_playback_events();
    check(events.events.size() == 2 && events.omitted == 0 &&
          events.events.back().playback.listened == 10'001ms,
          "adjacent progress samples coalesce without losing cumulative credit");
    for (int index = 0; index < 100; ++index) {
        fixture.player.pause();
        fixture.player.play();
    }
    events = fixture.player.take_playback_events();
    check(events.events.size() == PlaybackMonitor::event_limit && events.omitted == 200 - PlaybackMonitor::event_limit,
          "undrained transition buffer is bounded with explicit overflow count");
    check(fixture.player.take_playback_events().omitted == 0, "draining resets overflow count");
    fixture.player.shutdown();
    const auto final = fixture.player.take_playback_events();
    (void)ended(final, PlaybackEndReason::shutdown);
    fixture.player.shutdown();
    check(fixture.player.take_playback_events().events.empty(), "shutdown is idempotent");
}
}  // namespace

int main()
{
    try {
        test_transport_and_seek_accounting();
        test_clock_and_consumed_media_limits();
        test_eligibility_boundaries();
        test_repeat_navigation_and_owned_snapshots();
        test_refresh_and_preferences();
        test_errors_and_paused_recovery();
        test_event_bounds();
        std::cout << "all playback event/listening tests passed (fake clocks; no network or real library)\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
