#include "tui_panels.hpp"
#include "osu_player/terminal_text.hpp"

#include <curses.h>

#include <algorithm>
#include <cwctype>
#include <iomanip>
#include <sstream>

namespace osu_player::tui_detail {
std::string format_time(const AudioPlayer::Milliseconds value)
{
    const auto seconds = std::max(std::int64_t{0}, value.count()) / 1000;
    std::ostringstream result;
    result << std::setw(2) << std::setfill('0') << seconds / 60 << ':' << std::setw(2) << seconds % 60;
    return result.str();
}

std::vector<std::string> panel_content(const InfoPanel& panel, const CollectionsStatus& collections)
{
    if (panel.kind == InfoPanel::Kind::playlists) {
        std::vector<std::string> lines{
            "Enter browse | a add selected track | c create", "r rename | x delete (confirm) | b full library | Esc close",
            collections.message + " | pending: " + std::to_string(collections.pending),
            panel.track ? "Add target: " + panel.track->display_artist() + " — " + panel.track->display_title() : "No selected track to add", ""};
        if (collections.data->playlists.empty()) { lines.push_back("No playlists yet. Press c to create one."); }
        for (std::size_t i = 0; i < collections.data->playlists.size(); ++i) {
            const auto& list = collections.data->playlists[i];
            lines.push_back(std::string{i == panel.playlist_row ? "> " : "  "} + list.name + " [" + std::to_string(list.tracks.size()) + " saved]");
        }
        return lines;
    }
    if (panel.kind == InfoPanel::Kind::help) {
        return {
            "BROWSE AND SEARCH", "j/k or Up/Down: select track. Enter: play selection.",
            "PageUp/PageDown: page. g/G or Home/End: first/last result. J: jump to current track.",
            "/: search. Enter applies; Esc cancels edits or clears an applied filter.",
            "Search: Backspace removes a character; Ctrl-U clears. Filtering does not change the queue.", "",
            "PLAYBACK", "Space: pause/resume (start selection if nothing is loaded).",
            "n/p: next/previous in queue (including shuffle). Left/Right or h/l: seek five seconds. +/-: volume.",
            "s: toggle shuffle. R: repeat off/one/all. x: stop and rewind.",
            "Playback never starts automatically on launch.", "",
            "LIBRARY AND INTEGRATIONS", "r: refresh. Close lazer first; live database snapshots are unsupported.",
            "d: toggle/setup Discord using your Application ID, not a bot token.",
            "L: Last.fm setup. In that panel: c credentials, d disable, x local disconnect.",
            "Last.fm is opt-in; credentials and listening history are private plaintext files.", "",
            "FAVORITES AND PLAYLISTS", "f: toggle favorite. F: favorites/library view. b: full library. P: playlists.",
            "In playlists: c create, a add selected track, r rename, x delete (confirm), Enter browse.",
            "In a playlist browser: u removes selection; < and > move it earlier/later.",
            "Browsing never interrupts playback. Enter starts that list's full queue, independent of search.",
            "Edits affect the browser/saved list; the active queue is a snapshot until Enter plays a list again.", "",
            "PANELS", "?: help. i: selected-track details. Metadata is a snapshot; duration loads in background.",
            "j/k, arrows, PageUp/PageDown, Home/End: scroll this panel, not the library.",
            "Space, n/p, Left/Right or h/l, and +/- still control playback. Other library actions are blocked.",
            "Esc or q: close panel. Outside panels/search: q quits.",
            "Resize at any time; text reflows. Below 50x14, resize to restore the panel."
        };
    }
    if (!panel.track) { return {"No track selected.", "Close this panel, select a library result, then press i."}; }
    const auto& track = *panel.track;
    const auto value = [](const std::string& text) { return text.empty() ? std::string{"Unknown"} : text; };
    std::ostringstream bpm;
    if (track.bpm) { bpm << std::setprecision(8) << *track.bpm; }
    return {
        "Title: " + value(track.display_title()), "Artist: " + value(track.display_artist()),
        "Original title: " + value(track.title), "Original artist: " + value(track.artist),
        "Mapper: " + value(track.creator),
        "Duration: " + (panel.duration ? format_time(*panel.duration) : panel.duration_status),
        "BPM: " + (track.bpm ? bpm.str() : std::string{"Unknown"}),
        "Added: " + (track.date_added ? *track.date_added : std::string{"Unknown"}),
        "Beatmap: " + (track.online_set_id && *track.online_set_id > 0 ?
            "https://osu.ppy.sh/beatmapsets/" + std::to_string(*track.online_set_id) : std::string{"No online beatmap link"}),
        "", "LOCAL FILE",
        "Filename: " + value(track.audio_filename), "Path: " + value(track.audio_path.string()),
        "", "Playback error: " + (panel.error.empty() ? std::string{"No recent error recorded for this track"} : panel.error),
        "If playback failed, check the audio file/output device and select the track to retry.",
    };
}

bool panel_transport_key(const wint_t key, const bool special)
{
    return special ? key == KEY_LEFT || key == KEY_RIGHT :
        key == ' ' || key == '+' || key == '=' || key == '-' || key == 'h' || key == 'l' || key == 'n' || key == 'p';
}

PlaylistAction playlist_key(const wint_t key, const bool special, InfoPanel& panel, const Collections& collections)
{
    using Kind = PlaylistAction::Kind;
    PlaylistAction action;
    const bool enter = (!special && (key == '\n' || key == '\r')) || (special && key == KEY_ENTER);
    const auto& lists = collections.playlists;
    const auto* selected = panel.playlist_row < lists.size() ? &lists[panel.playlist_row] : nullptr;
    if (panel.name_edit != InfoPanel::NameEdit::none) {
        if (!special && key == 27) { panel.name_edit = InfoPanel::NameEdit::none; panel.input.clear(); }
        else if (enter) {
            const bool rename = panel.name_edit == InfoPanel::NameEdit::rename;
            action.kind = Kind::edit;
            action.edit = CollectionEdit{rename ? CollectionAction::rename : CollectionAction::create,
                rename ? panel.target_name : panel.input, panel.input, {}};
            panel.target_name = panel.input;
            panel.name_edit = InfoPanel::NameEdit::none; panel.input.clear();
        } else if ((special && key == KEY_BACKSPACE) || (!special && (key == 127 || key == 8))) { erase_last_utf8(panel.input); }
        else if (!special && key == 21) { panel.input.clear(); }
        else if (!special && std::iswprint(key)) {
            const auto value = codepoint_utf8(static_cast<char32_t>(key));
            if (panel.input.size() + value.size() <= collection_limits::name_bytes) { panel.input += value; }
        }
    } else if (panel.confirm_delete) {
        if (!special && key == 'y') {
            action.kind = Kind::edit;
            action.edit = CollectionEdit{CollectionAction::erase, panel.target_name, {}, {}};
        }
        panel.confirm_delete = false; panel.target_name.clear();
    } else if (!special && (key == 27 || key == 'q' || key == 'P')) { action.kind = Kind::close; }
    else if (!special && key == 'b') { action.kind = Kind::library; }
    else if (enter && selected) { action.kind = Kind::browse; action.name = selected->name; }
    else if (!special && key == 'c') { panel.name_edit = InfoPanel::NameEdit::create; panel.input.clear(); }
    else if (!special && key == 'r' && selected) {
        panel.name_edit = InfoPanel::NameEdit::rename; panel.input = selected->name; panel.target_name = selected->name;
    } else if (!special && key == 'x' && selected) { panel.confirm_delete = true; panel.target_name = selected->name; }
    else if (!special && key == 'a' && selected && panel.track) {
        action.kind = Kind::edit;
        action.edit = CollectionEdit{CollectionAction::add, selected->name, {}, {panel.track->stable_id, panel.track->audio_hash}};
    } else if ((special && (key == KEY_UP || key == KEY_PPAGE)) || (!special && key == 'k')) {
        const auto amount = special && key == KEY_PPAGE ? panel.page : 1;
        panel.playlist_row -= std::min(panel.playlist_row, amount); panel.target_name.clear(); panel.follow_selection = true;
    } else if ((special && (key == KEY_DOWN || key == KEY_NPAGE)) || (!special && key == 'j')) {
        if (!lists.empty()) { panel.playlist_row = std::min(lists.size() - 1, panel.playlist_row + (special && key == KEY_NPAGE ? panel.page : 1)); }
        panel.target_name.clear(); panel.follow_selection = true;
    } else if ((special && key == KEY_HOME) || (!special && key == 'g')) { panel.playlist_row = 0; panel.target_name.clear(); panel.follow_selection = true; }
    else if ((special && key == KEY_END) || (!special && key == 'G')) {
        panel.playlist_row = lists.empty() ? 0 : lists.size() - 1; panel.target_name.clear(); panel.follow_selection = true;
    } else if (panel_transport_key(key, special)) { action.kind = Kind::transport; }
    panel.wrapped_width = 0;
    return action;
}
} // namespace osu_player::tui_detail
