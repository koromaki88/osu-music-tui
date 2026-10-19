#include "osu_player/settings.hpp"

#include <json-c/json.h>
#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <vector>

namespace osu_player {
namespace {
using Json = std::unique_ptr<json_object, decltype(&json_object_put)>;

[[noreturn]] void invalid() { throw std::runtime_error{"invalid or unsupported settings file"}; }

json_object* field(json_object* object, const char* name)
{
    json_object* value{};
    json_object_object_get_ex(object, name, &value);
    return value;
}
}  // namespace

std::filesystem::path settings_path()
{
    if (const char* config = std::getenv("XDG_CONFIG_HOME"); config && *config) {
        const std::filesystem::path path{config};
        if (path.is_absolute()) { return path / "osu-player/settings.json"; }
    }
    if (const char* home = std::getenv("HOME"); home && *home) {
        const std::filesystem::path path{home};
        if (path.is_absolute()) { return path / ".config/osu-player/settings.json"; }
    }
    throw std::runtime_error{"no absolute XDG_CONFIG_HOME or HOME for settings"};
}

Settings load_settings(const std::filesystem::path& path)
{
    if (!std::filesystem::exists(path)) { return {}; }
    if (!std::filesystem::is_regular_file(path)) {
        throw std::runtime_error{"settings path is not a regular file"};
    }
    std::ifstream stream{path, std::ios::binary};
    if (!stream) { throw std::runtime_error{"cannot read settings file"}; }
    std::string text(4097, '\0');
    stream.read(text.data(), static_cast<std::streamsize>(text.size()));
    text.resize(static_cast<std::size_t>(stream.gcount()));
    if (stream.bad()) { throw std::runtime_error{"cannot read settings file"}; }
    if (text.size() > 4096 || text.find('\0') != std::string::npos) { invalid(); }
    std::unique_ptr<json_tokener, decltype(&json_tokener_free)> parser{json_tokener_new(), json_tokener_free};
    json_tokener_set_flags(parser.get(), JSON_TOKENER_STRICT | JSON_TOKENER_VALIDATE_UTF8);
    Json root{json_tokener_parse_ex(parser.get(), text.c_str(), static_cast<int>(text.size() + 1)), json_object_put};
    if (json_tokener_get_error(parser.get()) != json_tokener_success ||
        !json_object_is_type(root.get(), json_type_object)) { invalid(); }
    const auto version = field(root.get(), "version");
    if (!json_object_is_type(version, json_type_int) || json_object_get_int64(version) != 1) { invalid(); }
    Settings settings;
    if (const auto value = field(root.get(), "volume")) {
        if (!json_object_is_type(value, json_type_double) && !json_object_is_type(value, json_type_int)) { invalid(); }
        const double volume = json_object_get_double(value);
        if (!std::isfinite(volume) || volume < 0 || volume > 1) { invalid(); }
        settings.volume = static_cast<float>(volume);
    }
    if (const auto value = field(root.get(), "shuffle")) {
        if (!json_object_is_type(value, json_type_boolean)) { invalid(); }
        settings.shuffle = json_object_get_boolean(value) != 0;
    }
    if (const auto value = field(root.get(), "repeat")) {
        if (!json_object_is_type(value, json_type_string)) { invalid(); }
        const std::string mode{json_object_get_string(value)};
        if (mode != "off" && mode != "one" && mode != "all") { invalid(); }
        settings.repeat = mode == "one" ? RepeatMode::one : mode == "all" ? RepeatMode::all : RepeatMode::off;
    }
    if (const auto value = field(root.get(), "search")) {
        if (!json_object_is_type(value, json_type_string)) { invalid(); }
        settings.search.assign(json_object_get_string(value), static_cast<std::size_t>(json_object_get_string_len(value)));
        if (settings.search.size() > 256 || settings.search.find('\0') != std::string::npos) { invalid(); }
    }
    json_object* discord{};
    if (json_object_object_get_ex(root.get(), "discord", &discord)) {
        if (!json_object_is_type(discord, json_type_object)) { invalid(); }
        json_object* value{};
        if (json_object_object_get_ex(discord, "enabled", &value)) {
            if (!json_object_is_type(value, json_type_boolean)) { invalid(); }
            settings.discord_enabled = json_object_get_boolean(value) != 0;
        }
        if (json_object_object_get_ex(discord, "application_id", &value)) {
            if (!json_object_is_type(value, json_type_string)) { invalid(); }
            settings.discord_application_id.assign(json_object_get_string(value),
                static_cast<std::size_t>(json_object_get_string_len(value)));
            if (settings.discord_application_id.size() > 32 || settings.discord_application_id.find('\0') != std::string::npos) { invalid(); }
        }
    }
    json_object* lastfm{};
    if (json_object_object_get_ex(root.get(), "lastfm", &lastfm)) {
        if (!json_object_is_type(lastfm, json_type_object)) { invalid(); }
        json_object* enabled{};
        if (json_object_object_get_ex(lastfm, "enabled", &enabled)) {
            if (!json_object_is_type(enabled, json_type_boolean)) { invalid(); }
            settings.lastfm_enabled = json_object_get_boolean(enabled) != 0;
        }
    }
    return settings;
}

void save_settings(const std::filesystem::path& path, const Settings& settings)
{
    Json root{json_object_new_object(), json_object_put};
    json_object_object_add(root.get(), "version", json_object_new_int(1));
    json_object_object_add(root.get(), "volume", json_object_new_double(static_cast<double>(settings.volume)));
    json_object_object_add(root.get(), "shuffle", json_object_new_boolean(settings.shuffle));
    json_object_object_add(root.get(), "repeat", json_object_new_string(settings.repeat == RepeatMode::one ? "one" :
        settings.repeat == RepeatMode::all ? "all" : "off"));
    json_object_object_add(root.get(), "search", json_object_new_string(settings.search.c_str()));
    auto* discord = json_object_new_object();
    json_object_object_add(root.get(), "discord", discord);
    json_object_object_add(discord, "enabled", json_object_new_boolean(settings.discord_enabled));
    json_object_object_add(discord, "application_id", json_object_new_string(settings.discord_application_id.c_str()));
    auto* lastfm = json_object_new_object();
    json_object_object_add(root.get(), "lastfm", lastfm);
    json_object_object_add(lastfm, "enabled", json_object_new_boolean(settings.lastfm_enabled));
    const std::string data = std::string{json_object_to_json_string_ext(root.get(), JSON_C_TO_STRING_PRETTY)} + '\n';
    std::filesystem::create_directories(path.parent_path());
    std::string pattern = (path.parent_path() / ".settings-XXXXXX").string();
    std::vector<char> name(pattern.begin(), pattern.end());
    name.push_back('\0');
    int descriptor = mkstemp(name.data()); // Private 0600 file, same filesystem as destination.
    if (descriptor < 0) { throw std::runtime_error{"cannot create settings staging file"}; }
    try {
        std::size_t offset{};
        while (offset < data.size()) {
            const auto count = write(descriptor, data.data() + offset, data.size() - offset);
            if (count < 0 && errno == EINTR) { continue; }
            if (count <= 0) { throw std::runtime_error{"cannot write settings file"}; }
            offset += static_cast<std::size_t>(count);
        }
        if (fsync(descriptor) != 0) { throw std::runtime_error{"cannot sync settings file"}; }
        const int status = close(descriptor);
        descriptor = -1;
        if (status != 0) { throw std::runtime_error{"cannot close settings file"}; }
        std::filesystem::rename(name.data(), path);
        const int directory = open(path.parent_path().c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (directory < 0) { throw std::runtime_error{"cannot open settings directory"}; }
        const int synced = fsync(directory);
        close(directory);
        if (synced != 0) { throw std::runtime_error{"cannot sync settings directory"}; }
    } catch (...) {
        if (descriptor >= 0) { close(descriptor); }
        unlink(name.data());
        throw;
    }
}
}  // namespace osu_player
