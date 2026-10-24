#include "overlay_config.hpp"

#include <json-c/json.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <memory>
#include <stdexcept>
#include <string_view>

namespace osu_player::overlay_detail {
namespace {
constexpr std::size_t file_limit = 8192;
constexpr const char* defaults = R"({"width":null,"height":180,"font":"Noto Sans","title_size":32,"artist_size":23,"background":"#101218","background_opacity":0.85,"text_color":"#ffffff","accent_color":"#ff78ae","corner_radius":14,"show_progress":true,"show_time":true,"show_thumbnail":true,"thumbnail_size":112})";
using Json = std::unique_ptr<json_object, decltype(&json_object_put)>;
struct File {
    int fd;
    ~File() { if (fd >= 0) { close(fd); } }
};
void require(const bool valid, const std::string& message)
{
    if (!valid) { throw std::runtime_error{message}; }
}
bool color(const std::string_view value)
{
    return value.size() == 7 && value[0] == '#' && std::all_of(value.begin() + 1, value.end(), [](const char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
    });
}
}
std::string default_style() { return parse_style("{}"); }
std::string parse_style(const std::string& content)
{
    require(!content.empty() && content.size() <= file_limit && content.find('\0') == std::string::npos, "empty, oversized or invalid overlay.json");
    std::unique_ptr<json_tokener, decltype(&json_tokener_free)> parser{json_tokener_new(), json_tokener_free};
    json_tokener_set_flags(parser.get(), JSON_TOKENER_STRICT | JSON_TOKENER_VALIDATE_UTF8);
    Json input{json_tokener_parse_ex(parser.get(), content.c_str(), static_cast<int>(content.size() + 1)), json_object_put};
    require(json_tokener_get_error(parser.get()) == json_tokener_success && json_object_is_type(input.get(), json_type_object), "invalid overlay.json object");
    Json output{json_tokener_parse(defaults), json_object_put};
    json_object_object_foreach(input.get(), key, value) {
        const std::string_view name{key};
        json_object* original{};
        require(json_object_object_get_ex(output.get(), key, &original), "unknown overlay option");
        if (name == "width") {
            require(json_object_is_type(value, json_type_null) ||
                (json_object_is_type(value, json_type_int) && json_object_get_int64(value) >= 200 && json_object_get_int64(value) <= 3840),
                "overlay width must be null (fluid) or an integer between 200 and 3840");
            json_object_object_add(output.get(), key, json_object_get(value));
            continue;
        }
        require(json_object_is_type(value, json_object_get_type(original)) ||
            (name == "background_opacity" && json_object_is_type(value, json_type_int)), "wrong overlay option type");
        if (json_object_is_type(original, json_type_int)) {
            const auto number = json_object_get_int64(value);
            const auto low = name == "thumbnail_size" ? 32 : (name == "title_size" || name == "artist_size") ? 8 : 0;
            const auto high = name == "thumbnail_size" ? 256 : name == "height" ? 2160 : name == "artist_size" ? 96 : 128;
            require(number >= low && number <= high, "overlay numeric option out of range");
        } else if (name == "background_opacity") {
            const auto number = json_object_get_double(value);
            require(std::isfinite(number) && number >= 0 && number <= 1, "overlay opacity must be between 0 and 1");
        } else if (json_object_is_type(original, json_type_string)) {
            const std::string_view text{json_object_get_string(value), static_cast<std::size_t>(json_object_get_string_len(value))};
            if (name == "font") {
                require(!text.empty() && text.size() <= 128 && text.find_first_not_of(' ') != std::string_view::npos &&
                    std::all_of(text.begin(), text.end(), [](const char c) {
                        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == ' ' || c == '-' || c == '_';
                    }), "overlay font must be a plain family name (letters, digits, spaces, hyphens, underscores)");
            } else { require(color(text), "overlay colors must use #RRGGBB"); }
        }
        json_object_object_add(output.get(), key, json_object_get(value));
    }
    return json_object_to_json_string_ext(output.get(), JSON_C_TO_STRING_PLAIN);
}
std::string load_style(const std::filesystem::path& path)
{
    File file{open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC)};
    if (file.fd < 0 && errno == ENOENT) { return default_style(); }
    require(file.fd >= 0, "cannot open overlay.json (symlinks are not supported)");
    struct stat info{};
    require(fstat(file.fd, &info) == 0 && S_ISREG(info.st_mode) && info.st_uid == getuid() &&
        info.st_size >= 0 && info.st_size <= static_cast<off_t>(file_limit), "overlay.json must be an owned regular file, at most 8 KiB");
    std::string content;
    char buffer[1024];
    for (;;) {
        const auto bytes = read(file.fd, buffer, sizeof(buffer));
        if (bytes < 0 && errno == EINTR) { continue; }
        require(bytes >= 0, "cannot read overlay.json");
        if (bytes == 0) { break; }
        content.append(buffer, static_cast<std::size_t>(bytes));
        require(content.size() <= file_limit, "overlay.json exceeds 8 KiB");
    }
    return parse_style(content);
}
void create_style_if_missing(const std::filesystem::path& path)
{
    std::filesystem::create_directories(path.parent_path());
    File file{open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC, 0600)};
    if (file.fd < 0 && errno == EEXIST) { return; } // Never rewrite user edits, even invalid ones.
    require(file.fd >= 0, "cannot create default overlay.json");
    Json object{json_tokener_parse(defaults), json_object_put};
    const std::string content = std::string{json_object_to_json_string_ext(object.get(), JSON_C_TO_STRING_PRETTY)} + '\n';
    std::size_t offset{};
    while (offset < content.size()) {
        const auto bytes = write(file.fd, content.data() + offset, content.size() - offset);
        if (bytes < 0 && errno == EINTR) { continue; }
        require(bytes > 0, "cannot write default overlay.json");
        offset += static_cast<std::size_t>(bytes);
    }
    require(fsync(file.fd) == 0, "cannot flush default overlay.json");
}
} // namespace osu_player::overlay_detail
