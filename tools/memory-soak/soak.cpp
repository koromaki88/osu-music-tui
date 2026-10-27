// One-off synthetic investigation, not linked into the application.
#include "osu_player/audio_player.hpp"
#include "osu_player/lastfm_presence.hpp"

#include <atomic>
#include <chrono>
#include <fstream>
#include <iostream>
#include <malloc.h>
#include <thread>
#include <unistd.h>

using namespace osu_player;
using namespace std::chrono_literals;
namespace {
void sample(const std::string& phase, int cycle, const std::atomic<int>& requests)
{
    std::ifstream stat{"/proc/self/statm"};
    std::size_t total{}, resident{}; stat >> total >> resident;
    const auto heap = mallinfo2();
    std::cout << phase << ',' << cycle << ',' << resident * static_cast<std::size_t>(sysconf(_SC_PAGESIZE))
              << ',' << heap.uordblks << ',' << heap.fordblks << ',' << heap.hblkhd << ',' << requests.load() << std::endl;
}
template<class F> void until(F predicate)
{
    const auto deadline = std::chrono::steady_clock::now() + 15s;
    while (!predicate()) {
        if (std::chrono::steady_clock::now() > deadline) { throw std::runtime_error{"worker did not settle"}; }
        std::this_thread::sleep_for(1ms);
    }
}
}
int main(int argc, char** argv)
{
    try {
        if (argc < 5) { return 2; }
        const std::string mode = argv[1];
        const int cycles = std::stoi(argv[2]);
        const std::filesystem::path root = argv[3], audio = argv[4];
        std::atomic<int> requests{};
        std::cout << "phase,cycle,rss_bytes,arena_in_use_bytes,arena_free_bytes,mmap_bytes,requests\n";
        sample("start", 0, requests);
        {
            auto player = make_audio_player();
            std::unique_ptr<LastfmPresence> presence;
            if (mode != "off") {
                const auto path = root / "lastfm/credentials.json";
                save_lastfm_credentials(path, {std::string(32,'a'),std::string(32,'b'),"soak-user",std::string(32,'d')});
                LastfmTransport transport = [&](const auto& params, const auto& cancelled) {
                    LastfmResponse response;
                    if (mode == "https") { response = lastfm_https(params, cancelled); }
                    else if (params.at("method") == "track.updateNowPlaying") {
                        response = {200, R"({"nowplaying":{"ignoredMessage":{"code":"0"}}})"};
                    } else {
                        response = {200, "{\"scrobbles\":{\"@attr\":{\"accepted\":1,\"ignored\":0},\"scrobble\":{\"timestamp\":\"" + params.at("timestamp") + "\",\"ignoredMessage\":{\"code\":\"0\"}}}}"};
                    }
                    ++requests;
                    return response;
                };
                presence = std::make_unique<LastfmPresence>(path, transport, LastfmTiming{1ms,10ms});
                presence->configure(true);
                until([&] { return presence->status().phase == LastfmPhase::connected; });
            }
            for (int i = 1; i <= cycles; ++i) {
                // Alternate three real decoders, pumping a short dummy-output
                // buffer. Eligibility below is synthetic, not earned by this audio.
                player->load(audio / (i%3 == 0 ? "sample.wav" : i%3 == 1 ? "sample.mp3" : "sample.ogg"));
                player->play(); player->update();
                if (presence) {
                    PlaybackSnapshot snapshot;
                    snapshot.session_id = "soak:" + std::to_string(i);
                    snapshot.track.title = "Synthetic title " + std::to_string(i);
                    snapshot.track.artist = "Synthetic artist";
                    snapshot.started_at = std::chrono::system_clock::now() - 120s;
                    snapshot.duration = 120s; snapshot.playing = true;
                    const auto before = requests.load();
                    presence->publish({}, snapshot);
                    until([&] { return requests.load() >= before + 1; });
                    snapshot.listened = 60s; snapshot.listening_eligible = true;
                    presence->publish({{{PlaybackEventType::listening_eligible, PlaybackEndReason::none, {}, snapshot}},0},snapshot);
                    until([&] { return requests.load() >= before + 2 && presence->status().pending == 0; });
                }
                if (i == 1 || i % 100 == 0) { sample("running", i, requests); }
            }
            player->stop();
            std::this_thread::sleep_for(200ms);
            sample("settled", cycles, requests);
        }
        sample("destroyed", cycles, requests);
        // Diagnostic only, never a proposed production fix. Show memory the
        // allocator can return without destroying the process.
        malloc_trim(0);
        sample("trimmed", cycles, requests);
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
