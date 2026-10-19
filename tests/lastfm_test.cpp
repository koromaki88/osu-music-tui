#include "osu_player/lastfm_presence.hpp"

#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {
using namespace osu_player;
using namespace std::chrono_literals;
void check(const bool value, const char* message) { if (!value) { throw std::runtime_error{message}; } }
template<class Function> void rejects(Function function, const char* message)
{
    try { function(); } catch (const std::exception&) { return; }
    throw std::runtime_error{message};
}
template<class Function> void until(Function condition)
{
    const auto deadline = std::chrono::steady_clock::now() + 3s;
    while (!condition()) {
        if (std::chrono::steady_clock::now() >= deadline) { throw std::runtime_error{"timed out waiting for fake Last.fm worker"}; }
        std::this_thread::sleep_for(2ms);
    }
}
struct Temporary {
    std::filesystem::path root;
    Temporary()
    {
        std::string pattern = std::filesystem::exists("/tmp/opencode") ? "/tmp/opencode/lastfm-XXXXXX" : "/tmp/lastfm-XXXXXX";
        std::vector<char> name(pattern.begin(), pattern.end()); name.push_back('\0');
        const auto result = mkdtemp(name.data());
        if (!result) { throw std::runtime_error{"cannot create test fixture"}; }
        root = result;
    }
    ~Temporary() { std::error_code error; std::filesystem::remove_all(root, error); }
    std::filesystem::path path() const { return root / "lastfm/credentials.json"; }
};
LastfmCredentials application() { return {std::string(32, 'a'), std::string(32, 'b'), {}, {}}; }
LastfmCredentials account() { auto value = application(); value.username = "test-user"; value.session_key = std::string(32, 'd'); return value; }
PlaybackSnapshot playback(std::string id = "first")
{
    PlaybackSnapshot value;
    value.session_id = std::move(id); value.playing = true; value.duration = 120s;
    value.track = {"local-library-id", "local-audio-hash", "Original title", "Unicode title", "Original artist", "Unicode artist", 123};
    return value;
}
struct Fake {
    std::mutex mutex;
    std::vector<LastfmParameters> calls;
    std::atomic<int> token_error{};
    std::atomic<int> session_error{};
    std::atomic<int> now_error{};
    std::atomic<bool> malformed{};
    std::atomic<int> curl_error{};
    std::atomic<long> http_status{200};
    std::atomic<bool> ignored{};
    std::atomic<bool> blocked{};
    std::atomic<bool> entered{};
    std::atomic<bool> cancelled{};
    // Set before starting a worker; never mutated while transport is running.
    std::string response_override;
    LastfmResponse send(const LastfmParameters& parameters, const LastfmCancelled& cancel)
    {
        {
            const std::lock_guard lock{mutex}; calls.push_back(parameters);
        }
        entered = true;
        if (curl_error) { throw LastfmFailure{LastfmFailureKind::https_transfer, curl_error}; }
        if (blocked) {
            until([&] { return cancel(); }); cancelled = true;
            throw std::runtime_error{"DO NOT LOG SECRET BODY URL"};
        }
        check(parameters.at("api_sig") == lastfm_signature(parameters, application().secret), "signature matches request fields");
        check(parameters.at("format") == "json" && !parameters.contains("secret"), "secret is never transmitted");
        if (!response_override.empty()) { return {200, response_override}; }
        const auto method = parameters.at("method");
        int error{};
        if (method == "auth.getToken") { error = token_error; }
        else if (method == "auth.getSession") { error = session_error; }
        else if (method == "track.updateNowPlaying") { error = now_error; }
        else { throw std::runtime_error{"unexpected method: scrobble submissions are not implemented"}; }
        if (error) { return {200, "{\"error\":" + std::to_string(error) + ",\"message\":\"DO NOT LOG SECRET\"}"}; }
        if (malformed) { return {http_status, "<invalid>DO NOT LOG SECRET"}; }
        if (method == "auth.getToken") { return {200, "{\"token\":\"" + std::string(32, 'c') + "\"}"}; }
        if (method == "auth.getSession") {
            return {200, "{\"session\":{\"name\":\"test-user\",\"key\":\"" + std::string(32, 'd') + "\",\"subscriber\":\"0\"}}"};
        }
        return {200, "{\"nowplaying\":{\"artist\":{\"#text\":\"Unapproved correction\",\"corrected\":\"1\"},\"ignoredMessage\":{\"code\":\"" + std::string(ignored ? "1" : "0") + "\",\"#text\":\"DO NOT LOG SECRET\"}}}"};
    }
    LastfmTransport transport() { return [this](const auto& params, const auto& cancel) { return send(params, cancel); }; }
    std::size_t count(const std::string& method)
    {
        const std::lock_guard lock{mutex}; std::size_t result{};
        for (const auto& call : calls) { if (call.at("method") == method) { ++result; } }
        return result;
    }
};
void protocol_tests()
{
    check(std::string{lastfm_api_endpoint} == "https://ws.audioscrobbler.com/2.0/?format=json", "JSON response format is requested in the endpoint query, with no credentials");
    check(valid_lastfm_key(std::string(32, 'a')) && !valid_lastfm_key(std::string(32, 'z')), "hex key validation");
    check(valid_lastfm_token("opaque-Token_GZ.123~") && !valid_lastfm_token("") &&
          !valid_lastfm_token(std::string(257, 'a')) && !valid_lastfm_token("token&injected=true") &&
          !valid_lastfm_token("token\n"), "server-issued tokens are bounded opaque values, not assumed API keys");
    LastfmParameters parameters{{"api_key", "xxxxxxxx"}, {"method", "auth.getSession"}, {"token", "xxxxxxx"}};
    check(lastfm_signature(parameters, "mysecret") == "68afb32bee072407a63b6c41f3e1e2b4", "independent desktop auth signing vector");
    parameters["format"] = "json"; parameters["callback"] = "ignored"; parameters["api_sig"] = "ignored";
    check(lastfm_signature(parameters, "mysecret") == "68afb32bee072407a63b6c41f3e1e2b4", "non-signed fields excluded");
    check(lastfm_signature({{"api_key", "key"}, {"artist", "Björk"}, {"method", "track.updateNowPlaying"},
                           {"sk", "session"}, {"track", "Jóga"}}, "secret") == "76eb3706f6a33943282d19ed41c0518e", "UTF-8 signature before URL encoding");
    check(lastfm_form({{"artist", "Björk & +"}, {"track", "a=b/?"}}) == "artist=Bj%C3%B6rk%20%26%20%2B&track=a%3Db%2F%3F", "form escaping is UTF-8 and delimiter-safe");
    auto value = playback();
    const auto result = lastfm_now_playing(value);
    check(result && result->at("artist") == "Original artist" && result->at("track") == "Original title" &&
          result->at("duration") == "120" && result->size() == 4, "structured original metadata only; no paths/hash/album invention");
    value.track.title.clear(); value.track.artist.clear(); value.duration.reset();
    const auto fallback = lastfm_now_playing(value);
    check(fallback && fallback->at("artist") == "Unicode artist" && !fallback->contains("duration"), "fallback only for absent original fields");
    value.track.title_unicode = "\x1b[2J";
    check(!lastfm_now_playing(value), "control metadata rejected, not silently corrected");
    value.track.title_unicode = "\xff";
    check(!lastfm_now_playing(value), "invalid UTF-8 rejected");
    value = playback(); value.playing = false;
    check(!lastfm_now_playing(value), "no Now Playing while paused");
    bool cancelled = true;
    rejects([&] { (void)lastfm_https({}, [&] { return cancelled; }); }, "cancelled HTTPS must not connect");
}
void storage_tests()
{
    Temporary temporary;
    const auto path = temporary.path();
    check(load_lastfm_credentials(path) == LastfmCredentials{}, "missing credentials default empty");
    save_lastfm_credentials(path, account());
    check(load_lastfm_credentials(path) == account(), "private credentials round-trip");
    auto opaque = account(); opaque.session_key = "Opaque-session-GZ_123";
    save_lastfm_credentials(path, opaque);
    check(load_lastfm_credentials(path) == opaque, "opaque nonhex session keys survive private storage");
    save_lastfm_credentials(path, account());
    struct stat info{};
    check(stat(path.c_str(), &info) == 0 && (info.st_mode & 0777) == 0600, "credential file is private 0600");
    check(stat(path.parent_path().c_str(), &info) == 0 && (info.st_mode & 0777) == 0700, "credential directory is private 0700");
    chmod(path.c_str(), 0644);
    rejects([&] { (void)load_lastfm_credentials(path); }, "read must refuse exposed credentials");
    rejects([&] { save_lastfm_credentials(path, application()); }, "write must not overwrite unsafe file");
    chmod(path.c_str(), 0600);
    const auto original = temporary.root / "original";
    std::filesystem::rename(path, original); std::filesystem::create_symlink(original, path);
    rejects([&] { (void)load_lastfm_credentials(path); }, "symlink credentials rejected");
    rejects([&] { save_lastfm_credentials(path, application()); }, "symlink writes rejected");
    std::filesystem::remove(path); std::filesystem::rename(original, path);
    { std::ofstream file{path}; file << "{\"version\":99}"; }
    rejects([&] { save_lastfm_credentials(path, application()); }, "newer/malformed file preserved");
    { std::ifstream file{path}; std::string text; std::getline(file, text); check(text == "{\"version\":99}", "file unchanged after error"); }
    std::filesystem::remove(path);
    check(mkfifo(path.c_str(), 0600) == 0, "create FIFO fixture");
    rejects([&] { (void)load_lastfm_credentials(path); }, "FIFO cannot hang credential reads");
}
void worker_tests()
{
    Temporary temporary;
    Fake fake;
    {
        LastfmPresence presence{temporary.path(), fake.transport()};
        presence.publish({}, playback());
        check(fake.count("auth.getToken") == 0, "disabled by default");
        presence.configure(true);
        until([&] { return presence.status().phase == LastfmPhase::needs_credentials; });
        presence.set_credentials(application().api_key, application().secret);
        until([&] { return presence.status().phase == LastfmPhase::awaiting_authorization; });
        check(presence.status().authorization_url == "https://www.last.fm/api/auth/?api_key=" + application().api_key +
              "&token=" + std::string(32, 'c'), "only official HTTPS authorization URL constructed");
        check(load_lastfm_credentials(temporary.path()) == application(), "token never persisted before browser approval");
        fake.session_error = 14; presence.complete_authorization();
        until([&] { return presence.status().phase == LastfmPhase::awaiting_authorization; });
        check(presence.status().message.find("DO NOT LOG") == std::string::npos, "server text redacted");
        fake.session_error = 0; presence.complete_authorization();
        until([&] { return presence.status().message == "connected (Now Playing sent)"; });
        check(load_lastfm_credentials(temporary.path()) == account(), "account-associated session persisted");
        check(fake.count("track.updateNowPlaying") == 1, "current playing track sent after authorization");
        auto latest = playback();
        for (int index = 0; index < 100; ++index) { latest.position += 1s; presence.publish({}, latest); }
        latest.playing = false; presence.publish({}, latest);
        latest.playing = true; presence.publish(PlaybackEvents{{}, 12}, latest);
        std::this_thread::sleep_for(40ms);
        check(fake.count("track.updateNowPlaying") == 1, "no retries/spam on progress, resume, seek or overflow");
        fake.ignored = true; presence.publish({}, playback("second"));
        until([&] { return presence.status().message == "connected (Now Playing ignored)"; });
        fake.ignored = false; fake.now_error = 16; presence.publish({}, playback("third"));
        until([&] { return presence.status().message.find("not retried") != std::string::npos; });
        std::this_thread::sleep_for(40ms);
        check(fake.count("track.updateNowPlaying") == 3, "failed/ignored Now Playing not retried");
        fake.now_error = 9; presence.publish({}, playback("fourth"));
        until([&] { return presence.status().phase == LastfmPhase::needs_authorization; });
        check(load_lastfm_credentials(temporary.path()).session_key.empty(), "revoked session removed");
        presence.authorize(); until([&] { return presence.status().phase == LastfmPhase::awaiting_authorization; });
        fake.session_error = 15; presence.complete_authorization();
        until([&] { return presence.status().phase == LastfmPhase::needs_authorization; });
        presence.disconnect(); until([&] { return presence.status().phase == LastfmPhase::off; });
    }
    save_lastfm_credentials(temporary.path(), account());
    fake.now_error = 0;
    {
        LastfmPresence restored{temporary.path(), fake.transport()};
        auto latest = playback("restart"); latest.playing = false;
        restored.publish({}, latest); restored.configure(true);
        until([&] { return restored.status().phase == LastfmPhase::connected; });
        const auto before = fake.count("track.updateNowPlaying");
        std::this_thread::sleep_for(30ms);
        check(fake.count("track.updateNowPlaying") == before, "no stale Now Playing for paused startup");
        latest.playing = true; restored.publish({}, latest);
        until([&] { return fake.count("track.updateNowPlaying") == before + 1; });
        restored.disconnect(); until([&] { return restored.status().phase == LastfmPhase::off; });
        check(load_lastfm_credentials(temporary.path()).session_key.empty(), "disconnect clears stored session, retains API credentials");
    }
}
void cancellation_tests()
{
    Temporary temporary; save_lastfm_credentials(temporary.path(), account());
    Fake fake; fake.blocked = true;
    {
        LastfmPresence presence{temporary.path(), fake.transport()}; presence.configure(true);
        until([&] { return presence.status().phase == LastfmPhase::connected; });
        presence.publish({}, playback()); until([&] { return fake.entered.load(); });
        const auto start = std::chrono::steady_clock::now();
        presence.configure(false);
        check(std::chrono::steady_clock::now() - start < 100ms, "disable does not wait for network");
        until([&] { return fake.cancelled.load(); });
        check(presence.status().phase == LastfmPhase::off, "late replies cannot restore disabled state");
    }
    fake.entered = false; fake.cancelled = false;
    const auto start = std::chrono::steady_clock::now();
    {
        LastfmPresence presence{temporary.path(), fake.transport()}; presence.configure(true);
        until([&] { return presence.status().phase == LastfmPhase::connected; });
        presence.publish({}, playback()); until([&] { return fake.entered.load(); });
    }
    check(fake.cancelled && std::chrono::steady_clock::now() - start < 1s, "destruction cancels and joins owned worker");
}
void response_failure_tests()
{
    Temporary temporary; save_lastfm_credentials(temporary.path(), application());
    Fake fake; fake.malformed = true;
    LastfmPresence presence{temporary.path(), fake.transport()}; presence.configure(true);
    until([&] { return presence.status().phase == LastfmPhase::needs_authorization; });
    presence.authorize();
    until([&] { return presence.status().phase == LastfmPhase::error; });
    check(presence.status().message == "Response: invalid JSON (expected JSON, not XML/HTML)", "parse failures identified separately");
    check(presence.status().message.find("DO NOT LOG") == std::string::npos, "malformed response diagnostics redacted");
    fake.http_status = 503; presence.authorize();
    until([&] { return presence.status().phase == LastfmPhase::error; });
    check(presence.status().message == "HTTPS: unexpected HTTP status 503", "non-JSON HTTP failures identified");
    fake.http_status = 200;
    for (const int code : {6, 28, 60}) {
        fake.curl_error = code; presence.authorize();
        until([&] { return presence.status().phase == LastfmPhase::error; });
        check(presence.status().message == "HTTPS: transfer failed (curl code " + std::to_string(code) + ")", "DNS, timeout and TLS failures have safe diagnostics");
    }
    fake.curl_error = 0;
    fake.malformed = false; fake.token_error = 13; presence.authorize();
    until([&] { return presence.status().phase == LastfmPhase::error; });
    check(presence.status().message == "Last.fm error 13: invalid signature; check matching key/secret with c", "bad signatures require credential repair");
    for (const int code : {4, 10, 26, 29}) {
        fake.token_error = code; presence.authorize();
        until([&] { return presence.status().phase == LastfmPhase::error; });
        check(presence.status().message.starts_with("Last.fm error " + std::to_string(code) + ":"), "safe numeric API failure is visible");
        check(presence.status().message.find("DO NOT LOG") == std::string::npos, "API diagnostics never echo server text");
    }
    fake.token_error = 0; presence.authorize();
    until([&] { return presence.status().phase == LastfmPhase::awaiting_authorization; });
    check(load_lastfm_credentials(temporary.path()) == application(), "failed responses cannot persist a session");
    presence.configure(false);
    chmod(temporary.path().c_str(), 0644);
    presence.configure(true);
    until([&] { return presence.status().phase == LastfmPhase::error; });
    check(presence.status().message.starts_with("Credentials: unsafe owner/type/mode"), "unsafe credential mode is not reported as network/auth failure");
}
void token_response_tests()
{
    const std::vector<std::pair<std::string, std::string>> failures{
        {"<?xml version=\"1.0\"?><lfm status=\"ok\"><token>DO NOT LOG SECRET</token></lfm>", "Response: invalid JSON (expected JSON, not XML/HTML)"},
        {"[]", "Response: JSON root is not an object"},
        {"{}", "Response: auth.getToken missing string token"},
        {"{\"token\":123}", "Response: auth.getToken missing string token"},
        {"{\"token\":\"unsafe&token=DO NOT LOG SECRET\"}", "Response: auth.getToken token has unsafe length/characters"},
    };
    for (const auto& [body, message] : failures) {
        Temporary temporary; save_lastfm_credentials(temporary.path(), application());
        Fake fake; fake.response_override = body;
        LastfmPresence presence{temporary.path(), fake.transport()}; presence.configure(true);
        until([&] { return presence.status().phase == LastfmPhase::needs_authorization; });
        presence.authorize(); until([&] { return presence.status().phase == LastfmPhase::error; });
        check(presence.status().message == message, "token response failures distinguish parsing from missing/unsafe tokens without leaking values");
    }
    Temporary temporary; save_lastfm_credentials(temporary.path(), application());
    Fake fake; fake.response_override = "{\"token\":\"Opaque-token-GZ_123\"}";
    LastfmPresence presence{temporary.path(), fake.transport()}; presence.configure(true);
    until([&] { return presence.status().phase == LastfmPhase::needs_authorization; });
    presence.authorize(); until([&] { return presence.status().phase == LastfmPhase::awaiting_authorization; });
    check(presence.status().authorization_url.ends_with("&token=Opaque-token-GZ_123"), "nonhex server token is accepted for browser authorization");
}
}  // namespace
int main()
{
    try {
        protocol_tests(); storage_tests(); worker_tests(); cancellation_tests(); response_failure_tests(); token_response_tests();
        std::cout << "Last.fm protocol, private storage, fake authorization/Now Playing, revocation and cancellation tests passed (no service requests)\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << "Last.fm test failure: " << error.what() << '\n'; return 1; }
}
