#include "osu_player/storage_path.hpp"

#include <cstdlib>
#include <stdexcept>

namespace osu_player {
namespace {

[[nodiscard]] bool is_hexadecimal(const char character) noexcept
{
    return (character >= '0' && character <= '9') ||
           (character >= 'a' && character <= 'f') ||
           (character >= 'A' && character <= 'F');
}

[[nodiscard]] char lowercase_hexadecimal(const char character) noexcept
{
    if (character >= 'A' && character <= 'F') {
        return static_cast<char>(character + ('a' - 'A'));
    }

    return character;
}

}  // namespace

bool is_sha256(const std::string_view value) noexcept
{
    if (value.size() != 64) {
        return false;
    }

    for (const char character : value) {
        if (!is_hexadecimal(character)) {
            return false;
        }
    }

    return true;
}

std::string normalize_sha256(const std::string_view value)
{
    if (!is_sha256(value)) {
        throw std::invalid_argument{"a SHA-256 hash must contain exactly 64 hexadecimal characters"};
    }

    std::string normalized;
    normalized.reserve(value.size());

    for (const char character : value) {
        normalized.push_back(lowercase_hexadecimal(character));
    }

    return normalized;
}

std::filesystem::path content_path(
    const std::filesystem::path& osu_data_directory,
    const std::string_view hash)
{
    const std::string normalized = normalize_sha256(hash);
    return osu_data_directory / "files" / normalized.substr(0, 1) / normalized.substr(0, 2) / normalized;
}

std::filesystem::path default_osu_data_directory()
{
    if (const char* const home = std::getenv("HOME"); home != nullptr && home[0] != '\0') {
        return std::filesystem::path{home} / ".local" / "share" / "osu";
    }

    throw std::runtime_error{"HOME is not set; pass the osu! data directory explicitly"};
}

}  // namespace osu_player
