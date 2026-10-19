#include "osu_player/discord_presence.hpp"

#include <json-c/json.h>

#include <chrono>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>

namespace {
using namespace osu_player;
using namespace std::chrono_literals;
using Json = std::unique_ptr<json_object, decltype(&json_object_put)>;

void check(const bool condition, const char* message)
{
    if (!condition) { throw std::runtime_error{std::string{"FAIL: "} + message}; }
}

json_object* field(json_object* object, const char* name)
{
    json_object* value{};
    json_object_object_get_ex(object, name, &value);
    return value;
}

PlaybackSnapshot snapshot()
{
    PlaybackSnapshot value;
    value.session_id = "private-session-id";
    value.track = {"private-library-id", "private-content-hash", "Latin title", "日本語", "Artist", "作曲家", 123};
    value.playing = true;
    value.position = 12s;
    value.duration = 120s;
    return value;
}

void unit_tests()
{
    for (const auto& valid : {"12345678901234567", "123456789012345678", "18446744073709551615"}) {
        check(valid_discord_application_id(valid), "valid unsigned snowflake accepted");
    }
    for (const auto& invalid : {"", "123", "012345678901234567", "+123456789012345678", "123456789012345678x",
                                "123456789012345678\n", "18446744073709551616"}) {
        check(!valid_discord_application_id(invalid), "invalid ID rejected before connecting");
    }
    check(discord_activity_json(std::nullopt, {}) == "null", "no playback clears activity");
    auto playback = snapshot();
    const auto time = std::chrono::system_clock::time_point{1000s};
    const auto text = discord_activity_json(playback, time);
    Json activity{json_tokener_parse(text.c_str()), json_object_put};
    check(activity && json_object_get_int(field(activity.get(), "type")) == 2, "Listening activity type");
    check(std::string{json_object_get_string(field(activity.get(), "details"))} == "日本語", "Unicode title preferred");
    check(std::string{json_object_get_string(field(activity.get(), "state"))} == "作曲家", "Unicode artist preferred");
    auto* timestamps = field(activity.get(), "timestamps");
    check(json_object_get_int64(field(timestamps, "start")) == 988 && json_object_get_int64(field(timestamps, "end")) == 1108,
          "timer anchor uses current media position, not session start or listening credit");
    check(text.find("private-") == std::string::npos, "internal IDs/content hashes do not leave the process");
    check(text.find("https:\\/\\/osu.ppy.sh\\/beatmapsets\\/123") != std::string::npos ||
          text.find("https://osu.ppy.sh/beatmapsets/123") != std::string::npos, "button uses official beatmap URL");
    playback.playing = false;
    const auto paused = discord_activity_json(playback, time);
    check(paused.find("timestamps") == std::string::npos && paused.find("Paused") != std::string::npos,
          "pause removes advancing timer");
    playback.playing = true;
    playback.duration.reset();
    playback.track.online_set_id = -1;
    const auto unknown = discord_activity_json(playback, time);
    Json no_duration{json_tokener_parse(unknown.c_str()), json_object_put};
    check(!field(field(no_duration.get(), "timestamps"), "end") && !field(no_duration.get(), "buttons"),
          "unknown duration and invalid online ID omit unsupported fields");
    playback.track.title_unicode.clear();
    for (int index = 0; index < 100; ++index) { playback.track.title_unicode += "日"; }
    playback.track.artist_unicode = "\x1b\n\x7f\xc2\x85";
    const auto bounded = discord_activity_json(playback, time);
    Json limited{json_tokener_parse(bounded.c_str()), json_object_put};
    check(limited && json_object_get_string_len(field(limited.get(), "details")) == 126, "UTF-8 clipping retains codepoint boundaries");
    check(std::string{json_object_get_string(field(limited.get(), "state"))} == "Unknown artist", "controls are not exported");
    playback.position = std::chrono::milliseconds::max();
    check(discord_activity_json(playback, time).find("timestamps") == std::string::npos, "invalid huge positions cannot overflow timestamps");
    std::cout << "all Discord presence rendering tests passed\n";
}

// Line-controlled test harness. Explicit fake socket paths prevent discovery or
// contact with a developer's real Discord client. Production intervals stay unchanged.
void worker_driver(const char* path)
{
    DiscordOptions options;
    options.socket_paths = {path};
    options.update_interval = 200ms;
    options.io_timeout = 300ms;
    options.reconnect_min = 50ms;
    options.reconnect_max = 200ms;
    DiscordPresence discord{options};
    std::optional<PlaybackSnapshot> playback;
    std::cout << "ready\n" << std::flush;
    std::string command;
    while (std::getline(std::cin, command)) {
        PlaybackEvents events;
        if (command == "quit") { break; }
        if (command == "enable") { discord.configure(true, "123456789012345678"); }
        else if (command == "disable") { discord.configure(false, "123456789012345678"); }
        else if (command == "invalid") { discord.configure(true, "not-an-id"); }
        else if (command == "missing") { discord.configure(true, ""); }
        else if (command == "play") { playback = snapshot(); }
        else if (command == "pause") { if (playback) { playback->playing = false; } }
        else if (command == "resume") { if (playback) { playback->playing = true; } }
        else if (command == "seek") {
            if (playback) {
                playback->position = 50s;
                events.events.push_back({PlaybackEventType::seeked, PlaybackEndReason::none, {}, *playback});
            }
        } else if (command == "progress") { if (playback) { playback->position += 1s; } }
        else if (command == "stop") { playback.reset(); }
        else if (command == "overflow") { events.omitted = 5; }
        discord.publish(events, playback);
        std::cout << discord.status() << '\n' << std::flush;
    }
}
}  // namespace

int main(const int argc, char* argv[])
{
    try {
        if (argc == 3 && std::string{argv[1]} == "--worker") { worker_driver(argv[2]); }
        else { unit_tests(); }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
