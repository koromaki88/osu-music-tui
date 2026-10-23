#include "osu_player/tui_command.hpp"

#include "osu_player/library_loader.hpp"
#include "osu_player/library_view.hpp"
#include "osu_player/player_controller.hpp"
#include "osu_player/settings.hpp"
#include "osu_player/discord_presence.hpp"
#include "osu_player/lastfm_presence.hpp"
#include "osu_player/terminal_text.hpp"
#include "osu_player/collections.hpp"
#include "track_details_probe.hpp"
#include "tui_panels.hpp"

#include <curses.h>
#include <unistd.h>

extern "C" {
#include <libavutil/log.h>
}

#include <algorithm>
#include <array>
#include <chrono>
#include <clocale>
#include <csignal>
#include <cstdint>
#include <cwctype>
#include <fstream>
#include <functional>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace osu_player {
namespace {
using tui_detail::InfoPanel;
using tui_detail::format_time;
using tui_detail::panel_content;

volatile std::sig_atomic_t interrupted = 0;
void request_exit(const int signal) { interrupted = signal; }

class SignalGuard {
public:
    SignalGuard()
    {
        interrupted = 0;
        for (auto& entry : handlers_) {
            entry.second = std::signal(entry.first, request_exit);
        }
    }
    ~SignalGuard()
    {
        for (const auto& entry : handlers_) {
            if (entry.second != SIG_ERR) {
                std::signal(entry.first, entry.second);
            }
        }
    }
private:
    using Handler = void (*)(int);
    std::array<std::pair<int, Handler>, 3> handlers_{{{SIGINT, SIG_DFL}, {SIGTERM, SIG_DFL}, {SIGHUP, SIG_DFL}}};
};

struct ScreenDeleter {
    void operator()(SCREEN* screen) const noexcept
    {
        endwin();
        delscreen(screen);
    }
};

class Terminal {
public:
    Terminal()
    {
        if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) {
            throw std::runtime_error{"tui requires an interactive terminal on stdin and stdout"};
        }
        if (!std::setlocale(LC_CTYPE, "")) {
            throw std::runtime_error{"could not initialize terminal locale; try LANG=C.UTF-8"};
        }
        // newterm reports initialization failure instead of initscr's process exit.
        screen_.reset(newterm(nullptr, stdout, stdin));
        if (!screen_) {
            throw std::runtime_error{"could not initialize ncurses; check TERM and installed terminfo"};
        }
        if (cbreak() == ERR || noecho() == ERR || keypad(stdscr, true) == ERR) {
            throw std::runtime_error{"could not configure terminal input"};
        }
        curs_set(0);
        wtimeout(stdscr, 10);
        set_escdelay(25);
        if (has_colors() && start_color() == OK && COLOR_PAIRS >= 4) {
            const short background = use_default_colors() == OK ? -1 : COLOR_BLACK;
            init_pair(1, COLOR_CYAN, background);
            init_pair(2, COLOR_GREEN, background);
            init_pair(3, COLOR_YELLOW, background);
            colors = true;
        }
    }

    bool colors{};
private:
    std::unique_ptr<SCREEN, ScreenDeleter> screen_;
};

// Decoder diagnostics must not write over the ncurses screen. User-facing
// exceptions still reach the status line; CLI logging is restored on return.
class BackendLogGuard {
public:
    BackendLogGuard() : previous_{av_log_get_level()} { av_log_set_level(AV_LOG_QUIET); }
    ~BackendLogGuard() { av_log_set_level(previous_); }
private:
    int previous_;
};

std::string_view state_name(const PlayerState state)
{
    switch (state) {
        case PlayerState::empty: return "empty";
        case PlayerState::stopped: return "stopped";
        case PlayerState::playing: return "playing";
        case PlayerState::paused: return "paused";
        case PlayerState::finished: return "finished";
        case PlayerState::failed: return "failed";
    }
    return "unknown";
}

std::string_view repeat_name(const RepeatMode mode)
{
    return mode == RepeatMode::all ? "all" : mode == RepeatMode::one ? "one" : "off";
}

void draw(const int row, const int column, const std::string_view text, const int width,
          const attr_t attributes = A_NORMAL, const short color = 0)
{
    int rows{};
    int columns{};
    getmaxyx(stdscr, rows, columns);
    if (row < 0 || row >= rows || column < 0 || column >= columns - 1) {
        return;
    }
    const auto fitted = fit_terminal_text(text, std::min(width, columns - column - 1));
    if (!fitted.empty()) {
        wattr_set(stdscr, attributes, color, nullptr);
        mvwaddnwstr(stdscr, row, column, fitted.c_str(), static_cast<int>(fitted.size()));
        wattr_set(stdscr, A_NORMAL, 0, nullptr);
    }
}

struct DiscordSetup {
    bool active{};
    std::string input;
    std::string error;
};

void render_panel(InfoPanel& panel, const int rows, const int columns, const bool colors, const CollectionsStatus& collections)
{
    const int width = std::min(columns - 2, 96);
    const int left = (columns - width) / 2;
    const int height = rows - 10;
    if (panel.wrapped_width != width) {
        panel.lines.clear();
        for (const auto& paragraph : panel_content(panel, collections)) {
            auto wrapped = wrap_terminal_text(paragraph, width);
            panel.lines.insert(panel.lines.end(), std::make_move_iterator(wrapped.begin()), std::make_move_iterator(wrapped.end()));
        }
        panel.wrapped_width = width;
    }
    const auto& lines = panel.lines;
    panel.page = static_cast<std::size_t>(height);
    panel.maximum = lines.size() > panel.page ? lines.size() - panel.page : 0;
    panel.scroll = std::min(panel.scroll, panel.maximum);
    if (panel.kind == InfoPanel::Kind::playlists && panel.follow_selection) {
        for (std::size_t i = 0; i < lines.size(); ++i) {
            if (lines[i].starts_with("> ")) {
                if (i < panel.scroll) { panel.scroll = i; }
                else if (i >= panel.scroll + panel.page) { panel.scroll = i - panel.page + 1; }
                break;
            }
        }
        panel.follow_selection = false;
    }
    draw(2, left, panel.kind == InfoPanel::Kind::help ? "Help" : panel.kind == InfoPanel::Kind::playlists ? "Playlists" : "Track details (selected track)", width, A_BOLD, colors ? 1 : 0);
    const auto input_lines = panel.name_edit != InfoPanel::NameEdit::none ? wrap_terminal_text(panel.input + "_", width - 10) : std::vector<std::string>{};
    const auto input_line = input_lines.empty() ? std::string{} : "Name: " + std::string{input_lines.size() > 1 ? "..." : ""} + input_lines.back();
    draw(3, left, panel.name_edit != InfoPanel::NameEdit::none ? input_line :
         panel.confirm_delete ? "Delete playlist? y confirms; any other key cancels" :
         std::string{panel.kind == InfoPanel::Kind::playlists ? "Esc/q close | j/k select | " : "Esc/q close | j/k scroll | "} + std::to_string(panel.scroll + 1) + "-" +
         std::to_string(std::min(lines.size(), panel.scroll + panel.page)) + "/" + std::to_string(lines.size()), width);
    for (std::size_t row = 0; row < panel.page && panel.scroll + row < lines.size(); ++row) {
        draw(4 + static_cast<int>(row), left, lines[panel.scroll + row], width);
    }
}

enum class SetupResult { editing, cancelled, accepted };

struct LastfmSetup {
    enum class Field { menu, api_key, secret };
    bool active{};
    Field field{Field::menu};
    std::string input;
    std::string api_key;
    std::string error;
};

std::string_view lastfm_phase_name(const LastfmPhase phase)
{
    switch (phase) {
        case LastfmPhase::off: return "off";
        case LastfmPhase::loading: return "working";
        case LastfmPhase::needs_credentials: return "needs credentials";
        case LastfmPhase::needs_authorization: return "needs authorization";
        case LastfmPhase::awaiting_authorization: return "authorize in browser";
        case LastfmPhase::connected: return "connected";
        case LastfmPhase::error: return "error (L for details)";
    }
    return "unavailable";
}

void lastfm_setup_key(const wint_t key, const bool special, LastfmSetup& setup,
                      LastfmPresence& lastfm, bool& enabled)
{
    if (!special && key == 27) { setup = {}; return; }
    const bool enter = (!special && (key == '\n' || key == '\r')) || (special && key == KEY_ENTER);
    if (setup.field != LastfmSetup::Field::menu) {
        if (enter) {
            if (!valid_lastfm_key(setup.input)) { setup.error = "Enter exactly 32 hexadecimal characters."; return; }
            if (setup.field == LastfmSetup::Field::api_key) {
                setup.api_key = std::move(setup.input); setup.input.clear();
                setup.field = LastfmSetup::Field::secret; setup.error.clear();
            } else {
                lastfm.set_credentials(std::move(setup.api_key), std::move(setup.input));
                setup = {true, LastfmSetup::Field::menu, {}, {}, {}};
            }
        } else if ((special && key == KEY_BACKSPACE) || (!special && (key == 127 || key == 8))) {
            erase_last_utf8(setup.input); setup.error.clear();
        } else if (!special && key == 21) { setup.input.clear(); setup.error.clear(); }
        else if (!special && key >= 32 && key <= 126 && setup.input.size() < 32) {
            setup.input += static_cast<char>(key); setup.error.clear();
        }
        return;
    }
    if (!special && key == 'c') { setup.field = LastfmSetup::Field::api_key; setup.input.clear(); }
    else if (!special && (key == 'd' || key == 'x')) {
        enabled = false;
        if (key == 'x') { lastfm.disconnect(); } else { lastfm.configure(false); }
        setup = {};
    } else if (enter) {
        const auto status = lastfm.status();
        if (status.phase == LastfmPhase::needs_credentials) { setup.field = LastfmSetup::Field::api_key; }
        else if (status.phase == LastfmPhase::awaiting_authorization) { lastfm.complete_authorization(); }
        else if (status.phase == LastfmPhase::needs_authorization || status.phase == LastfmPhase::error) { lastfm.authorize(); }
        else if (status.phase == LastfmPhase::connected) { setup = {}; }
    }
}

SetupResult discord_setup_key(const wint_t key, const bool special, DiscordSetup& setup)
{
    if (!special && key == 27) { return SetupResult::cancelled; }
    if ((!special && (key == '\n' || key == '\r')) || (special && key == KEY_ENTER)) {
        if (valid_discord_application_id(setup.input)) { return SetupResult::accepted; }
        setup.error = "Invalid Application ID: enter 17-20 decimal digits (not a token).";
    } else if ((special && key == KEY_BACKSPACE) || (!special && (key == 127 || key == 8))) {
        erase_last_utf8(setup.input);
        setup.error.clear();
    } else if (!special && key == 21) { // Ctrl-U
        setup.input.clear();
        setup.error.clear();
    } else if (!special && std::iswprint(key)) {
        const auto encoded = codepoint_utf8(static_cast<char32_t>(key));
        if (setup.input.size() + encoded.size() <= 32) {
            setup.input += encoded;
            setup.error.clear();
        }
    }
    return SetupResult::editing;
}

void render(const PlayerController& player, const LibraryView& view, const bool searching,
            const std::string& notice, const bool colors, const bool refreshing, const std::string& discord_status,
            const DiscordSetup& discord_setup, const LastfmStatus& lastfm_status, const LastfmSetup& lastfm_setup,
            InfoPanel& panel, const CollectionsStatus& collections, const std::string& browse_source,
            const std::string& queue_source)
{
    int rows{};
    int columns{};
    getmaxyx(stdscr, rows, columns);
    erase();
    if (rows < 14 || columns < 50) {
        draw(0, 0, "Terminal too small", columns, A_BOLD);
        draw(1, 0, panel.kind != InfoPanel::Kind::none ? "Resize to at least 50x14; Esc closes panel" :
              discord_setup.active || lastfm_setup.active ? "Resize to at least 50x14; Esc cancels setup" :
             "Resize to at least 50x14; q quits", columns);
        wnoutrefresh(stdscr);
        doupdate();
        return;
    }

    draw(0, 0, "osu! music player | Discord: " + discord_status + " | Last.fm: " +
         std::string{lastfm_phase_name(lastfm_status.phase)}, columns, A_BOLD, colors ? 1 : 0);
    const std::string summary = "Library: " + std::to_string(player.library().tracks().size()) +
        " | Matches: " + std::to_string(view.matches().size()) +
        " | Shuffle: " + (player.shuffle() ? "on" : "off") +
         " | Repeat: " + std::string{repeat_name(player.repeat())} + (refreshing ? " | Index: running" : "");
    draw(1, 0, summary, columns);
    if (panel.kind == InfoPanel::Kind::none) {
        draw(2, 0, discord_setup.active ? "Discord Application ID: " + discord_setup.input + "_" :
             "Search: " + view.query() + (searching ? "_  [Enter apply / Esc cancel]" : ""),
             columns, searching ? A_BOLD : A_NORMAL);
        draw(3, 0, discord_setup.active ? "Enter save & enable | Esc cancel | Ctrl-U clear | Use Application ID, not token" :
             browse_source == "Library" ? "Library (Enter plays selection; * marks current track)" :
             "View: " + browse_source + " | Enter plays this list | * current", columns);
    }

    const auto visible_rows = static_cast<std::size_t>(rows - 10);
    if (panel.kind == InfoPanel::Kind::none && !lastfm_setup.active && view.matches().empty()) {
        draw(4, 1, player.library().tracks().empty() ? "Library is empty" : "No matching tracks", columns - 2);
    }
    for (std::size_t offset = 0; panel.kind == InfoPanel::Kind::none && !lastfm_setup.active && offset < visible_rows && view.top() + offset < view.matches().size(); ++offset) {
        const std::size_t match_row = view.top() + offset;
        const std::size_t index = view.matches()[match_row];
        const Track& track = player.library().tracks()[index];
        const bool current = player.current_index() == index;
        const std::string line = std::string{match_row == view.selected_row() ? "> " : "  "} +
            (current ? "* " : "  ") + std::to_string(index + 1) + "  " +
            (collections.data->favorite(track) ? "[fav] " : "") +
            track.display_artist() + " — " + track.display_title();
        attr_t attributes = match_row == view.selected_row() ? A_REVERSE : A_NORMAL;
        if (current) {
            attributes |= A_BOLD;
        }
        draw(4 + static_cast<int>(offset), 0, line, columns, attributes, colors && current ? 2 : 0);
    }
    if (panel.kind != InfoPanel::Kind::none) { render_panel(panel, rows, columns, colors, collections); }

    if (lastfm_setup.active) {
        draw(2, 0, std::string(static_cast<std::size_t>(columns - 1), ' '), columns);
        draw(3, 0, std::string(static_cast<std::size_t>(columns - 1), ' '), columns);
        draw(2, 0, "Last.fm setup — Now Playing and scrobbling | pending: " + std::to_string(lastfm_status.pending) +
             " | unsaved: " + std::to_string(lastfm_status.dropped), columns, A_BOLD);
        const bool editing = lastfm_setup.field != LastfmSetup::Field::menu;
        const auto entry = lastfm_setup.field == LastfmSetup::Field::secret ?
            "API secret: " + std::string(lastfm_setup.input.size(), '*') + "_" : "API key: " + lastfm_setup.input + "_";
        draw(3, 0, editing ? entry : lastfm_status.message, columns);
        draw(4, 0, editing ? "Enter continue | Esc cancel | Ctrl-U clear (never enter a Last.fm password)" :
             "Enter authorize/confirm | Esc close | c credentials | d disable | x disconnect", columns);
        const auto url = editing ? std::string{} : lastfm_status.authorization_url;
        if (!editing && url.empty()) { draw(5, 0, lastfm_status.scrobble_status, columns); }
        const auto width = static_cast<std::size_t>(columns - 1);
        for (std::size_t offset = 0; offset < url.size() && offset / width < static_cast<std::size_t>(rows - 11); offset += width) {
            draw(5 + static_cast<int>(offset / width), 0, url.substr(offset, width), columns);
        }
    }
    const Track* current = player.current_track();
    draw(rows - 6, 0, current ? "Now: " + current->display_artist() + " — " + current->display_title()
                              : "Now: No track selected", columns, A_BOLD);
    const auto duration = player.duration();
    draw(rows - 5, 0, "State: " + std::string{state_name(player.state())} + " | " +
         format_time(player.position()) + " / " + (duration ? format_time(*duration) : "--:--") +
         " | Volume: " + std::to_string(static_cast<int>(player.volume() * 100.0F + 0.5F)) + "% | Queue: " + queue_source, columns);
    const int bar_width = columns - 4;
    const double fraction = duration && duration->count() > 0
        ? std::clamp(static_cast<double>(player.position().count()) / static_cast<double>(duration->count()), 0.0, 1.0)
        : 0.0;
    const auto filled = static_cast<std::size_t>(static_cast<double>(bar_width) * fraction);
    draw(rows - 4, 0, "[" + std::string(filled, '=') +
         std::string(static_cast<std::size_t>(bar_width) - filled, '-') + "]", columns);
    draw(rows - 3, 0, lastfm_setup.active && !lastfm_setup.error.empty() ? lastfm_setup.error :
         discord_setup.active && !discord_setup.error.empty() ? discord_setup.error : notice,
         columns, A_NORMAL, colors ? 3 : 0);
    draw(rows - 2, 0, lastfm_setup.active ? "Get personal API credentials: https://www.last.fm/api/account/create" :
         discord_setup.active ? "Create an app: https://discord.com/developers/applications" :
         panel.name_edit != InfoPanel::NameEdit::none ? "Enter save | Esc cancel | Ctrl-U clear | Playback continues" :
         panel.confirm_delete ? "y confirms deletion | any other key cancels | Playback continues" :
         panel.kind != InfoPanel::Kind::none ? "Space pause  n/p tracks  h/l seek  +/- volume | Esc/q closes panel" :
          "Enter play  Space pause  n/p tracks  ? help  q quit", columns);
    draw(rows - 1, 0, lastfm_setup.active ? (!lastfm_status.authorization_url.empty() ?
         "Open the link in your browser, approve access, then press Enter." :
          "Credentials are private files, not encrypted. Playback continues while editing.") :
          discord_setup.active ? "Copy its Application ID. Playback continues while you edit." :
          panel.kind == InfoPanel::Kind::playlists ? "c new  a add  r rename  x delete  Enter browse  b all" :
          panel.kind != InfoPanel::Kind::none ? "? help  i details | Enter does not play while a panel is open" :
           "h/l seek  +/- volume  / search  s shuffle  R repeat", columns);
    wnoutrefresh(stdscr);
    doupdate();
}

bool normal_key(const wint_t key, const bool special, PlayerController& player,
                LibraryView& view, bool& searching, std::string& saved_query, const std::function<void()>& play_selection)
{
    if (special) {
        switch (key) {
            case KEY_UP: view.move(-1); return true;
            case KEY_DOWN: view.move(1); return true;
            case KEY_PPAGE: view.page_up(); return true;
            case KEY_NPAGE: view.page_down(); return true;
            case KEY_HOME: view.home(); return true;
            case KEY_END: view.end(); return true;
            case KEY_LEFT: player.seek(player.position() - std::chrono::seconds{5}); return true;
            case KEY_RIGHT: player.seek(player.position() + std::chrono::seconds{5}); return true;
            case KEY_ENTER:
                play_selection();
                return true;
            default: return true;
        }
    }
    switch (key) {
        case 'q': return false;
        case 'j': view.move(1); break;
        case 'k': view.move(-1); break;
        case 'g': view.home(); break;
        case 'G': view.end(); break;
        case '\n': case '\r':
            play_selection();
            break;
        case ' ':
            if (player.state() == PlayerState::playing) { player.pause(); }
            else if (player.current_index()) { player.play(); }
            else { play_selection(); }
            break;
        case 'n': player.next(); break;
        case 'p': player.previous(); break;
        case 'h': player.seek(player.position() - std::chrono::seconds{5}); break;
        case 'l': player.seek(player.position() + std::chrono::seconds{5}); break;
        case 'x': player.stop(); break;
        case '+': case '=': player.set_volume(std::min(1.0F, player.volume() + 0.05F)); break;
        case '-': player.set_volume(std::max(0.0F, player.volume() - 0.05F)); break;
        case 's': player.set_shuffle(!player.shuffle()); break;
        case 'R':
            player.set_repeat(player.repeat() == RepeatMode::off ? RepeatMode::one :
                              player.repeat() == RepeatMode::one ? RepeatMode::all : RepeatMode::off);
            break;
        case '/': saved_query = view.query(); searching = true; break;
        case 27: view.set_query(""); break;
        default: break;
    }
    return true;
}

void search_key(const wint_t key, const bool special, LibraryView& view,
                bool& searching, const std::string& saved_query)
{
    if ((special && key == KEY_BACKSPACE) || (!special && (key == 127 || key == 8))) {
        std::string query = view.query();
        erase_last_utf8(query);
        view.set_query(std::move(query));
    } else if ((!special && (key == '\n' || key == '\r')) || (special && key == KEY_ENTER)) {
        searching = false;
    } else if (!special && key == 27) {
        view.set_query(saved_query);
        searching = false;
    } else if (!special && key == 21) { // Ctrl-U
        view.set_query("");
    } else if (special && key == KEY_UP) {
        view.move(-1);
    } else if (special && key == KEY_DOWN) {
        view.move(1);
    } else if (!special && std::iswprint(key)) {
        const auto encoded = codepoint_utf8(static_cast<char32_t>(key));
        if (view.query().size() + encoded.size() <= 256) {
            view.set_query(view.query() + encoded);
        }
    }
}

}  // namespace

int run_tui_command(RefreshOptions options)
{
    PlayerController player{Library{}, make_audio_player()};
    LibraryView view{player.library()};
    Terminal terminal;
    SignalGuard signals;
    BackendLogGuard backend_logs;
    Settings saved_settings;
    std::optional<std::filesystem::path> config_path;
    std::string settings_warning;
    bool settings_writable = true;
    try {
        config_path = settings_path();
        saved_settings = load_settings(*config_path);
    } catch (const std::exception& error) {
        settings_warning = "Settings: " + std::string{error.what()};
        // Preserve malformed/newer files rather than silently replacing them.
        settings_writable = false;
    }
    player.set_volume(saved_settings.volume);
    player.set_shuffle(saved_settings.shuffle);
    player.set_repeat(saved_settings.repeat);
    view.set_query(saved_settings.search);
    bool discord_enabled = saved_settings.discord_enabled;
    std::string discord_application_id = saved_settings.discord_application_id;
    DiscordSetup discord_setup{discord_enabled && !valid_discord_application_id(discord_application_id),
                               discord_application_id, {}};
    std::unique_ptr<DiscordPresence> discord;
    try {
        discord = std::make_unique<DiscordPresence>();
        discord->configure(discord_enabled, discord_application_id);
    } catch (const std::exception&) {
        // Optional integration resource failures must not prevent audio/UI startup.
    }
    bool lastfm_enabled = saved_settings.lastfm_enabled;
    // Restore the integration, not its menu. Setup opens only on explicit input.
    LastfmSetup lastfm_setup;
    std::unique_ptr<LastfmPresence> lastfm;
    try {
        lastfm = std::make_unique<LastfmPresence>(lastfm_credentials_path());
        lastfm->configure(lastfm_enabled);
    } catch (const std::exception&) { }
    const bool export_mode = options.export_file.has_value();
    LibraryRefresh refresh{std::move(options)};
    refresh.start(true);
    std::string notice = export_mode ? "Loading library in background."
        : "Loading/indexing in background. Close lazer before indexing; r retries.";
    bool searching = false;
    InfoPanel panel;
    std::unique_ptr<TrackDetailsProbe> detail_probe;
    std::uint64_t detail_generation{};
    std::optional<TrackError> latest_track_error;
    std::string saved_query;
    std::unique_ptr<CollectionsStore> collections_store;
    CollectionsStatus collections{std::make_shared<Collections>(), true, false, 0, "Collections unavailable"};
    try { collections_store = std::make_unique<CollectionsStore>(settings_path().parent_path() / "collections/lists.json"); }
    catch (const std::exception&) { }
    enum class Browse { library, favorites, playlist };
    Browse browse = Browse::library;
    std::string playlist_name;
    std::string queue_source{"Library"};
    std::optional<std::pair<std::string, std::string>> pending_rename;
    const auto source_name = [&] { return browse == Browse::library ? std::string{"Library"} :
        browse == Browse::favorites ? std::string{"Favorites"} : "Playlist: " + playlist_name; };
    const auto apply_scope = [&](const std::string& selected) {
        if (browse == Browse::library) { view.set_scope(std::nullopt, selected); }
        else { view.set_scope(collections.data->resolve(player.library(), browse == Browse::favorites, playlist_name), selected); }
    };
    const auto select_source = [&](const Browse next, std::string name = {}) {
        std::string selected;
        if (const auto index = view.selected_index()) { selected = player.library().tracks()[*index].stable_id; }
        browse = next; playlist_name = std::move(name); apply_scope(selected);
    };
    const std::function<void()> play_selection = [&] {
        if (const auto selected = view.selected_index()) {
            if (browse == Browse::library && queue_source == "Library") { player.play_track(*selected); }
            else {
                player.play_queue(view.scope() ? *view.scope() : player.library().search(""), *selected, browse != Browse::library);
                queue_source = source_name();
            }
        }
    };
    const auto submit_collection = [&](CollectionEdit edit) {
        if (!collections_store) { notice = collections.message; }
        else if (!collections_store->submit(std::move(edit))) { notice = collections_store->status().message; }
    };
    bool running = true;
    auto next_frame = std::chrono::steady_clock::now();
    auto next_collection_poll = next_frame;
    while (running && !interrupted) {
        if (collections_store && std::chrono::steady_clock::now() >= next_collection_poll) {
            next_collection_poll = std::chrono::steady_clock::now() + std::chrono::milliseconds{100};
            auto updated = collections_store->status();
            if (updated.data != collections.data) {
                std::string selected;
                if (const auto index = view.selected_index()) { selected = player.library().tracks()[*index].stable_id; }
                collections.data = updated.data;
                panel.wrapped_width = 0;
                next_frame = std::chrono::steady_clock::now();
                if (pending_rename) {
                    const bool renamed = std::any_of(collections.data->playlists.begin(), collections.data->playlists.end(),
                        [&](const auto& list) { return list.name == pending_rename->second; }) &&
                        (pending_rename->first == pending_rename->second || std::none_of(collections.data->playlists.begin(), collections.data->playlists.end(),
                            [&](const auto& list) { return list.name == pending_rename->first; }));
                    if (renamed && browse == Browse::playlist && playlist_name == pending_rename->first) { playlist_name = pending_rename->second; }
                    if (renamed || updated.pending == 0) { pending_rename.reset(); }
                }
                if (browse != Browse::library) { apply_scope(selected); }
                if (panel.kind == InfoPanel::Kind::playlists) {
                    const auto& lists = collections.data->playlists;
                    if (!panel.target_name.empty()) {
                        const auto found = std::find_if(lists.begin(), lists.end(), [&](const auto& list) { return list.name == panel.target_name; });
                        if (found != lists.end()) { panel.playlist_row = static_cast<std::size_t>(found - lists.begin()); }
                    }
                    panel.playlist_row = lists.empty() ? 0 : std::min(panel.playlist_row, lists.size() - 1);
                    panel.follow_selection = true;
                }
            }
            if (updated.message != collections.message || updated.pending != collections.pending || updated.ready != collections.ready) {
                if (updated.ready && updated.message != "Collections ready") { notice = updated.message; }
                panel.wrapped_width = 0; next_frame = std::chrono::steady_clock::now();
            }
            collections = std::move(updated);
        }
        if (detail_probe) {
            if (const auto result = detail_probe->poll(); result && result->generation == detail_generation &&
                panel.kind == InfoPanel::Kind::details) {
                if (result->duration) { panel.duration = AudioPlayer::Milliseconds{*result->duration}; }
                panel.duration_status = result->failed ? "Unavailable (probe failed; check local file)" : "Unknown (not reported by audio)";
                panel.wrapped_width = 0;
                next_frame = std::chrono::steady_clock::now();
            }
        }
        while (auto event = refresh.poll()) {
            if (event->loaded) {
                std::string selected_id;
                if (const auto index = view.selected_index()) {
                    selected_id = player.library().tracks()[*index].stable_id;
                }
                player.replace_library(std::move(event->loaded->library));
                apply_scope(selected_id);
                if (!event->loaded->warnings.empty()) {
                    event->message += " Warnings: " + std::to_string(event->loaded->warnings.size()) +
                        "; " + event->loaded->warnings.front();
                }
            }
            notice = std::move(event->message);
            next_frame = std::chrono::steady_clock::now();
        }
        int rows{};
        int columns{};
        getmaxyx(stdscr, rows, columns);
        (void)columns;
        view.set_page_size(static_cast<std::size_t>(std::max(1, rows - 10)));
        const auto now = std::chrono::steady_clock::now();
        if (now >= next_frame) {
            render(player, view, searching, settings_warning.empty() ? notice : settings_warning,
                   terminal.colors, refresh.busy(), discord ? discord->status() : "unavailable", discord_setup,
                     lastfm ? lastfm->status() : LastfmStatus{LastfmPhase::error, "Last.fm unavailable", {}}, lastfm_setup, panel,
                     collections, source_name(), queue_source);
            next_frame = now + std::chrono::milliseconds{100};
        }
        wint_t key{};
        const int result = wget_wch(stdscr, &key);
        try {
            if (result != ERR) {
                if (result == KEY_CODE_YES && key == KEY_RESIZE) {
                    clearok(stdscr, true);
                    panel.follow_selection = true;
                } else if (lastfm_setup.active && lastfm) {
                    lastfm_setup_key(key, result == KEY_CODE_YES, lastfm_setup, *lastfm, lastfm_enabled);
                } else if (discord_setup.active) {
                    const auto action = discord_setup_key(key, result == KEY_CODE_YES, discord_setup);
                    if (action != SetupResult::editing) {
                        discord_setup.active = false;
                        discord_enabled = action == SetupResult::accepted;
                        if (discord_enabled) { discord_application_id = discord_setup.input; }
                        if (discord) { discord->configure(discord_enabled, discord_application_id); }
                    }
                } else if (searching) {
                    search_key(key, result == KEY_CODE_YES, view, searching, saved_query);
                } else if (panel.kind == InfoPanel::Kind::playlists) {
                    const auto action = tui_detail::playlist_key(key, result == KEY_CODE_YES, panel, *collections.data);
                    using Kind = tui_detail::PlaylistAction::Kind;
                    if (action.kind == Kind::edit) {
                        const auto& edit = *action.edit;
                        if (edit.action == CollectionAction::rename && browse == Browse::playlist && playlist_name == edit.name) {
                            pending_rename = std::pair{edit.name, edit.replacement};
                        }
                        submit_collection(edit);
                    } else if (action.kind == Kind::library) { select_source(Browse::library); panel = {}; }
                    else if (action.kind == Kind::browse) { select_source(Browse::playlist, action.name); panel = {}; }
                    else if (action.kind == Kind::close) { panel = {}; }
                    else if (action.kind == Kind::transport) {
                        running = normal_key(key, result == KEY_CODE_YES, player, view, searching, saved_query, play_selection);
                    }
                } else if (result != KEY_CODE_YES && (key == '?' || key == 'i')) {
                    ++detail_generation;
                    if (detail_probe) { detail_probe->cancel(); }
                    const auto kind = key == '?' ? InfoPanel::Kind::help : InfoPanel::Kind::details;
                    if (panel.kind == kind) { panel = {}; }
                    else {
                        panel = {}; panel.kind = kind;
                        if (kind == InfoPanel::Kind::details) {
                            if (const auto selected = view.selected_index()) {
                                panel.track = player.library().tracks()[*selected];
                                if (player.current_index() == selected) { panel.duration = player.duration(); }
                                if (!panel.duration && panel.track->duration_ms && *panel.track->duration_ms > 0) {
                                    panel.duration = AudioPlayer::Milliseconds{*panel.track->duration_ms};
                                }
                                if (!panel.duration) {
                                    try {
                                        if (!detail_probe) { detail_probe = std::make_unique<TrackDetailsProbe>(); }
                                        panel.duration_status = "Loading audio duration...";
                                        detail_probe->request(detail_generation, panel.track->audio_path);
                                    } catch (const std::exception&) { panel.duration_status = "Unavailable (probe worker could not start)"; }
                                }
                                if (latest_track_error && latest_track_error->stable_id == panel.track->stable_id) {
                                    panel.error = latest_track_error->message;
                                }
                            }
                        }
                    }
                } else if (panel.kind != InfoPanel::Kind::none) {
                    const bool special = result == KEY_CODE_YES;
                    if (!special && (key == 27 || key == 'q')) {
                        ++detail_generation;
                        if (detail_probe) { detail_probe->cancel(); }
                        panel = {};
                    }
                    else if ((special && key == KEY_UP) || (!special && key == 'k')) {
                        if (panel.scroll > 0) { --panel.scroll; }
                    } else if ((special && key == KEY_DOWN) || (!special && key == 'j')) {
                        panel.scroll = std::min(panel.maximum, panel.scroll + 1);
                    } else if (special && key == KEY_PPAGE) {
                        panel.scroll -= std::min(panel.scroll, panel.page);
                    } else if (special && key == KEY_NPAGE) {
                        panel.scroll += std::min(panel.maximum - std::min(panel.maximum, panel.scroll), panel.page);
                    } else if ((special && key == KEY_HOME) || (!special && key == 'g')) { panel.scroll = 0; }
                    else if ((special && key == KEY_END) || (!special && key == 'G')) { panel.scroll = panel.maximum; }
                    else if (tui_detail::panel_transport_key(key, special)) {
                        running = normal_key(key, special, player, view, searching, saved_query, play_selection);
                    }
                } else if (result != KEY_CODE_YES && key == 'J') {
                    if (const auto current = player.current_index()) {
                        if (!view.select_index(*current)) {
                            select_source(Browse::library);
                            view.set_query("");
                            saved_query.clear();
                            (void)view.select_index(*current);
                        }
                    } else { notice = "No current track to jump to."; }
                } else if (result != KEY_CODE_YES && key == 'L') {
                    if (lastfm) {
                        lastfm_enabled = true;
                        lastfm->configure(true);
                        lastfm_setup = {true, LastfmSetup::Field::menu, {}, {}, {}};
                    }
                } else if (result != KEY_CODE_YES && key == 'P') {
                    panel = {}; panel.kind = InfoPanel::Kind::playlists;
                    if (const auto selected = view.selected_index()) { panel.track = player.library().tracks()[*selected]; }
                } else if (result != KEY_CODE_YES && key == 'F') {
                    select_source(browse == Browse::favorites ? Browse::library : Browse::favorites);
                } else if (result != KEY_CODE_YES && key == 'b') {
                    select_source(Browse::library);
                } else if (result != KEY_CODE_YES && (key == 'f' || key == 'u' || key == '<' || key == '>')) {
                    if (const auto selected = view.selected_index()) {
                        const auto& track = player.library().tracks()[*selected];
                        if (key == 'f') { submit_collection({CollectionAction::favorite, {}, {}, {track.stable_id, track.audio_hash}}); }
                        else if (browse == Browse::playlist) {
                            submit_collection({key == 'u' ? CollectionAction::remove : key == '<' ? CollectionAction::earlier : CollectionAction::later,
                                playlist_name, {}, {track.stable_id, track.audio_hash}});
                        }
                    }
                } else if (result != KEY_CODE_YES && key == 'd') {
                    if (!discord_enabled && !valid_discord_application_id(discord_application_id)) {
                        discord_setup = {true, discord_application_id, {}};
                    } else {
                        discord_enabled = !discord_enabled;
                        if (discord) { discord->configure(discord_enabled, discord_application_id); }
                    }
                } else if (result != KEY_CODE_YES && key == 'r') {
                    notice = refresh.start() ? (export_mode ? "Reloading library in background."
                        : "Indexing in background; lazer must be closed.") : "Refresh already running.";
                } else {
                    running = normal_key(key, result == KEY_CODE_YES, player, view,
                                          searching, saved_query, play_selection);
                }
                next_frame = std::chrono::steady_clock::now();
            }
            player.update();
        } catch (const AudioOutputError& error) {
            notice = "Audio error: " + std::string{error.what()};
            next_frame = std::chrono::steady_clock::now();
        }
        const auto errors = player.take_errors();
        const auto playback_events = player.take_playback_events();
        if (discord) { discord->publish(playback_events, player.playback_snapshot()); }
        if (lastfm) { lastfm->publish(playback_events, player.playback_snapshot()); }
        const Settings current_settings{player.volume(), player.shuffle(), player.repeat(),
                                         searching ? saved_query : view.query(), discord_enabled, discord_application_id, lastfm_enabled};
        if (settings_writable && current_settings != saved_settings) {
            try {
                save_settings(*config_path, current_settings);
                saved_settings = current_settings;
                settings_warning.clear();
            } catch (const std::exception& error) {
                settings_warning = "Settings not saved: " + std::string{error.what()};
                // Avoid retrying disk I/O every frame; retry after another change.
                saved_settings = current_settings;
            }
            next_frame = std::chrono::steady_clock::now();
        }
        if (!errors.tracks.empty()) {
            latest_track_error = errors.tracks.back();
            notice = "Skipped track: " + errors.tracks.back().message;
            if (errors.omitted) {
                notice += " (" + std::to_string(errors.omitted) + " earlier errors omitted)";
            }
            next_frame = std::chrono::steady_clock::now();
        }
    }
    player.shutdown();
    const auto final_events = player.take_playback_events();
    if (discord) { discord->publish(final_events, player.playback_snapshot()); }
    if (lastfm) { lastfm->publish(final_events, player.playback_snapshot()); }
    return interrupted ? 128 + interrupted : 0;
}

}  // namespace osu_player
