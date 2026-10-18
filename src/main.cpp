#include "osu_player/audio_probe.hpp"
#include "osu_player/library_loader.hpp"
#include "osu_player/play_command.hpp"
#include "osu_player/storage_path.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

constexpr std::string_view usage =
    "usage: osu-player probe HASH [OSU_DATA_DIRECTORY]\n"
    "       osu-player play HASH [OSU_DATA_DIRECTORY]\n"
    "       osu-player queue NDJSON_FILE\n"
    "       osu-player library NDJSON_FILE|- [SEARCH]\n"
    "\n"
    "Commands:\n"
    "  probe    Resolve a lazer content hash and inspect its audio stream.\n"
    "  play     Play hashed audio with line-based transport controls.\n"
    "  queue    Play an indexed library with queue, shuffle, and repeat controls.\n"
    "  library  Validate and print a normalized NDJSON music library.\n";

int run_probe(const int argc, char* argv[])
{
    if (argc != 3 && argc != 4) {
        std::cerr << usage;
        return 2;
    }

    const std::filesystem::path osu_directory =
        argc == 4 ? std::filesystem::path{argv[3]} : osu_player::default_osu_data_directory();
    const std::filesystem::path path = osu_player::content_path(osu_directory, argv[2]);

    if (!std::filesystem::is_regular_file(path)) {
        throw std::runtime_error{"content file does not exist: " + path.string()};
    }

    const osu_player::AudioMetadata audio = osu_player::probe_audio(path);

    std::cout << "path: " << path << '\n'
              << "container: " << audio.container << '\n'
              << "codec: " << audio.codec << '\n'
              << "duration_ms: " << audio.duration_ms << '\n'
              << "sample_rate: " << audio.sample_rate << '\n'
              << "channels: " << audio.channels << '\n';

    return 0;
}

int run_library(const int argc, char* argv[])
{
    if (argc != 3 && argc != 4) {
        std::cerr << usage;
        return 2;
    }

    std::ifstream file;
    std::istream* input = &std::cin;
    if (std::string_view{argv[2]} != "-") {
        file.open(argv[2]);
        if (!file) {
            throw std::runtime_error{"could not open library file: " + std::string{argv[2]}};
        }
        input = &file;
    }

    const osu_player::LibraryLoadResult loaded = osu_player::load_library_ndjson(*input);
    const std::string_view query = argc == 4 ? std::string_view{argv[3]} : std::string_view{};
    const std::vector<std::size_t> matches = loaded.library.search(query);

    std::cout << "schema_version: " << loaded.schema_version << '\n'
              << "lazer_version: "
              << (loaded.lazer_version.empty() ? "unknown" : loaded.lazer_version) << '\n'
              << "tracks: " << loaded.library.tracks().size() << '\n'
              << "matches: " << matches.size() << '\n';

    for (const std::string& warning : loaded.warnings) {
        std::cerr << "warning: " << warning << '\n';
    }

    for (const std::size_t index : matches) {
        const osu_player::Track& track = loaded.library.tracks()[index];
        std::cout << track.display_artist() << " — " << track.display_title() << '\n';
    }

    return 0;
}

int run(const int argc, char* argv[])
{
    if (argc < 2) {
        std::cerr << usage;
        return 2;
    }

    const std::string_view command{argv[1]};
    if (command == "probe") {
        return run_probe(argc, argv);
    }
    if (command == "library") {
        return run_library(argc, argv);
    }
    if (command == "play") {
        if (argc != 3 && argc != 4) {
            std::cerr << usage;
            return 2;
        }
        const std::filesystem::path directory = argc == 4
            ? std::filesystem::path{argv[3]} : osu_player::default_osu_data_directory();
        return osu_player::run_play_command(osu_player::content_path(directory, argv[2]));
    }
    if (command == "queue") {
        if (argc != 3 || std::string_view{argv[2]} == "-") {
            std::cerr << "queue requires a library file; stdin is reserved for transport commands\n" << usage;
            return 2;
        }
        return osu_player::run_queue_command(argv[2]);
    }

    std::cerr << "error: unknown command '" << command << "'\n\n" << usage;
    return 2;
}

}  // namespace

int main(const int argc, char* argv[])
{
    try {
        return run(argc, argv);
    } catch (const std::exception& error) {
        std::cerr << "error: " << error.what() << '\n';
        return 1;
    }
}
