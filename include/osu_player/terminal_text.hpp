#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace osu_player {

// Requires a UTF-8 LC_CTYPE locale for wcwidth(). Clips by terminal columns,
// replaces control/invalid input, and never emits terminal escape characters.
[[nodiscard]] std::wstring fit_terminal_text(std::string_view utf8, int columns);
// Sanitized, cell-width-aware wrapping; bounds each input to the same 8192
// codepoint budget as fit_terminal_text. Never splits a UTF-8 sequence.
[[nodiscard]] std::vector<std::string> wrap_terminal_text(std::string_view utf8, int columns);
[[nodiscard]] std::string codepoint_utf8(char32_t codepoint);
void erase_last_utf8(std::string& text);

}  // namespace osu_player
