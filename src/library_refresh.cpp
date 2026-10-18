#include "osu_player/library_refresh.hpp"

#include <json-c/json.h>
#include <fcntl.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <utility>

extern char** environ;

namespace osu_player {
namespace {

using Path = std::filesystem::path;
using Json = std::unique_ptr<json_object, decltype(&json_object_put)>;

[[noreturn]] void system_error(const std::string& operation)
{
    throw std::runtime_error{operation + ": " + std::strerror(errno)};
}

bool within(const Path& path, const Path& directory)
{
    const auto parent = std::filesystem::weakly_canonical(directory);
    const auto child = std::filesystem::weakly_canonical(path);
    return std::mismatch(parent.begin(), parent.end(), child.begin(), child.end()).first == parent.end();
}

void require_outside(const Path& path, const Path& osu_directory)
{
    if (within(path, osu_directory)) {
        throw std::runtime_error{"cache/temp directory must be outside the osu! data directory"};
    }
}

std::string fingerprint(const Path& file)
{
    struct stat info{};
    if (::stat(file.c_str(), &info) != 0) { system_error("cannot stat " + file.string()); }
    if (!S_ISREG(info.st_mode)) { throw std::runtime_error{"not a regular file: " + file.string()}; }
    return std::to_string(info.st_dev) + ":" + std::to_string(info.st_ino) + ":" +
        std::to_string(info.st_size) + ":" + std::to_string(info.st_mtim.tv_sec) + ":" +
        std::to_string(info.st_mtim.tv_nsec) + ":" + std::to_string(info.st_ctim.tv_sec) + ":" +
        std::to_string(info.st_ctim.tv_nsec);
}

Path cache_path(const Path& source)
{
    Path base;
    if (const char* xdg = std::getenv("XDG_CACHE_HOME"); xdg && Path{xdg}.is_absolute()) { base = xdg; }
    else if (const char* home = std::getenv("HOME"); home && Path{home}.is_absolute()) {
        base = Path{home} / ".cache";
    } else { throw std::runtime_error{"set an absolute XDG_CACHE_HOME or HOME for caching"}; }
    // This is only a filename bucket, not an integrity/security hash. The full
    // canonical source path is also checked in the header (including collisions).
    std::uint64_t hash = 14695981039346656037ULL;
    for (const char byte : source.string()) {
        hash ^= static_cast<unsigned char>(byte);
        hash *= 1099511628211ULL;
    }
    std::ostringstream name;
    name << std::hex << std::setw(16) << std::setfill('0') << hash << ".ndjson";
    const auto path = base / "osu-music-player" / name.str();
    require_outside(path.parent_path(), source);
    return path;
}

class TempDirectory {
public:
    explicit TempDirectory(const Path& source)
    {
        auto pattern = (std::filesystem::temp_directory_path() / "osu-player-index-XXXXXX").string();
        require_outside(Path{pattern}.parent_path(), source);
        const char* created = ::mkdtemp(pattern.data());
        if (!created) { system_error("create index staging directory"); }
        path = created;
    }
    ~TempDirectory()
    {
        std::error_code ignored;
        // Exactly this freshly created private tree, including any adapter Realm
        // snapshots left by forced cancellation. Never a source/cache directory.
        std::filesystem::remove_all(path, ignored);
    }
    Path path;
};

struct SpawnSetup {
    SpawnSetup()
    {
        const int actions_error = posix_spawn_file_actions_init(&actions);
        if (actions_error) { throw std::runtime_error{std::strerror(actions_error)}; }
        const int attributes_error = posix_spawnattr_init(&attributes);
        if (attributes_error) {
            posix_spawn_file_actions_destroy(&actions);
            throw std::runtime_error{std::strerror(attributes_error)};
        }
    }
    ~SpawnSetup()
    {
        posix_spawn_file_actions_destroy(&actions);
        posix_spawnattr_destroy(&attributes);
    }
    posix_spawn_file_actions_t actions{};
    posix_spawnattr_t attributes{};
};

void spawn_check(const int error)
{
    if (error) { throw std::runtime_error{"configure indexer process: " + std::string{std::strerror(error)}}; }
}

class Child {
public:
    ~Child()
    {
        if (pid > 0) {
            // The adapter owns only private snapshots. Kill its whole process
            // group on cancellation/timeout, then always reap the direct child.
            ::kill(-pid, SIGKILL);
            if (!reaped) {
                int status{};
                while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) { }
            }
        }
    }
    pid_t pid{-1};
    bool reaped{};
};

std::uintmax_t staged_size(const Path& file)
{
    std::error_code error;
    const auto size = std::filesystem::file_size(file, error);
    return error ? 0 : size;
}

void run_adapter(const RefreshOptions& options, const TempDirectory& temp, const std::stop_token stop)
{
    if (options.adapter_command.empty()) { throw std::runtime_error{"no indexer command configured"}; }
    auto arguments = options.adapter_command;
    arguments.push_back("--osu-dir");
    arguments.push_back(options.osu_directory.string());
    std::vector<char*> argv;
    for (auto& argument : arguments) { argv.push_back(argument.data()); }
    argv.push_back(nullptr);
    // Do not mutate the parent's environment from a worker thread. Give the
    // adapter a private TMPDIR so even SIGKILL cannot orphan full Realm copies
    // in the user's shared temporary directory.
    std::vector<std::string> environment;
    for (auto entry = environ; *entry; ++entry) {
        if (!std::string_view{*entry}.starts_with("TMPDIR=")) { environment.emplace_back(*entry); }
    }
    environment.push_back("TMPDIR=" + temp.path.string());
    std::vector<char*> envp;
    for (auto& entry : environment) { envp.push_back(entry.data()); }
    envp.push_back(nullptr);
    SpawnSetup setup;
    spawn_check(posix_spawn_file_actions_addopen(&setup.actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0));
    spawn_check(posix_spawn_file_actions_addopen(&setup.actions, STDOUT_FILENO,
        (temp.path / "stdout").c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600));
    spawn_check(posix_spawn_file_actions_addopen(&setup.actions, STDERR_FILENO,
        (temp.path / "stderr").c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600));
    sigset_t empty;
    sigemptyset(&empty);
    spawn_check(posix_spawnattr_setsigmask(&setup.attributes, &empty));
    spawn_check(posix_spawnattr_setpgroup(&setup.attributes, 0));
    spawn_check(posix_spawnattr_setflags(&setup.attributes, POSIX_SPAWN_SETPGROUP | POSIX_SPAWN_SETSIGMASK));
    Child child;
    const int error = posix_spawnp(&child.pid, argv[0], &setup.actions, &setup.attributes, argv.data(), envp.data());
    if (error) {
        child.pid = -1;
        throw std::runtime_error{"cannot launch indexer (run make indexer or use --indexer): " +
                                 std::string{std::strerror(error)}};
    }
    const auto deadline = std::chrono::steady_clock::now() + options.timeout;
    int status{};
    for (;;) {
        if (stop.stop_requested()) { throw std::runtime_error{"indexing cancelled"}; }
        if (std::chrono::steady_clock::now() >= deadline) { throw std::runtime_error{"indexer timed out"}; }
        if (staged_size(temp.path / "stdout") > 512U * 1024U * 1024U ||
            staged_size(temp.path / "stderr") > 1024U * 1024U) {
            throw std::runtime_error{"indexer output exceeded safety limit"};
        }
        const pid_t result = ::waitpid(child.pid, &status, WNOHANG);
        if (result == child.pid) { child.reaped = true; break; }
        if (result < 0 && errno != EINTR) { system_error("wait for indexer"); }
        std::this_thread::sleep_for(std::chrono::milliseconds{20});
    }
    if (staged_size(temp.path / "stdout") > 512U * 1024U * 1024U ||
        staged_size(temp.path / "stderr") > 1024U * 1024U) {
        throw std::runtime_error{"indexer output exceeded safety limit"};
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        std::ifstream diagnostics{temp.path / "stderr"};
        std::string text(2048, '\0');
        diagnostics.read(text.data(), static_cast<std::streamsize>(text.size()));
        text.resize(static_cast<std::size_t>(diagnostics.gcount()));
        throw std::runtime_error{"indexer failed: " + (text.empty() ? "no diagnostics" : text)};
    }
}

LibraryLoadResult load_file(const Path& path, const std::stop_token stop)
{
    if (!std::filesystem::is_regular_file(path)) {
        throw std::runtime_error{"not a regular library file: " + path.string()};
    }
    std::ifstream input{path};
    if (!input) { throw std::runtime_error{"cannot open library: " + path.string()}; }
    return load_library_ndjson(input, stop);
}

std::string json_string(json_object* header, const char* key)
{
    json_object* value{};
    if (!json_object_object_get_ex(header, key, &value) || !json_object_is_type(value, json_type_string)) {
        throw std::runtime_error{"invalid cache metadata"};
    }
    return {json_object_get_string(value), static_cast<std::size_t>(json_object_get_string_len(value))};
}

struct CachedLibrary {
    LibraryLoadResult loaded;
    std::string stamp;
};

CachedLibrary read_cache(const Path& path, const Path& source, const std::stop_token stop)
{
    if (!std::filesystem::is_regular_file(path)) { throw std::runtime_error{"cache is not a regular file"}; }
    std::ifstream input{path};
    std::string line;
    std::getline(input, line);
    Json header{json_tokener_parse(line.c_str()), json_object_put};
    if (!header || json_string(header.get(), "cache_version") != "1" ||
        json_string(header.get(), "cache_source") != source.string()) {
        throw std::runtime_error{"cache does not match this osu! directory/version"};
    }
    const auto stamp = json_string(header.get(), "cache_stamp");
    input.clear();
    input.seekg(0);
    // Keep the same open inode if another player atomically replaces this cache.
    auto loaded = load_library_ndjson(input, stop);
    if (json_string(header.get(), "cache_tracks") != std::to_string(loaded.library.tracks().size())) {
        throw std::runtime_error{"truncated cache"};
    }
    return {std::move(loaded), stamp};
}

class AtomicFile {
public:
    explicit AtomicFile(const Path& target)
    {
        std::filesystem::create_directories(target.parent_path());
        auto pattern = target.string() + ".tmp.XXXXXX";
        descriptor = ::mkstemp(pattern.data());
        if (descriptor < 0) { system_error("create cache staging file"); }
        path = pattern;
        ::fcntl(descriptor, F_SETFD, FD_CLOEXEC);
    }
    ~AtomicFile()
    {
        if (descriptor >= 0) { ::close(descriptor); }
        std::error_code ignored;
        std::filesystem::remove(path, ignored);
    }
    void write(const std::string_view text)
    {
        std::size_t offset = 0;
        while (offset < text.size()) {
            const auto written = ::write(descriptor, text.data() + offset, text.size() - offset);
            if (written < 0 && errno == EINTR) { continue; }
            if (written <= 0) { system_error("write cache"); }
            offset += static_cast<std::size_t>(written);
        }
    }
    int descriptor{-1};
    Path path;
};

void write_cache(const Path& target, const Path& raw, const Path& source, const std::string& stamp,
                 const std::size_t tracks, const std::stop_token stop)
{
    std::ifstream input{raw};
    std::string line;
    do { std::getline(input, line); } while (input && line.find_first_not_of(" \t\r\n") == std::string::npos);
    Json header{json_tokener_parse(line.c_str()), json_object_put};
    if (!header) { throw std::runtime_error{"cannot read validated index header"}; }
    const auto add = [&](const char* key, const std::string& value) {
        json_object_object_add(header.get(), key, json_object_new_string(value.c_str()));
    };
    add("cache_version", "1");
    add("cache_source", source.string());
    add("cache_stamp", stamp);
    add("cache_tracks", std::to_string(tracks));
    AtomicFile output{target};
    output.write(json_object_to_json_string_ext(header.get(), JSON_C_TO_STRING_PLAIN));
    output.write("\n");
    char buffer[65536];
    while (input.read(buffer, sizeof buffer) || input.gcount()) {
        if (stop.stop_requested()) { throw std::runtime_error{"indexing cancelled"}; }
        output.write({buffer, static_cast<std::size_t>(input.gcount())});
    }
    if (input.bad()) { throw std::runtime_error{"cannot read staged index"}; }
    if (::fsync(output.descriptor) != 0) { system_error("flush cache"); }
    if (stop.stop_requested()) { throw std::runtime_error{"indexing cancelled"}; }
    if (::rename(output.path.c_str(), target.c_str()) != 0) { system_error("replace cache"); }
}

}  // namespace

std::vector<std::string> default_indexer_command()
{
    const auto executable_directory = std::filesystem::canonical("/proc/self/exe").parent_path();
    const auto packaged = executable_directory / "../lib/osu-player/indexer/osu-indexer";
    if (std::filesystem::is_regular_file(packaged) && ::access(packaged.c_str(), X_OK) == 0) {
        return {packaged.lexically_normal().string()};
    }
    // Development layout, independent of the caller's working directory. An
    // installed adapter can instead be supplied as an executable via --indexer.
    const auto root = executable_directory.parent_path();
    const auto local_dotnet = root / ".tools/dotnet10/dotnet";
    return {std::filesystem::exists(local_dotnet) ? local_dotnet.string() : "dotnet",
            (root / "tools/osu-indexer/bin/Release/net10.0/osu-indexer.dll").string()};
}

LibraryRefresh::LibraryRefresh(RefreshOptions options) : options_{std::move(options)}
{
    if (!options_.export_file) {
        options_.osu_directory = std::filesystem::weakly_canonical(options_.osu_directory);
    }
}

LibraryRefresh::~LibraryRefresh() = default;

bool LibraryRefresh::start(const bool try_cache)
{
    if (busy_.exchange(true)) { return false; }
    try {
        worker_ = std::jthread{[this, try_cache](const std::stop_token stop) {
            try { run(stop, try_cache); }
            catch (const std::exception& error) {
                if (!stop.stop_requested()) { publish({std::nullopt, "Refresh failed: " + std::string{error.what()}}); }
            }
            busy_ = false;
        }};
    } catch (...) { busy_ = false; throw; }
    return true;
}

bool LibraryRefresh::busy() const noexcept { return busy_.load(); }

void LibraryRefresh::publish(RefreshEvent event)
{
    std::lock_guard lock{mutex_};
    events_.push_back(std::move(event));
}

std::optional<RefreshEvent> LibraryRefresh::poll()
{
    std::lock_guard lock{mutex_};
    if (events_.empty()) { return std::nullopt; }
    auto event = std::move(events_.front());
    events_.pop_front();
    return event;
}

void LibraryRefresh::run(const std::stop_token stop, const bool try_cache)
{
    if (options_.export_file) {
        auto loaded = load_file(*options_.export_file, stop);
        if (!stop.stop_requested()) { publish({std::move(loaded), "Library reloaded."}); }
        return;
    }
    const auto source = options_.osu_directory;
    std::optional<Path> cache;
    std::string cache_warning;
    if (options_.use_cache) {
        try { cache = cache_path(source); }
        catch (const std::exception& error) {
            cache_warning = " Cache unavailable: " + std::string{error.what()};
            publish({std::nullopt, cache_warning});
        }
    }
    std::error_code cache_error;
    if (try_cache && cache && std::filesystem::exists(*cache, cache_error)) {
        try {
            auto cached = read_cache(*cache, source, stop);
            bool fresh = false;
            try { fresh = fingerprint(source / "client.realm") == cached.stamp; } catch (const std::exception&) { }
            publish({std::move(cached.loaded), fresh ? "Loaded cached library." : "Loaded stale cache; indexing in background."});
            if (fresh) { return; }
        } catch (const std::exception& error) {
            publish({std::nullopt, "Cache ignored: " + std::string{error.what()}});
        }
    }
    if (stop.stop_requested()) { return; }
    const auto before = fingerprint(source / "client.realm");
    TempDirectory temp{source};
    run_adapter(options_, temp, stop);
    auto loaded = load_file(temp.path / "stdout", stop);
    if (stop.stop_requested()) { return; }
    if (fingerprint(source / "client.realm") != before) {
        throw std::runtime_error{"client.realm changed during indexing; close lazer and retry"};
    }
    std::string message = "Library indexed." + cache_warning;
    if (cache) {
        try { write_cache(*cache, temp.path / "stdout", source, before, loaded.library.tracks().size(), stop); }
        catch (const std::exception& error) { message += " Cache not saved: " + std::string{error.what()}; }
    }
    if (!stop.stop_requested()) { publish({std::move(loaded), std::move(message)}); }
}

}  // namespace osu_player
