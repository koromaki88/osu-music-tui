#include "osu_player/play_command.hpp"

#include "osu_player/library_loader.hpp"
#include "osu_player/player_controller.hpp"

#include <poll.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cmath>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>

namespace osu_player {
namespace {

void report_errors(PlayerController& player)
{
    const auto errors = player.take_errors();
    for (const auto& error : errors.tracks) {
        std::cerr << "track error [" << error.stable_id << "]: " << error.message << '\n';
    }
    if (errors.omitted != 0) {
        std::cerr << errors.omitted << " earlier track errors omitted\n";
    }
}

void print_status(const PlayerController& player)
{
    if (const Track* track = player.current_track()) {
        std::cout << "track " << *player.current_index() + 1 << ": "
                  << track->display_artist() << " — " << track->display_title() << '\n';
    }
    std::cout << "position_ms: " << player.position().count() << " / ";
    if (player.duration()) {
        std::cout << player.duration()->count();
    } else {
        std::cout << "unknown";
    }
    std::cout << "  volume: " << player.volume()
              << (player.state() == PlayerState::paused ? "  paused" : "")
              << (player.state() == PlayerState::stopped ? "  stopped" : "")
              << "  shuffle: " << (player.shuffle() ? "on" : "off")
              << "  repeat: " << (player.repeat() == RepeatMode::all ? "all" :
                                    player.repeat() == RepeatMode::one ? "one" : "off") << std::endl;
}

bool command(PlayerController& player, const std::string& line)
{
    std::istringstream input{line};
    std::string name;
    input >> name;
    if (name.empty()) {
        return true;
    }
    if (name == "q" || name == "quit") {
        return false;
    }
    if (name == "p" || name == "pause") {
        player.pause();
    } else if (name == "r" || name == "resume") {
        player.play();
    } else if (name == "stop") {
        player.stop();
    } else if (name == "next" || name == "n") {
        player.next();
    } else if (name == "previous" || name == "prev") {
        player.previous();
    } else if (name == "shuffle" || name == "repeat") {
        std::string value;
        std::string extra;
        if (!(input >> value) || (input >> extra)) {
            throw std::invalid_argument{"expected shuffle on|off or repeat off|one|all"};
        }
        if (name == "shuffle" && (value == "on" || value == "off")) {
            player.set_shuffle(value == "on");
        } else if (name == "repeat" && (value == "off" || value == "one" || value == "all")) {
            player.set_repeat(value == "all" ? RepeatMode::all :
                              value == "one" ? RepeatMode::one : RepeatMode::off);
        } else {
            throw std::invalid_argument{"expected shuffle on|off or repeat off|one|all"};
        }
    } else if (name == "track") {
        long long index{};
        std::string extra;
        if (!(input >> index) || (input >> extra) || index < 1 ||
            static_cast<unsigned long long>(index) > player.library().tracks().size()) {
            throw std::invalid_argument{"track requires a valid 1-based library index"};
        }
        player.play_track(static_cast<std::size_t>(index - 1));
    } else if (name == "list") {
        std::string query;
        std::getline(input >> std::ws, query);
        const auto matches = player.library().search(query);
        std::size_t printed = 0;
        for (const std::size_t index : matches) {
            if (printed++ == 20) {
                std::cout << "... more matches omitted; narrow your search\n";
                break;
            }
            const Track& track = player.library().tracks()[index];
            std::cout << index + 1 << ": " << track.display_artist() << " — " << track.display_title() << '\n';
        }
    } else if (name == "seek" || name == "volume") {
        double value{};
        std::string extra;
        if (!(input >> value) || !std::isfinite(value) || (input >> extra)) {
            throw std::invalid_argument{"expected one finite numeric argument"};
        }
        if (name == "seek") {
            // Keep numeric conversions bounded, even when duration is unknown.
            if (value < 0.0 || value > 7 * 24 * 60 * 60) {
                throw std::invalid_argument{"seek seconds must be between 0 and 604800"};
            }
            player.seek(AudioPlayer::Milliseconds{static_cast<std::int64_t>(value * 1000.0)});
        } else {
            player.set_volume(static_cast<float>(value));
        }
    } else if (name != "status") {
        std::cerr << "unknown command; use pause, resume, stop, next, previous, track INDEX, list [SEARCH], "
                     "shuffle on|off, repeat off|one|all, seek SECONDS, volume 0..1, status, quit\n";
    }
    report_errors(player);
    print_status(player);
    return true;
}

int run_session(Library library)
{
    PlayerController player{std::move(library), make_audio_player()};
    if (player.library().tracks().empty()) {
        std::cout << "Queue is empty.\n";
        return 0;
    }
    player.play();
    report_errors(player);
    if (player.state() == PlayerState::failed) {
        std::cerr << "No playable tracks in queue.\n";
        return 1;
    }
    std::cout << "Commands (then Enter): pause, resume, stop, next, previous, track INDEX, list [SEARCH],\n"
                 "shuffle on|off, repeat off|one|all, seek SECONDS, volume 0..1, status, quit\n";
    print_status(player);

    std::string pending;
    bool accepting_input = true;
    auto next_status = std::chrono::steady_clock::now() + std::chrono::seconds{1};
    while (player.state() != PlayerState::finished && player.state() != PlayerState::failed) {
        pollfd descriptor{accepting_input ? STDIN_FILENO : -1, POLLIN, 0};
        const int result = poll(&descriptor, 1, 10);
        if (result < 0 && errno != EINTR) {
            throw std::runtime_error{"could not poll terminal input"};
        }
        if (result > 0 && (descriptor.revents & (POLLERR | POLLNVAL)) != 0) {
            throw std::runtime_error{"terminal input is unavailable"};
        }
        if (result > 0 && (descriptor.revents & (POLLIN | POLLHUP)) != 0) {
            char buffer[1024];
            const ssize_t count = read(STDIN_FILENO, buffer, sizeof(buffer));
            if (count < 0 && errno != EINTR) {
                throw std::runtime_error{"could not read terminal input"};
            }
            if (count == 0) {
                accepting_input = false;  // EOF: allow noninteractive playback to finish.
                if (!pending.empty()) {
                    pending.push_back('\n');
                }
            } else if (count > 0) {
                pending.append(buffer, static_cast<std::size_t>(count));
            }
            std::size_t newline{};
            while ((newline = pending.find('\n')) != std::string::npos) {
                const std::string line = pending.substr(0, newline);
                pending.erase(0, newline + 1);
                try {
                    if (!command(player, line)) {
                        return 0;
                    }
                } catch (const std::invalid_argument& error) {
                    std::cerr << "command error: " << error.what() << '\n';
                }
            }
            if (pending.size() > 4096) {
                pending.clear();
                std::cerr << "command error: input line too long\n";
            }
        }
        const auto previous_track = player.current_index();
        player.update();
        report_errors(player);
        const auto now = std::chrono::steady_clock::now();
        if (player.current_index() != previous_track || now >= next_status) {
            print_status(player);
            next_status = now + std::chrono::seconds{1};
        }
    }
    if (player.state() == PlayerState::failed) {
        std::cerr << "No playable tracks remain in queue.\n";
        return 1;
    }
    std::cout << "Playback finished.\n";
    return 0;
}

}  // namespace

int run_play_command(const std::filesystem::path& path)
{
    Track track;
    track.stable_id = path.filename().string();
    track.beatmap_set_id = "single";
    track.title = path.filename().string();
    track.artist = "Local audio";
    track.audio_path = path;
    return run_session(Library{std::vector<Track>{std::move(track)}});
}

int run_queue_command(const std::filesystem::path& library_file)
{
    std::ifstream input{library_file};
    if (!input) {
        throw std::runtime_error{"could not open library file: " + library_file.string()};
    }
    auto loaded = load_library_ndjson(input);
    const std::size_t warning_limit = 32;
    for (std::size_t index = 0; index < loaded.warnings.size() && index < warning_limit; ++index) {
        std::cerr << "library warning: " << loaded.warnings[index] << '\n';
    }
    if (loaded.warnings.size() > warning_limit) {
        std::cerr << loaded.warnings.size() - warning_limit << " more library warnings omitted\n";
    }
    return run_session(std::move(loaded.library));
}

}  // namespace osu_player
