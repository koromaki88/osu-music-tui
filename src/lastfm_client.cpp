#include "osu_player/lastfm_client.hpp"
#include "osu_player/version.hpp"
#include "osu_player/settings.hpp"
#include "lastfm_storage.hpp"

#include <curl/curl.h>
#include <json-c/json.h>
#include <openssl/evp.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstdlib>
#include <memory>
#include <stdexcept>
#include <string_view>

namespace osu_player {
namespace {
using Json = std::unique_ptr<json_object, decltype(&json_object_put)>;
[[noreturn]] void invalid() { throw LastfmFailure{LastfmFailureKind::credential_format}; }
[[noreturn]] void io_failure() { throw LastfmFailure{LastfmFailureKind::credential_io, errno}; }
[[noreturn]] void unsafe() { throw LastfmFailure{LastfmFailureKind::credential_permissions}; }

std::string string_field(json_object* root, const char* name)
{
    json_object* value{};
    if (!json_object_object_get_ex(root, name, &value) || !json_object_is_type(value, json_type_string)) { invalid(); }
    return {json_object_get_string(value), static_cast<std::size_t>(json_object_get_string_len(value))};
}

bool valid_username(const std::string& value)
{
    if (value.empty() || value.size() > 128) { return false; }
    for (const char character : value) {
        const auto byte = static_cast<unsigned char>(character);
        if (byte < 0x20 || byte == 0x7f) { return false; }
    }
    return true;
}

void validate(const LastfmCredentials& value)
{
    if (!valid_lastfm_key(value.api_key) || !valid_lastfm_key(value.secret) ||
        (value.session_key.empty() != value.username.empty()) ||
        (!value.session_key.empty() && (!valid_lastfm_token(value.session_key) || !valid_username(value.username)))) { invalid(); }
}

std::string encode(const std::string& text)
{
    constexpr char hex[] = "0123456789ABCDEF";
    std::string result;
    for (const char character : text) {
        const auto byte = static_cast<unsigned char>(character);
        if ((byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') ||
            (byte >= '0' && byte <= '9') || byte == '-' || byte == '.' || byte == '_' || byte == '~') {
            result += static_cast<char>(byte);
        } else {
            result += '%'; result += hex[byte >> 4]; result += hex[byte & 15];
        }
    }
    return result;
}

bool metadata_valid(const std::string& text)
{
    if (text.empty() || text.size() > 1024 || text.find('\0') != std::string::npos) { return false; }
    bool visible{};
    for (const char character : text) {
        const auto byte = static_cast<unsigned char>(character);
        if (byte < 0x20 || byte == 0x7f) { return false; }
        visible = visible || byte != ' ';
    }
    // Validate UTF-8 without normalizing, guessing, or correcting metadata.
    Json value{json_object_new_string_len(text.data(), static_cast<int>(text.size())), json_object_put};
    const std::string encoded = json_object_to_json_string_ext(value.get(), JSON_C_TO_STRING_PLAIN);
    std::unique_ptr<json_tokener, decltype(&json_tokener_free)> parser{json_tokener_new(), json_tokener_free};
    json_tokener_set_flags(parser.get(), JSON_TOKENER_STRICT | JSON_TOKENER_VALIDATE_UTF8);
    Json parsed{json_tokener_parse_ex(parser.get(), encoded.c_str(), static_cast<int>(encoded.size() + 1)), json_object_put};
    return visible && json_tokener_get_error(parser.get()) == json_tokener_success;
}
}  // namespace

std::string lastfm_failure_message(const LastfmFailure& failure)
{
    switch (failure.kind) {
        case LastfmFailureKind::credential_format: return "Credentials: malformed/unsupported file; preserved";
        case LastfmFailureKind::credential_permissions: return "Credentials: unsafe owner/type/mode/link (directory 0700, file 0600)";
        case LastfmFailureKind::credential_io: return "Credentials: file I/O failed (errno " + std::to_string(failure.code) + ")";
        case LastfmFailureKind::signing: return "Signing: OpenSSL MD5 unavailable/failed";
        case LastfmFailureKind::https_setup: return "HTTPS: libcurl setup failed (code " + std::to_string(failure.code) + ")";
        case LastfmFailureKind::https_transfer: return "HTTPS: transfer failed (curl code " + std::to_string(failure.code) + ")";
        case LastfmFailureKind::response_format:
            switch (failure.code) {
                case 1: return "Response: invalid JSON (expected JSON, not XML/HTML)";
                case 2: return "Response: JSON root is not an object";
                case 3: return "Response: auth.getToken missing string token";
                case 4: return "Response: auth.getToken token has unsafe length/characters";
                case 5: return "Response: auth.getSession missing string session.key";
                case 6: return "Response: auth.getSession missing string session.name";
                case 7: return "Response: Now Playing missing ignoredMessage.code";
                case 8: return "Response: invalid scrobble acknowledgement; retained for retry";
                default: return "Response: invalid Last.fm response fields";
            }
        case LastfmFailureKind::http_status: return "HTTPS: unexpected HTTP status " + std::to_string(failure.code);
        case LastfmFailureKind::queue:
            switch (failure.code) {
                case 1: return "Scrobble queue: malformed/newer file; preserved";
                case 2: return "Scrobble queue: unsafe file or I/O failure; submissions stopped";
                case 3: return "Scrobble queue: in use by another player";
                case 4: return "Scrobble queue: full; new scrobbles cannot be saved";
                default: return "Scrobble queue: unavailable";
            }
    }
    return "Last.fm: unknown failure";
}

bool valid_lastfm_key(const std::string& value) noexcept
{
    if (value.size() != 32) { return false; }
    for (const char byte : value) {
        if (!((byte >= '0' && byte <= '9') || (byte >= 'a' && byte <= 'f') || (byte >= 'A' && byte <= 'F'))) { return false; }
    }
    return true;
}

bool valid_lastfm_token(const std::string& value) noexcept
{
    if (value.empty() || value.size() > 256) { return false; }
    for (const char byte : value) {
        if (!((byte >= '0' && byte <= '9') || (byte >= 'a' && byte <= 'z') ||
              (byte >= 'A' && byte <= 'Z') || byte == '-' || byte == '_' || byte == '.' || byte == '~')) { return false; }
    }
    return true;
}

std::filesystem::path lastfm_credentials_path() { return settings_path().parent_path() / "lastfm/credentials.json"; }

LastfmCredentials load_lastfm_credentials(const std::filesystem::path& path)
{
    struct stat parent{};
    if (lstat(path.parent_path().c_str(), &parent) != 0) {
        if (errno == ENOENT) { return {}; }
        io_failure();
    }
    if (!S_ISDIR(parent.st_mode) || parent.st_uid != getuid() || (parent.st_mode & 077) != 0) { unsafe(); }
    lastfm_storage::Directory directory{path.parent_path(), false};
    auto file = directory.open_file(path.filename().string(), O_RDONLY, true);
    if (file.fd < 0) { return {}; }
    struct stat info{};
    if (fstat(file.fd, &info) != 0) { io_failure(); }
    if (!S_ISREG(info.st_mode) || info.st_uid != getuid() || (info.st_mode & 077) != 0) { unsafe(); }
    if (info.st_size < 0 || info.st_size > 4096) { invalid(); }
    std::string text;
    std::array<char, 4097> buffer{};
    while (true) {
        const auto count = read(file.fd, buffer.data(), buffer.size());
        if (count < 0 && errno == EINTR) { continue; }
        if (count < 0) { io_failure(); }
        if (count == 0) { break; }
        text.append(buffer.data(), static_cast<std::size_t>(count));
        if (text.size() > 4096) { invalid(); }
    }
    if (text.find('\0') != std::string::npos) { invalid(); }
    std::unique_ptr<json_tokener, decltype(&json_tokener_free)> parser{json_tokener_new(), json_tokener_free};
    json_tokener_set_flags(parser.get(), JSON_TOKENER_STRICT | JSON_TOKENER_VALIDATE_UTF8);
    Json root{json_tokener_parse_ex(parser.get(), text.c_str(), static_cast<int>(text.size() + 1)), json_object_put};
    json_object* version{};
    if (json_tokener_get_error(parser.get()) != json_tokener_success ||
        !json_object_is_type(root.get(), json_type_object) ||
        !json_object_object_get_ex(root.get(), "version", &version) ||
        !json_object_is_type(version, json_type_int) || json_object_get_int64(version) != 1) { invalid(); }
    LastfmCredentials credentials{string_field(root.get(), "api_key"), string_field(root.get(), "secret"),
                                   string_field(root.get(), "username"), string_field(root.get(), "session_key")};
    validate(credentials);
    return credentials;
}

void save_lastfm_credentials(const std::filesystem::path& path, const LastfmCredentials& value)
{
    validate(value);
    // Preserve malformed/newer/unsafe files, even during reconfiguration.
    (void)load_lastfm_credentials(path);
    std::error_code directory_error;
    std::filesystem::create_directories(path.parent_path().parent_path(), directory_error);
    if (directory_error) { throw LastfmFailure{LastfmFailureKind::credential_io, directory_error.value()}; }
    if (mkdir(path.parent_path().c_str(), 0700) != 0 && errno != EEXIST) { io_failure(); }
    struct stat directory_info{};
    if (lstat(path.parent_path().c_str(), &directory_info) != 0 || !S_ISDIR(directory_info.st_mode) ||
        directory_info.st_uid != getuid() || (directory_info.st_mode & 077) != 0) { unsafe(); }
    Json root{json_object_new_object(), json_object_put};
    json_object_object_add(root.get(), "version", json_object_new_int(1));
    for (const auto& [name, field] : std::array<std::pair<const char*, const std::string*>, 4>{{
             {"api_key", &value.api_key}, {"secret", &value.secret}, {"username", &value.username}, {"session_key", &value.session_key}}}) {
        json_object_object_add(root.get(), name, json_object_new_string(field->c_str()));
    }
    const std::string text = json_object_to_json_string_ext(root.get(), JSON_C_TO_STRING_PLAIN);
    lastfm_storage::Directory directory{path.parent_path(), false};
    directory.atomic_write(path.filename().string(), text);
}

std::string lastfm_signature(const LastfmParameters& parameters, const std::string& secret)
{
    std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> context{EVP_MD_CTX_new(), EVP_MD_CTX_free};
    if (!context || EVP_DigestInit_ex(context.get(), EVP_md5(), nullptr) != 1) { throw LastfmFailure{LastfmFailureKind::signing}; }
    for (const auto& [key, value] : parameters) {
        if (key == "format" || key == "callback" || key == "api_sig") { continue; }
        if (EVP_DigestUpdate(context.get(), key.data(), key.size()) != 1 ||
            EVP_DigestUpdate(context.get(), value.data(), value.size()) != 1) { throw LastfmFailure{LastfmFailureKind::signing}; }
    }
    std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
    unsigned int size{};
    if (EVP_DigestUpdate(context.get(), secret.data(), secret.size()) != 1 ||
        EVP_DigestFinal_ex(context.get(), digest.data(), &size) != 1 || size != 16) { throw LastfmFailure{LastfmFailureKind::signing}; }
    constexpr char hex[] = "0123456789abcdef";
    std::string result;
    for (unsigned int index = 0; index < size; ++index) { result += hex[digest[index] >> 4]; result += hex[digest[index] & 15]; }
    return result;
}

std::string lastfm_form(const LastfmParameters& parameters)
{
    std::string result;
    for (const auto& [key, value] : parameters) {
        if (!result.empty()) { result += '&'; }
        result += encode(key) + '=' + encode(value);
    }
    return result;
}

LastfmParameters lastfm_signed(LastfmParameters parameters, const LastfmCredentials& credentials)
{
    parameters["api_key"] = credentials.api_key;
    parameters["api_sig"] = lastfm_signature(parameters, credentials.secret);
    parameters["format"] = "json";
    return parameters;
}

std::optional<LastfmParameters> lastfm_now_playing(const PlaybackSnapshot& playback)
{
    const auto& track = playback.track;
    // Use structured original metadata, falling back only when the original field
    // is absent. Do not strip artist prefixes or apply Last.fm corrections.
    const auto& artist = track.artist.empty() ? track.artist_unicode : track.artist;
    const auto& title = track.title.empty() ? track.title_unicode : track.title;
    if (!playback.playing || !metadata_valid(artist) || !metadata_valid(title)) { return std::nullopt; }
    LastfmParameters parameters{{"method", "track.updateNowPlaying"}, {"artist", artist}, {"track", title}};
    if (playback.duration && playback.duration->count() >= 1000) {
        parameters["duration"] = std::to_string(playback.duration->count() / 1000);
    }
    return parameters;
}

LastfmResponse lastfm_https(const LastfmParameters& parameters, const LastfmCancelled& cancelled)
{
    static const auto initialized = curl_global_init(CURL_GLOBAL_DEFAULT);
    if (initialized != CURLE_OK) { throw LastfmFailure{LastfmFailureKind::https_setup, static_cast<int>(initialized)}; }
    if (cancelled()) { throw std::runtime_error{"Last.fm request cancelled"}; }
    std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> curl{curl_easy_init(), curl_easy_cleanup};
    if (!curl) { throw LastfmFailure{LastfmFailureKind::https_setup, static_cast<int>(CURLE_FAILED_INIT)}; }
    const auto body = lastfm_form(parameters);
    LastfmResponse response;
    auto write_body = +[](char* data, std::size_t size, std::size_t count, void* context) noexcept -> std::size_t {
        auto& output = *static_cast<std::string*>(context);
        if (size != 0 && count > (128 * 1024 - output.size()) / size) { return 0; }
        const auto bytes = size * count;
        try { output.append(data, bytes); } catch (...) { return 0; }
        return bytes;
    };
    auto progress = +[](void* context, curl_off_t, curl_off_t, curl_off_t, curl_off_t) noexcept -> int {
        try { return (*static_cast<const LastfmCancelled*>(context))() ? 1 : 0; } catch (...) { return 1; }
    };
    // Check every option: no silent downgrade if libcurl lacks required security.
    auto option = [&](const CURLoption key, auto value) {
        const auto result = curl_easy_setopt(curl.get(), key, value);
        if (result != CURLE_OK) { throw LastfmFailure{LastfmFailureKind::https_setup, static_cast<int>(result)}; }
    };
    option(CURLOPT_URL, lastfm_api_endpoint);
    option(CURLOPT_PROTOCOLS_STR, "https");
    option(CURLOPT_REDIR_PROTOCOLS_STR, "https");
    option(CURLOPT_FOLLOWLOCATION, 0L);
    option(CURLOPT_SSL_VERIFYPEER, 1L);
    option(CURLOPT_SSL_VERIFYHOST, 2L);
    // Match OpenSSL's standard explicit trust-store override. AppRun supplies
    // a host store when available, otherwise its bundled fallback. Never disable TLS.
    if (const auto* ca_file = std::getenv("SSL_CERT_FILE"); ca_file && *ca_file) { option(CURLOPT_CAINFO, ca_file); }
    option(CURLOPT_POST, 1L);
    option(CURLOPT_POSTFIELDS, body.c_str());
    option(CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
    option(CURLOPT_USERAGENT, application_user_agent);
    option(CURLOPT_CONNECTTIMEOUT_MS, 3000L);
    option(CURLOPT_TIMEOUT_MS, 8000L);
    option(CURLOPT_NOSIGNAL, 1L);
    option(CURLOPT_NETRC, static_cast<long>(CURL_NETRC_IGNORED));
    option(CURLOPT_WRITEFUNCTION, write_body);
    option(CURLOPT_WRITEDATA, &response.body);
    option(CURLOPT_NOPROGRESS, 0L);
    option(CURLOPT_XFERINFOFUNCTION, progress);
    option(CURLOPT_XFERINFODATA, &cancelled);
    const auto result = curl_easy_perform(curl.get());
    if (result != CURLE_OK) { throw LastfmFailure{LastfmFailureKind::https_transfer, static_cast<int>(result)}; }
    const auto info = curl_easy_getinfo(curl.get(), CURLINFO_RESPONSE_CODE, &response.http_status);
    if (info != CURLE_OK) { throw LastfmFailure{LastfmFailureKind::https_transfer, static_cast<int>(info)}; }
    return response;
}
}  // namespace osu_player
