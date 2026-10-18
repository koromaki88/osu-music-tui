#include "osu_player/library_loader.hpp"

#include "osu_player/storage_path.hpp"

#include <json-c/json.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace osu_player {
namespace {

struct JsonDeleter {
    void operator()(json_object* object) const noexcept
    {
        json_object_put(object);
    }
};

using JsonObject = std::unique_ptr<json_object, JsonDeleter>;

[[noreturn]] void fail(const std::size_t line_number, const std::string& message)
{
    throw LibraryFormatError{"line " + std::to_string(line_number) + ": " + message};
}

[[nodiscard]] JsonObject parse_json_line(const std::string& line, const std::size_t line_number)
{
    json_tokener_error error = json_tokener_success;
    json_object* const parsed = json_tokener_parse_verbose(line.c_str(), &error);
    JsonObject result{parsed};

    if (error != json_tokener_success || result == nullptr) {
        fail(line_number, "invalid JSON: " + std::string{json_tokener_error_desc(error)});
    }

    if (!json_object_is_type(result.get(), json_type_object)) {
        fail(line_number, "record must be a JSON object");
    }

    return result;
}

[[nodiscard]] json_object* property(
    json_object* object,
    const char* name,
    const std::size_t line_number,
    const bool required = true)
{
    json_object* value = nullptr;
    if (!json_object_object_get_ex(object, name, &value) || value == nullptr ||
        json_object_is_type(value, json_type_null)) {
        if (required) {
            fail(line_number, "missing required property '" + std::string{name} + "'");
        }
        return nullptr;
    }

    return value;
}

[[nodiscard]] std::string string_property(
    json_object* object,
    const char* name,
    const std::size_t line_number,
    const bool required = true)
{
    json_object* const value = property(object, name, line_number, required);
    if (value == nullptr) {
        return {};
    }

    if (!json_object_is_type(value, json_type_string)) {
        fail(line_number, "property '" + std::string{name} + "' must be a string");
    }

    return json_object_get_string(value);
}

[[nodiscard]] std::optional<std::int64_t> optional_integer_property(
    json_object* object,
    const char* name,
    const std::size_t line_number)
{
    json_object* const value = property(object, name, line_number, false);
    if (value == nullptr) {
        return std::nullopt;
    }

    if (!json_object_is_type(value, json_type_int)) {
        fail(line_number, "property '" + std::string{name} + "' must be an integer or null");
    }

    return json_object_get_int64(value);
}

[[nodiscard]] std::optional<double> optional_number_property(
    json_object* object,
    const char* name,
    const std::size_t line_number)
{
    json_object* const value = property(object, name, line_number, false);
    if (value == nullptr) {
        return std::nullopt;
    }

    if (!json_object_is_type(value, json_type_double) &&
        !json_object_is_type(value, json_type_int)) {
        fail(line_number, "property '" + std::string{name} + "' must be a number or null");
    }

    return json_object_get_double(value);
}

[[nodiscard]] std::optional<std::string> optional_string_property(
    json_object* object,
    const char* name,
    const std::size_t line_number)
{
    json_object* const value = property(object, name, line_number, false);
    if (value == nullptr) {
        return std::nullopt;
    }

    if (!json_object_is_type(value, json_type_string)) {
        fail(line_number, "property '" + std::string{name} + "' must be a string or null");
    }

    return std::string{json_object_get_string(value)};
}

void require_nonempty(
    const std::string& value,
    const char* property_name,
    const std::size_t line_number)
{
    if (value.empty()) {
        fail(line_number, "property '" + std::string{property_name} + "' must not be empty");
    }
}

[[nodiscard]] Track parse_track(json_object* record, const std::size_t line_number)
{
    Track track;
    track.stable_id = string_property(record, "stable_id", line_number);
    track.beatmap_set_id = string_property(record, "beatmap_set_id", line_number);
    track.online_set_id = optional_integer_property(record, "online_set_id", line_number);
    track.title = string_property(record, "title", line_number);
    track.title_unicode = string_property(record, "title_unicode", line_number, false);
    track.artist = string_property(record, "artist", line_number);
    track.artist_unicode = string_property(record, "artist_unicode", line_number, false);
    track.creator = string_property(record, "creator", line_number, false);
    track.audio_filename = string_property(record, "audio_filename", line_number);
    track.audio_hash = string_property(record, "audio_hash", line_number);
    track.audio_path = string_property(record, "audio_path", line_number);
    track.duration_ms = optional_integer_property(record, "duration_ms", line_number);
    track.bpm = optional_number_property(record, "bpm", line_number);
    track.date_added = optional_string_property(record, "date_added", line_number);

    require_nonempty(track.stable_id, "stable_id", line_number);
    require_nonempty(track.beatmap_set_id, "beatmap_set_id", line_number);
    require_nonempty(track.title, "title", line_number);
    require_nonempty(track.artist, "artist", line_number);
    require_nonempty(track.audio_filename, "audio_filename", line_number);

    if (!is_sha256(track.audio_hash)) {
        fail(line_number, "property 'audio_hash' is not a SHA-256 hash");
    }
    track.audio_hash = normalize_sha256(track.audio_hash);

    if (!track.audio_path.is_absolute()) {
        fail(line_number, "property 'audio_path' must be absolute");
    }
    if (track.duration_ms.has_value() && *track.duration_ms < 0) {
        fail(line_number, "property 'duration_ms' must not be negative");
    }
    if (track.bpm.has_value() && *track.bpm < 0.0) {
        fail(line_number, "property 'bpm' must not be negative");
    }

    return track;
}

[[nodiscard]] bool is_blank(const std::string_view line)
{
    return line.find_first_not_of(" \t\r\n") == std::string_view::npos;
}

}  // namespace

LibraryLoadResult load_library_ndjson(std::istream& input)
{
    LibraryLoadResult result;
    std::vector<Track> tracks;
    std::string line;
    std::size_t line_number = 0;
    bool header_seen = false;

    while (std::getline(input, line)) {
        ++line_number;
        if (is_blank(line)) {
            continue;
        }

        JsonObject record = parse_json_line(line, line_number);
        const std::string type = string_property(record.get(), "type", line_number);

        if (!header_seen) {
            if (type != "header") {
                fail(line_number, "the first record must be a header");
            }

            const std::optional<std::int64_t> version =
                optional_integer_property(record.get(), "schema_version", line_number);
            if (!version.has_value()) {
                fail(line_number, "header requires an integer 'schema_version'");
            }
            if (*version != library_schema_version) {
                fail(line_number, "unsupported schema version " + std::to_string(*version));
            }

            result.schema_version = static_cast<int>(*version);
            result.lazer_version = string_property(record.get(), "lazer_version", line_number, false);
            header_seen = true;
            continue;
        }

        if (type == "track") {
            tracks.push_back(parse_track(record.get(), line_number));
        } else if (type == "warning") {
            const std::string message = string_property(record.get(), "message", line_number);
            require_nonempty(message, "message", line_number);
            result.warnings.push_back(message);
        } else if (type == "header") {
            fail(line_number, "duplicate header record");
        } else {
            fail(line_number, "unknown record type '" + type + "'");
        }
    }

    if (!input.eof() && input.fail()) {
        throw LibraryFormatError{"could not read the NDJSON stream"};
    }
    if (!header_seen) {
        throw LibraryFormatError{"library contains no header record"};
    }

    result.library = Library{std::move(tracks)};
    return result;
}

}  // namespace osu_player
