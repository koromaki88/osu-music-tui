#include "osu_player/audio_player.hpp"

#include <chrono>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <thread>

namespace {

using namespace std::chrono_literals;

void check(const bool condition, const char* message)
{
    if (!condition) {
        throw std::runtime_error{std::string{"FAIL: "} + message};
    }
}

void pump(osu_player::AudioPlayer& player, const std::chrono::milliseconds elapsed)
{
    const auto deadline = std::chrono::steady_clock::now() + elapsed;
    do {
        player.update();
        std::this_thread::sleep_for(5ms);
    } while (std::chrono::steady_clock::now() < deadline);
}

void test_audio(const std::filesystem::path& path)
{
    using osu_player::PlaybackState;
    auto player = osu_player::make_audio_player();
    check(player->state() == PlaybackState::empty, "initial state is empty");
    check(!player->duration().has_value(), "unloaded duration is unknown");
    player->stop();
    try {
        player->play();
        throw std::runtime_error{"FAIL: playing without a source must fail"};
    } catch (const std::logic_error&) { }

    player->load(path);
    check(player->state() == PlaybackState::ready, "load leaves output paused and ready");
    check(player->duration().has_value() && *player->duration() >= 1900ms,
          "duration is read from the generated two-second media");
    check(player->position() == 0ms, "initial position is zero");
    player->set_volume(0.25F);
    check(player->volume() == 0.25F, "volume is retained");
    for (const float invalid : {-1.0F, 1.1F, std::numeric_limits<float>::quiet_NaN()}) {
        try {
            player->set_volume(invalid);
            throw std::runtime_error{"FAIL: invalid volume must fail"};
        } catch (const std::invalid_argument&) { }
    }

    player->play();
    pump(*player, 200ms);
    check(player->state() == PlaybackState::playing, "audio is playing");
    check(player->position() > 0ms, "playback position advances");
    player->pause();
    const auto paused = player->position();
    pump(*player, 80ms);
    check(player->state() == PlaybackState::paused && player->position() == paused,
          "position does not advance while paused");

    player->seek(700ms);
    check(player->position() == 700ms && player->state() == PlaybackState::paused,
          "seek preserves pause state and changes position");
    player->play();
    pump(*player, 100ms);
    check(player->position() > 700ms, "resume advances from seek target");

    bool load_failed = false;
    try {
        player->load(path.string() + ".not-found");
    } catch (const std::runtime_error&) {
        load_failed = true;
    }
    check(load_failed && player->state() == PlaybackState::playing,
          "failed replacement throws and preserves current source");
    player->stop();
    check(player->state() == PlaybackState::ready && player->position() == 0ms,
          "stop rewinds and returns to ready");
    player->seek(-100ms);
    check(player->position() == 0ms, "negative seek clamps to zero");
    player->seek(1600ms);
    player->play();
    const auto deadline = std::chrono::steady_clock::now() + 3s;
    while (player->state() != PlaybackState::finished && std::chrono::steady_clock::now() < deadline) {
        pump(*player, 10ms);
    }
    check(player->state() == PlaybackState::finished, "EOF drains queued audio and reports finished");
    player->play();
    pump(*player, 80ms);
    check(player->state() == PlaybackState::playing && player->position() < 500ms,
          "playing a finished source restarts it");
    player->pause();
    player->seek(99999ms);
    check(player->position() == *player->duration(), "oversized seek clamps to duration");
    player.reset(); // Destruction while paused releases the device before decoder state.
    auto active = osu_player::make_audio_player();
    active->load(path);
    active->play();
    pump(*active, 20ms);
    // Also destroy an actively playing player; SDL must stop before resources disappear.
}

}  // namespace

int main(const int argc, char* argv[])
{
    try {
        if (argc < 2) {
            throw std::invalid_argument{"provide generated fixture paths"};
        }
        for (int index = 1; index < argc; ++index) {
            test_audio(argv[index]);
        }
        std::cout << "all audio player tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
