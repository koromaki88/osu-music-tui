#include "osu_player/player_controller.hpp"
#include "fake_audio_player.hpp"

#include <algorithm>
#include <chrono>
#include <iomanip>
#include <iostream>
#include <memory>
#include <numeric>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace std::chrono_literals;
using osu_player::PlayerState;
using osu_player::RepeatMode;

void check(const bool condition, const char* message)
{
    if (!condition) {
        throw std::runtime_error{std::string{"FAIL: "} + message};
    }
}

osu_player::Library make_library(const std::size_t size)
{
    std::vector<osu_player::Track> tracks;
    for (std::size_t index = 0; index < size; ++index) {
        std::ostringstream id;
        id << std::setw(5) << std::setfill('0') << index;
        osu_player::Track track;
        track.stable_id = id.str();
        track.beatmap_set_id = id.str();
        track.title = id.str();
        track.artist = "Artist";
        track.audio_hash = std::string(64, 'a');
        track.audio_path = "/fake/" + id.str();
        tracks.push_back(std::move(track));
    }
    return osu_player::Library{std::move(tracks)};
}

struct Fixture {
    FakeAudioPlayer* audio;
    osu_player::PlayerController controller;

    explicit Fixture(const std::size_t count)
        : Fixture{count, std::make_unique<FakeAudioPlayer>()} { }

    Fixture(const std::size_t count, std::unique_ptr<FakeAudioPlayer> backend)
        : audio{backend.get()}, controller{make_library(count), std::move(backend), 12345} { }

    std::string path(const std::size_t index) const
    {
        return controller.library().tracks()[index].audio_path.string();
    }

    void finish()
    {
        audio->finish_on_update = true;
        controller.update();
    }
};

void test_empty_and_bounds()
{
    Fixture empty{0};
    empty.controller.play();
    empty.controller.next();
    empty.controller.previous();
    empty.controller.pause();
    empty.controller.stop();
    empty.controller.seek(500ms);
    empty.controller.update();
    empty.controller.set_shuffle(true);
    empty.controller.set_repeat(RepeatMode::all);
    check(empty.controller.state() == PlayerState::empty && !empty.controller.current_index(),
          "empty queue stays empty under all transport commands");
    check(!empty.controller.current_track() && !empty.controller.duration(), "empty queue has no current media");
    check(empty.audio->attempts.empty(), "empty queue never asks backend to load");
    bool rejected = false;
    try {
        osu_player::PlayerController invalid{make_library(1), nullptr};
    } catch (const std::invalid_argument&) { rejected = true; }
    check(rejected, "a null backend is rejected");

    Fixture one{1};
    one.controller.play();
    rejected = false;
    try {
        one.controller.play_track(1);
    } catch (const std::out_of_range&) { rejected = true; }
    check(rejected && one.controller.state() == PlayerState::playing
          && one.controller.current_index() == 0, "invalid selection preserves playback");
}

void test_transport()
{
    Fixture fixture{3};
    auto& player = fixture.controller;
    player.play();
    check(player.current_index() == 0 && player.state() == PlayerState::playing, "play starts first track");
    check(player.current_track()->stable_id == "00000", "current track comes from owned library");
    player.play();
    check(fixture.audio->attempts.size() == 1, "play while playing is idempotent");
    player.pause();
    const auto updates = fixture.audio->update_calls;
    player.update();
    check(player.state() == PlayerState::paused && fixture.audio->update_calls == updates,
          "paused controller does not pump audio");
    player.seek(400ms);
    check(player.position() == 400ms, "seek delegates to backend");
    player.play();
    check(fixture.audio->attempts.size() == 1, "resume does not reload audio");
    player.set_volume(0.3F);
    check(player.volume() == 0.3F, "volume delegates to backend");
    player.stop();
    check(player.state() == PlayerState::stopped && player.position() == 0ms && player.current_index() == 0,
          "stop rewinds and retains selection");
    player.play();
    check(fixture.audio->attempts.size() == 2, "play after stop reloads from scratch");
    player.next();
    check(player.current_index() == 1, "next advances");
    player.previous();
    check(player.current_index() == 0, "previous returns to prior entry");
    player.previous();
    check(player.current_index() == 0, "previous at head restarts first entry without underflow");
    player.play_track(2);
    check(player.current_index() == 2, "explicit selection uses library index");
    player.stop();
    player.seek(200ms);
    check(player.state() == PlayerState::paused && player.position() == 200ms,
          "seeking a stopped track makes the target resumable");
}

void test_eof_and_repeat()
{
    Fixture fixture{2};
    auto& player = fixture.controller;
    player.play();
    fixture.finish();
    check(player.current_index() == 1, "EOF automatically advances");
    fixture.finish();
    check(player.state() == PlayerState::finished && player.current_index() == 1,
          "EOF at tail finishes without wrapping when repeat is off");
    const auto attempts = fixture.audio->attempts.size();
    player.update();
    check(fixture.audio->attempts.size() == attempts, "completed queue does not keep loading");
    player.play();
    check(player.current_index() == 0, "playing a completed queue starts from its beginning");
    player.set_repeat(RepeatMode::one);
    fixture.finish();
    check(player.current_index() == 0, "repeat-one restarts current track on EOF");
    player.next();
    check(player.current_index() == 1, "manual next ignores repeat-one");
    player.previous();
    check(player.current_index() == 0, "manual previous ignores repeat-one");
    player.set_repeat(RepeatMode::all);
    player.previous();
    check(player.current_index() == 1, "repeat-all wraps backwards");
    fixture.finish();
    check(player.current_index() == 0 && player.state() == PlayerState::playing,
          "repeat-all wraps forward on EOF");

    Fixture single{1};
    single.controller.set_repeat(RepeatMode::all);
    single.controller.play();
    single.finish();
    check(single.controller.current_index() == 0, "single-entry repeat-all wraps safely");
}

void test_shuffle()
{
    Fixture first{20};
    Fixture second{20};
    first.controller.set_shuffle(true);
    second.controller.set_shuffle(true);
    check(first.controller.queue() == second.controller.queue(), "shuffle is reproducible with an injected seed");
    auto sorted = first.controller.queue();
    std::ranges::sort(sorted);
    std::vector<std::size_t> canonical(20);
    std::iota(canonical.begin(), canonical.end(), std::size_t{0});
    check(sorted == canonical, "shuffle produces a permutation without omissions or duplicates");
    const auto order = first.controller.queue();
    first.controller.set_shuffle(true);
    check(first.controller.queue() == order, "enabling shuffle twice is idempotent");
    first.controller.play();
    check(first.controller.current_index() == order.front(), "play follows shuffled order");
    first.controller.next();
    first.controller.previous();
    check(first.controller.current_index() == order.front(), "previous follows actual shuffle history");
    first.controller.play_track(order.back());
    first.controller.set_repeat(RepeatMode::all);
    first.controller.next();
    check(first.controller.current_index() != order.back(), "new shuffled cycle avoids immediate repeat at boundary");

    Fixture midway{20};
    midway.controller.play_track(3);
    const auto loads = midway.audio->attempts.size();
    midway.controller.set_shuffle(true);
    check(midway.controller.current_index() == 3 && midway.audio->attempts.size() == loads,
          "enabling shuffle does not reload or change current song");
    check(std::equal(midway.controller.queue().begin(), midway.controller.queue().begin() + 4,
                     canonical.begin()), "enabling shuffle preserves played prefix");
    midway.controller.set_shuffle(false);
    check(midway.controller.current_index() == 3 && midway.controller.queue() == canonical,
          "disabling shuffle restores canonical order while preserving song");
}

void test_load_play_and_runtime_failures()
{
    Fixture fixture{4};
    auto& player = fixture.controller;
    fixture.audio->bad_loads.insert(fixture.path(0));
    fixture.audio->bad_plays.insert(fixture.path(1));
    player.play();
    check(player.current_index() == 2, "load/play failures are skipped until a playable track is found");
    auto errors = player.take_errors();
    check(errors.tracks.size() == 2 && errors.tracks[0].stable_id == "00000", "failures retain track identity");
    check(player.take_errors().tracks.empty(), "taking errors drains the report");
    fixture.audio->bad_updates.insert(fixture.path(2));
    player.set_repeat(RepeatMode::one);
    player.update();
    check(player.current_index() == 3, "decoder failures advance even in repeat-one");
    fixture.audio->bad_updates.insert(fixture.path(3));
    player.update();
    check(player.state() == PlayerState::failed && !player.current_index(),
          "a runtime failure at queue tail leaves no fake current track");
    check(fixture.audio->state() != osu_player::PlaybackState::playing,
          "failed queue does not leave stale source audible");

    Fixture looping{3};
    looping.controller.set_repeat(RepeatMode::all);
    for (std::size_t index = 0; index < 3; ++index) {
        looping.audio->bad_updates.insert(looping.path(index));
    }
    looping.controller.play();
    for (int tick = 0; tick < 10; ++tick) {
        looping.controller.update();
    }
    check(looping.controller.state() == PlayerState::failed && looping.audio->attempts.size() == 3,
          "decode failures are remembered across repeat-all cycles, preventing retry storms");
    looping.audio->bad_updates.clear();
    looping.controller.play();
    check(looping.controller.state() == PlayerState::playing, "explicit play retries a failed session");
}

void test_large_broken_queue_and_error_bound()
{
    Fixture fixture{10'001};
    for (const auto& track : fixture.controller.library().tracks()) {
        fixture.audio->bad_loads.insert(track.audio_path.string());
    }
    fixture.controller.set_repeat(RepeatMode::all);
    fixture.controller.play();
    check(fixture.audio->attempts.size() == 10'001 && fixture.controller.state() == PlayerState::failed,
          "all-broken 10001-track queue terminates after one pass, not recursion");
    auto errors = fixture.controller.take_errors();
    check(errors.tracks.size() == 32 && errors.omitted == 10'001 - 32,
          "error buffering is bounded and reports omitted count");
    check(fixture.controller.take_errors().omitted == 0, "draining resets omitted count");
    fixture.audio->bad_loads.clear();
    fixture.controller.play_track(123);
    check(fixture.controller.current_index() == 123, "explicit selection retries a previously broken entry");
}

void test_directional_failure_skipping()
{
    Fixture fixture{4};
    fixture.audio->bad_loads.insert(fixture.path(1));
    fixture.controller.play_track(2);
    fixture.controller.previous();
    check(fixture.controller.current_index() == 0, "previous skips bad tracks backwards");
    fixture.audio->bad_loads.insert(fixture.path(3));
    fixture.controller.set_repeat(RepeatMode::all);
    fixture.controller.previous();
    check(fixture.controller.current_index() == 2, "backwards wrapping skips bad tail entries");
    fixture.controller.next();
    check(fixture.controller.current_index() == 0, "forward wrapping skips known bad entries");
    const auto state = fixture.controller.state();
    bool rejected = false;
    try {
        fixture.controller.set_volume(2.0F);
    } catch (const std::invalid_argument&) { rejected = true; }
    check(rejected && fixture.controller.state() == state, "invalid volume does not disturb playback state");
}

void test_seek_stop_and_resume_failures()
{
    Fixture fixture{3};
    auto& player = fixture.controller;
    player.play();
    player.pause();
    fixture.audio->bad_plays.insert(fixture.path(0));
    player.play();
    check(player.current_index() == 1, "resume failure advances to a playable track");
    fixture.audio->fail_seek = true;
    player.seek(500ms);
    check(player.current_index() == 2, "seek failure advances safely");
    fixture.audio->fail_stop = true;
    player.stop();
    check(player.state() == PlayerState::stopped && player.position() == 0ms,
          "stop failure still pauses output and displays stopped state");
    fixture.audio->fail_stop = false;
    player.play();
    check(player.current_index() == 2, "play reloads selected track after stop failure");

    Fixture paused{2};
    paused.controller.play();
    paused.controller.pause();
    paused.audio->fail_seek = true;
    paused.controller.seek(100ms);
    check(paused.controller.current_index() == 1 && paused.controller.state() == PlayerState::paused,
          "seek failure while paused must not unexpectedly start audible playback");
    check(paused.audio->play_calls == 1, "paused seek recovery never briefly starts the replacement");
}

void test_output_failure_is_not_a_bad_track()
{
    Fixture fixture{100};
    fixture.audio->fail_output_load = true;
    bool rejected = false;
    try {
        fixture.controller.play();
    } catch (const osu_player::AudioOutputError&) { rejected = true; }
    check(rejected && fixture.audio->attempts.size() == 1,
          "device failure aborts immediately rather than scanning all tracks");
    check(fixture.controller.state() == PlayerState::failed && !fixture.controller.current_index(),
          "device failure clears current selection and reports failed state");
    check(fixture.controller.take_errors().tracks.empty(), "device errors are not mislabeled as bad tracks");
    fixture.audio->fail_output_load = false;
    fixture.controller.play();
    fixture.audio->fail_output_update = true;
    rejected = false;
    try {
        fixture.controller.update();
    } catch (const osu_player::AudioOutputError&) { rejected = true; }
    check(rejected && fixture.controller.state() == PlayerState::failed
          && fixture.audio->state() != osu_player::PlaybackState::playing,
          "device failure during output stops the session");
}

}  // namespace

int main()
{
    try {
        test_empty_and_bounds();
        test_transport();
        test_eof_and_repeat();
        test_shuffle();
        test_load_play_and_runtime_failures();
        test_large_broken_queue_and_error_bound();
        test_directional_failure_skipping();
        test_seek_stop_and_resume_failures();
        test_output_failure_is_not_a_bad_track();
        std::cout << "all player controller tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
