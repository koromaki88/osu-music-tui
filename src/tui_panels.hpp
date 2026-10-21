#pragma once

#include "osu_player/audio_player.hpp"
#include "osu_player/collections.hpp"

#include <cwchar>
#include <optional>
#include <string>
#include <vector>

namespace osu_player::tui_detail {

// Modal state owns its metadata; no curses window, worker or library pointers.
struct InfoPanel {
    enum class Kind { none, help, details, playlists };
    enum class NameEdit { none, create, rename };
    Kind kind{Kind::none};
    std::optional<Track> track;
    std::optional<AudioPlayer::Milliseconds> duration;
    std::string duration_status{"Unknown"};
    std::string error;
    std::size_t scroll{};
    std::size_t maximum{};
    std::size_t page{1};
    int wrapped_width{};
    std::vector<std::string> lines;
    std::size_t playlist_row{};
    NameEdit name_edit{NameEdit::none};
    std::string input;
    std::string target_name;
    bool confirm_delete{};
    bool follow_selection{true};
};

std::string format_time(AudioPlayer::Milliseconds value);
std::vector<std::string> panel_content(const InfoPanel& panel, const CollectionsStatus& collections);
bool panel_transport_key(wint_t key, bool special);

struct PlaylistAction {
    enum class Kind { none, close, library, browse, edit, transport };
    Kind kind{Kind::none};
    std::string name;
    std::optional<CollectionEdit> edit;
};
// Pure input handling: returns intent; the frontend alone dispatches playback
// and worker submissions. Editable names consume keys before global shortcuts.
PlaylistAction playlist_key(wint_t key, bool special, InfoPanel& panel, const Collections& collections);

} // namespace osu_player::tui_detail
