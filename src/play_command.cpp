#include "osu_player/play_command.hpp"

#include "osu_player/audio_player.hpp"

#include <poll.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cmath>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>

namespace osu_player {
namespace {

void print_status(const AudioPlayer& audio)
{
    std::cout << "position_ms: " << audio.position().count() << " / ";
    if (audio.duration()) {
        std::cout << audio.duration()->count();
    } else {
        std::cout << "unknown";
    }
    std::cout << "  volume: " << audio.volume()
              << (audio.state() == PlaybackState::paused ? "  paused" : "") << std::endl;
}

bool command(AudioPlayer& audio, const std::string& line)
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
        audio.pause();
    } else if (name == "r" || name == "resume") {
        audio.play();
    } else if (name == "stop") {
        audio.stop();
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
            audio.seek(AudioPlayer::Milliseconds{static_cast<std::int64_t>(value * 1000.0)});
        } else {
            audio.set_volume(static_cast<float>(value));
        }
    } else if (name != "status") {
        std::cerr << "unknown command; use pause, resume, stop, seek SECONDS, volume 0..1, status, quit\n";
    }
    print_status(audio);
    return true;
}

}  // namespace

int run_play_command(const std::filesystem::path& path)
{
    auto audio = make_audio_player();
    audio->load(path);
    audio->play();
    std::cout << "Playing " << path << '\n'
              << "Commands (then Enter): pause, resume, stop, seek SECONDS, volume 0..1, status, quit\n";
    print_status(*audio);

    std::string pending;
    bool accepting_input = true;
    auto next_status = std::chrono::steady_clock::now() + std::chrono::seconds{1};
    while (audio->state() != PlaybackState::finished) {
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
                    if (!command(*audio, line)) {
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
        audio->update();
        const auto now = std::chrono::steady_clock::now();
        if (now >= next_status) {
            print_status(*audio);
            next_status = now + std::chrono::seconds{1};
        }
    }
    std::cout << "Playback finished.\n";
    return 0;
}

}  // namespace osu_player
