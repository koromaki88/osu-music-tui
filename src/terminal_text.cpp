#include "osu_player/terminal_text.hpp"

#include <cstddef>
#include <cwchar>

namespace osu_player {
namespace {

bool continuation(const unsigned char value) { return (value & 0xC0U) == 0x80U; }

char32_t decode(std::string_view text, std::size_t& offset)
{
    const auto first = static_cast<unsigned char>(text[offset++]);
    if (first < 0x80U) {
        return first;
    }
    int tail{};
    char32_t codepoint{};
    char32_t minimum{};
    if (first >= 0xC2U && first <= 0xDFU) {
        tail = 1; codepoint = first & 0x1FU; minimum = 0x80;
    } else if (first >= 0xE0U && first <= 0xEFU) {
        tail = 2; codepoint = first & 0x0FU; minimum = 0x800;
    } else if (first >= 0xF0U && first <= 0xF4U) {
        tail = 3; codepoint = first & 0x07U; minimum = 0x10000;
    } else {
        return 0xFFFD;
    }
    const auto start = offset;
    for (int index = 0; index < tail; ++index) {
        if (offset >= text.size() || !continuation(static_cast<unsigned char>(text[offset]))) {
            offset = start; // Only the malformed leading byte is consumed.
            return 0xFFFD;
        }
        codepoint = (codepoint << 6U) | (static_cast<unsigned char>(text[offset++]) & 0x3FU);
    }
    if (codepoint < minimum || codepoint > 0x10FFFF || (codepoint >= 0xD800 && codepoint <= 0xDFFF)) {
        return 0xFFFD;
    }
    return codepoint;
}

}  // namespace

std::wstring fit_terminal_text(const std::string_view utf8, const int columns)
{
    std::wstring result;
    if (columns <= 0) {
        return result;
    }
    int used = 0;
    std::size_t offset = 0;
    while (offset < utf8.size()) {
        if (result.size() >= 8192) {
            break; // Bound even metadata made entirely of zero-width characters.
        }
        char32_t codepoint = decode(utf8, offset);
        if (codepoint < 0x20 || (codepoint >= 0x7F && codepoint <= 0x9F) ||
            (codepoint >= 0x202A && codepoint <= 0x202E) ||
            (codepoint >= 0x2066 && codepoint <= 0x2069)) {
            codepoint = U' ';
        }
        wchar_t character = static_cast<wchar_t>(codepoint);
        int width = ::wcwidth(character);
        if (width < 0) {
            character = L'?';
            width = 1;
        }
        if (width > columns - used) {
            break;
        }
        if (width == 0 && result.empty()) {
            continue; // Don't let a leading combining mark modify adjacent UI text.
        }
        result.push_back(character);
        used += width;
    }
    return result;
}

std::string codepoint_utf8(char32_t value)
{
    if (value > 0x10FFFF || (value >= 0xD800 && value <= 0xDFFF)) {
        value = 0xFFFD;
    }
    std::string result;
    if (value <= 0x7F) {
        result.push_back(static_cast<char>(value));
    } else if (value <= 0x7FF) {
        result.push_back(static_cast<char>(0xC0U | (value >> 6U)));
        result.push_back(static_cast<char>(0x80U | (value & 0x3FU)));
    } else if (value <= 0xFFFF) {
        result.push_back(static_cast<char>(0xE0U | (value >> 12U)));
        result.push_back(static_cast<char>(0x80U | ((value >> 6U) & 0x3FU)));
        result.push_back(static_cast<char>(0x80U | (value & 0x3FU)));
    } else {
        result.push_back(static_cast<char>(0xF0U | (value >> 18U)));
        result.push_back(static_cast<char>(0x80U | ((value >> 12U) & 0x3FU)));
        result.push_back(static_cast<char>(0x80U | ((value >> 6U) & 0x3FU)));
        result.push_back(static_cast<char>(0x80U | (value & 0x3FU)));
    }
    return result;
}

void erase_last_utf8(std::string& text)
{
    if (text.empty()) {
        return;
    }
    auto offset = text.size() - 1;
    while (offset > 0 && continuation(static_cast<unsigned char>(text[offset]))) {
        --offset;
    }
    text.resize(offset);
}

}  // namespace osu_player
