#include "osu_player/library_view.hpp"
#include "osu_player/terminal_text.hpp"

#include <clocale>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

void check(const bool condition, const char* message)
{
    if (!condition) {
        throw std::runtime_error{std::string{"FAIL: "} + message};
    }
}

osu_player::Library make_library(const std::size_t count)
{
    std::vector<osu_player::Track> tracks;
    for (std::size_t index = 0; index < count; ++index) {
        osu_player::Track track;
        track.stable_id = std::to_string(index);
        track.beatmap_set_id = track.stable_id;
        track.audio_hash = std::string(64, 'a');
        track.artist = "Artist";
        track.title = index == 4 ? "Unique 日本" : "Song " + std::to_string(index);
        tracks.push_back(std::move(track));
    }
    return osu_player::Library{std::move(tracks)};
}

void test_selection_and_viewport()
{
    const auto library = make_library(100);
    osu_player::LibraryView view{library};
    view.set_page_size(10);
    check(view.selected_index() == 0 && view.top() == 0, "initial selection is first result");
    view.move(-1);
    check(view.selected_row() == 0, "up at first row does not underflow");
    view.page_down();
    check(view.selected_row() == 10 && view.top() == 1, "page-down scrolls selection into view");
    view.end();
    check(view.selected_row() == 99 && view.top() == 90, "end shows the final page");
    view.move(std::numeric_limits<int>::max());
    check(view.selected_row() == 99, "large positive movement is clamped");
    view.move(std::numeric_limits<int>::min());
    check(view.selected_row() == 0 && view.top() == 0, "INT_MIN movement does not overflow");
    view.end();
    view.set_page_size(200);
    check(view.top() == 0 && view.selected_row() == 99, "resize preserves selection and repairs viewport");
    view.set_page_size(0);
    check(view.top() == 99, "a zero-row viewport is treated as one row");
    view.page_up();
    check(view.selected_row() == 98, "page-up moves by viewport size");
    view.home();
    check(view.selected_row() == 0, "home returns to the first result");
}

void test_filtering_and_empty_results()
{
    const auto library = make_library(10'001);
    osu_player::LibraryView view{library};
    view.end();
    const auto previous = view.selected_index();
    view.set_query("UNIQUE");
    check(view.matches().size() == 1 && view.selected_index() == previous,
          "filter preserves selection when its track remains visible");
    view.set_query("日本");
    check(view.matches().size() == 1, "search can match Unicode text");
    view.set_query("not found anywhere");
    check(!view.selected_index() && view.top() == 0, "no matches has no selection");
    view.move(1);
    view.page_down();
    view.page_up();
    view.end();
    view.home();
    check(!view.selected_index(), "navigation on no results is safe");
    view.set_query("");
    check(view.matches().size() == 10'001 && view.selected_index() == 0,
          "clearing an empty filter safely selects the first entry");

    const osu_player::Library empty;
    osu_player::LibraryView empty_view{empty};
    empty_view.end();
    empty_view.set_query("anything");
    check(!empty_view.selected_index(), "empty library cannot acquire a selection");
}

void test_scoped_view()
{
    const auto library = make_library(20);
    const auto unique = library.search("Unique").front();
    osu_player::LibraryView view{library};
    view.set_scope(std::vector<std::size_t>{8,2,unique});
    check(view.matches()==std::vector<std::size_t>({8,2,unique}),"browser preserves playlist order");
    view.set_query("Unique");
    check(view.matches()==std::vector<std::size_t>{unique},"search intersects scope without adding unrelated results");
    view.set_query(""); view.set_scope(std::vector<std::size_t>{});
    check(view.matches().empty() && !view.selected_index(),"empty favorites/playlist has no selection");
    view.set_scope(std::nullopt);
    check(view.matches().size()==20,"full library restored independently of queue");
}

void test_terminal_text()
{
    using osu_player::wrap_terminal_text;
    check(wrap_terminal_text("日本abc", 4) == std::vector<std::string>{"日本", "abc"}, "wrap CJK by cells");
    check(wrap_terminal_text("a\xCC\x81" "bc", 1) == std::vector<std::string>{"a\xCC\x81", "b", "c"}, "wrap preserves combining marks");
    check(wrap_terminal_text("日本", 1) == std::vector<std::string>{"?", "?"}, "one-cell wrap makes progress");
    check(wrap_terminal_text("a\x1b\nb", 2) == std::vector<std::string>{"a ", " b"}, "wrap sanitizes controls");
    check(wrap_terminal_text("text", 0).empty(), "zero width wrap is empty");
    check(wrap_terminal_text(std::string(20000, 'x'), 100).size() == 82, "wrap bounds hostile metadata");
    using osu_player::fit_terminal_text;
    check(fit_terminal_text("abcdef", 3) == L"abc", "ASCII text clips by columns");
    check(fit_terminal_text("日本x", 3) == L"日", "a wide character is not split to fit one remaining column");
    check(fit_terminal_text("日本x", 4) == L"日本", "two CJK characters occupy four columns");
    check(fit_terminal_text("a\xCC\x81" "b", 1) == L"a\u0301", "combining mark stays with its preceding base");
    check(fit_terminal_text("\xCC\x81" "a", 1) == L"a", "leading combining mark cannot alter adjacent UI text");
    check(fit_terminal_text("a\x1b[2J\nb", 30) == L"a [2J b", "metadata cannot inject terminal escapes or newlines");
    check(fit_terminal_text(std::string{"a\0b", 3}, 3) == L"a b", "embedded NUL is neutralized");
    check(fit_terminal_text("\xFFx", 2) == L"\uFFFDx", "malformed UTF-8 is replaced safely");
    check(fit_terminal_text("\xED\xA0\x80", 3) == L"\uFFFD", "UTF-8 surrogates are rejected");
    check(fit_terminal_text("text", 0).empty(), "zero-width output is empty");
    std::string query = "a日本";
    osu_player::erase_last_utf8(query);
    check(query == "a日", "backspace removes a full UTF-8 codepoint");
    osu_player::erase_last_utf8(query);
    osu_player::erase_last_utf8(query);
    osu_player::erase_last_utf8(query);
    check(query.empty(), "backspace is safe on empty text");
    check(osu_player::codepoint_utf8(U'日') == "日", "wide input encodes to UTF-8");
    check(osu_player::codepoint_utf8(0x110000) == "\xEF\xBF\xBD", "invalid input codepoints become replacement characters");
}

void test_refreshed_view()
{
    auto library = make_library(100);
    osu_player::LibraryView view{library};
    view.set_query("song");
    view.end();
    const auto id = library.tracks()[*view.selected_index()].stable_id;
    auto tracks = library.tracks();
    tracks[*view.selected_index()].title = "Song AAA";
    library = osu_player::Library{std::move(tracks)};
    view.refresh(id);
    check(view.query() == "song" && library.tracks()[*view.selected_index()].stable_id == id,
          "refresh preserves filter and remaps selection by ID");
    library = osu_player::Library{};
    view.refresh(id);
    check(view.matches().empty() && !view.selected_index() && view.top() == 0,
          "refresh handles removal of all visible tracks");
}

}  // namespace

int main()
{
    try {
        if (!std::setlocale(LC_CTYPE, "C.UTF-8") && !std::setlocale(LC_CTYPE, "C.utf8")) {
            throw std::runtime_error{"tests require a UTF-8 locale"};
        }
        test_selection_and_viewport();
        test_filtering_and_empty_results();
        test_scoped_view();
        test_terminal_text();
        test_refreshed_view();
        std::cout << "all TUI model/text tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
