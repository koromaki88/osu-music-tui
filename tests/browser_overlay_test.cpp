#include "osu_player/browser_overlay.hpp"
#include "../src/overlay_config.hpp"

#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#include <sys/stat.h>

#include <chrono>
#include <iostream>
#include <fstream>
#include <stdexcept>
#include <thread>

using namespace osu_player;
using namespace std::chrono_literals;
namespace {
void check(const bool value, const char* message) { if (!value) { throw std::runtime_error{message}; } }
struct Temporary {
    std::filesystem::path root;
    Temporary()
    {
        auto pattern = (std::filesystem::is_directory("/tmp/opencode") ? std::string{"/tmp/opencode/"} : std::string{"/tmp/"}) + "overlay-unit-XXXXXX";
        const auto path = mkdtemp(pattern.data());
        check(path != nullptr, "temporary directory"); root = path;
    }
    ~Temporary() { std::filesystem::remove_all(root); }
};
void style_tests(const std::filesystem::path& root)
{
    using namespace overlay_detail;
    check(parse_style("{}") == default_style(), "missing style fields use defaults");
    check(parse_style("{\"width\":600,\"show_time\":false,\"background_opacity\":0}").find("\"width\":600") != std::string::npos, "valid partial style overrides");
    for (const auto& bad : {"", "[]", "null", "{bad", "{} trailing", "{\"width\":-1}", "{\"width\":9000}", "{\"width\":600.5}",
                           "{\"height\":null}", "{\"show_time\":1}", "{\"artist_size\":0}", "{\"font\":\"url(https://evil)\"}",
                           "{\"font\":\"</script>\"}", "{\"font\":\"   \"}", "{\"background\":\"red; color:black\"}",
                           "{\"accent_color\":\"#gg0000\"}", "{\"background_opacity\":1.1}", "{\"corner_radius\":999}", "{\"typo\":1}",
                           "{\"show_thumbnail\":1}", "{\"thumbnail_size\":31}", "{\"thumbnail_size\":257}"}) {
        bool rejected{};
        try { (void)parse_style(bad); } catch (const std::exception&) { rejected = true; }
        check(rejected, "invalid style must be rejected transactionally");
    }
    const auto path = root / "styles/overlay.json";
    create_style_if_missing(path);
    check(load_style(path) == default_style(), "generated defaults load");
    struct stat info{};
    check(stat(path.c_str(), &info) == 0 && (info.st_mode & 0777) == 0600, "private generated config");
    { std::ofstream out{path}; out << "broken user edits"; }
    create_style_if_missing(path);
    { std::ifstream in{path}; std::string text; std::getline(in, text); check(text == "broken user edits", "never replace invalid user config"); }
    std::filesystem::remove(path);
    check(load_style(path) == default_style(), "missing file restores defaults");
    for (const bool fifo : {false, true}) {
        if (fifo) { check(mkfifo(path.c_str(), 0600) == 0, "FIFO fixture"); }
        else { std::filesystem::create_symlink(root / "absent", path); }
        bool rejected{};
        try { (void)load_style(path); } catch (const std::exception&) { rejected = true; }
        check(rejected, "nonregular style files rejected without blocking");
        std::filesystem::remove(path);
    }
    { std::ofstream out{path}; out << std::string(8193, 'x'); }
    bool rejected{};
    try { (void)load_style(path); } catch (const std::exception&) { rejected = true; }
    check(rejected, "oversized config rejected");
}
template<class Function> void until(Function predicate)
{
    const auto deadline = std::chrono::steady_clock::now() + 4s;
    while (!predicate()) {
        check(std::chrono::steady_clock::now() < deadline, "overlay condition timeout (port 7270 must be free)");
        std::this_thread::sleep_for(10ms);
    }
}
std::string body()
{
    const int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    check(fd >= 0, "test socket");
    struct Guard { int value; ~Guard() { close(value); } } guard{fd};
    timeval timeout{2, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    sockaddr_in address{};
    address.sin_family = AF_INET; address.sin_port = htons(7270); address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    check(connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0, "test connect");
    const std::string request{"GET /state HTTP/1.1\r\nHost: 127.0.0.1:7270\r\n\r\n"};
    check(send(fd, request.data(), request.size(), MSG_NOSIGNAL) == static_cast<ssize_t>(request.size()), "test send");
    std::string result;
    char buffer[1024];
    for (;;) {
        const auto bytes = recv(fd, buffer, sizeof(buffer), 0);
        if (bytes == 0) { break; }
        check(bytes > 0, "test receive deadline");
        result.append(buffer, static_cast<std::size_t>(bytes));
    }
    check(result.starts_with("HTTP/1.1 200"), "state response");
    return result.substr(result.find("\r\n\r\n") + 4);
}
}
int main()
{
    try {
        Temporary temp;
        style_tests(temp.root);
        BrowserOverlay overlay{temp.root / "server/overlay.json"};
        check(overlay.status() == "off", "default off");
        PlaybackSnapshot snapshot;
        snapshot.track.title = "Original"; snapshot.track.title_unicode = "日本";
        snapshot.track.artist = "Artist"; snapshot.track.artist_unicode = "音楽";
        snapshot.track.stable_id = "PRIVATE-ID"; snapshot.track.audio_hash = "PRIVATE-HASH";
        snapshot.session_id = "PRIVATE-SESSION";
        snapshot.position = -100ms;
        overlay.publish(snapshot);
        overlay.configure(true);
        until([&] { return overlay.status().starts_with("http://"); });
        auto state = body();
        check(state.find("日本") != std::string::npos && state.find("音楽") != std::string::npos, "Unicode display preference");
        check(state.find("PRIVATE") == std::string::npos && state.find("Original") == std::string::npos, "display-only whitelist");
        check(state.find("\"thumbnail_url\":null") != std::string::npos, "local-only tracks have no thumbnail");
        snapshot.track.online_set_id = 123;
        overlay.publish(snapshot);
        until([&] { return body().find("assets.ppy.sh") != std::string::npos; });
        snapshot.track.online_set_id = -1;
        overlay.publish(snapshot);
        until([&] { return body().find("\"thumbnail_url\":null") != std::string::npos; });
        check(state.find("\"position_ms\":0") != std::string::npos && state.find("\"duration_ms\":null") != std::string::npos, "unknown duration and nonnegative position");
        snapshot.track.title_unicode = std::string(4095, 'x') + "日本";
        snapshot.playing = true; snapshot.duration = 100s; snapshot.position = 25s;
        overlay.publish(snapshot);
        until([&] { return body().find("25000") != std::string::npos; });
        state = body();
        check(state.find(std::string(4095, 'x')) != std::string::npos && state.find("日本") == std::string::npos, "bounded title truncates at UTF-8 boundary");
        check(state.size() < 8500, "bounded display JSON");
        overlay.publish(std::nullopt);
        until([&] { return body().find("\"state\":\"idle\"") != std::string::npos; });
        for (int i = 0; i < 20; ++i) { overlay.configure(false); overlay.configure(true); }
        until([&] { return overlay.status().starts_with("http://"); });
        check(body().find("\"title\":\"\"") != std::string::npos, "rapid configuration retains cleared snapshot");
        overlay.configure(false);
        check(overlay.status() == "off", "disabled status");
        std::cout << "Browser overlay snapshot, bounds, Unicode, privacy and worker lifecycle tests passed\n";
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
