#include "osu_player/tui_command.hpp"

#include "osu_player/library_loader.hpp"
#include "osu_player/library_view.hpp"
#include "osu_player/player_controller.hpp"
#include "osu_player/settings.hpp"
#include "osu_player/terminal_text.hpp"

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
#include <iomanip>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace osu_player {
namespace {

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

std::string format_time(const AudioPlayer::Milliseconds value)
{
    const auto seconds = std::max(std::int64_t{0}, value.count()) / 1000;
    std::ostringstream result;
    result << std::setw(2) << std::setfill('0') << seconds / 60 << ':' << std::setw(2) << seconds % 60;
    return result.str();
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

void render(const PlayerController& player, const LibraryView& view, const bool searching,
            const std::string& notice, const bool colors, const bool refreshing)
{
    int rows{};
    int columns{};
    getmaxyx(stdscr, rows, columns);
    erase();
    if (rows < 14 || columns < 50) {
        draw(0, 0, "Terminal too small", columns, A_BOLD);
        draw(1, 0, "Resize to at least 50x14; q quits", columns);
        wnoutrefresh(stdscr);
        doupdate();
        return;
    }

    draw(0, 0, "osu! music player", columns, A_BOLD, colors ? 1 : 0);
    const std::string summary = "Library: " + std::to_string(player.library().tracks().size()) +
        " | Matches: " + std::to_string(view.matches().size()) +
        " | Shuffle: " + (player.shuffle() ? "on" : "off") +
        " | Repeat: " + std::string{repeat_name(player.repeat())} + (refreshing ? " | Index: running" : "");
    draw(1, 0, summary, columns);
    draw(2, 0, "Search: " + view.query() + (searching ? "_  [Enter apply / Esc cancel]" : ""),
         columns, searching ? A_BOLD : A_NORMAL);
    draw(3, 0, "Library (Enter plays selection; * marks current track)", columns);

    const auto visible_rows = static_cast<std::size_t>(rows - 10);
    if (view.matches().empty()) {
        draw(4, 1, player.library().tracks().empty() ? "Library is empty" : "No matching tracks", columns - 2);
    }
    for (std::size_t offset = 0; offset < visible_rows && view.top() + offset < view.matches().size(); ++offset) {
        const std::size_t match_row = view.top() + offset;
        const std::size_t index = view.matches()[match_row];
        const Track& track = player.library().tracks()[index];
        const bool current = player.current_index() == index;
        const std::string line = std::string{match_row == view.selected_row() ? "> " : "  "} +
            (current ? "* " : "  ") + std::to_string(index + 1) + "  " +
            track.display_artist() + " — " + track.display_title();
        attr_t attributes = match_row == view.selected_row() ? A_REVERSE : A_NORMAL;
        if (current) {
            attributes |= A_BOLD;
        }
        draw(4 + static_cast<int>(offset), 0, line, columns, attributes, colors && current ? 2 : 0);
    }

    const Track* current = player.current_track();
    draw(rows - 6, 0, current ? "Now: " + current->display_artist() + " — " + current->display_title()
                              : "Now: No track selected", columns, A_BOLD);
    const auto duration = player.duration();
    draw(rows - 5, 0, "State: " + std::string{state_name(player.state())} + " | " +
         format_time(player.position()) + " / " + (duration ? format_time(*duration) : "--:--") +
         " | Volume: " + std::to_string(static_cast<int>(player.volume() * 100.0F + 0.5F)) + "%", columns);
    const int bar_width = columns - 4;
    const double fraction = duration && duration->count() > 0
        ? std::clamp(static_cast<double>(player.position().count()) / static_cast<double>(duration->count()), 0.0, 1.0)
        : 0.0;
    const auto filled = static_cast<std::size_t>(static_cast<double>(bar_width) * fraction);
    draw(rows - 4, 0, "[" + std::string(filled, '=') +
         std::string(static_cast<std::size_t>(bar_width) - filled, '-') + "]", columns);
    draw(rows - 3, 0, notice, columns, A_NORMAL, colors ? 3 : 0);
    draw(rows - 2, 0, "j/k move  Enter play  Space pause  n/p next/prev  / search  q quit", columns);
    draw(rows - 1, 0, "Left/Right seek  +/- volume  s shuffle  R repeat  x stop  r refresh", columns);
    wnoutrefresh(stdscr);
    doupdate();
}

bool normal_key(const wint_t key, const bool special, PlayerController& player,
                LibraryView& view, bool& searching, std::string& saved_query)
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
                if (const auto selected = view.selected_index()) { player.play_track(*selected); }
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
            if (const auto selected = view.selected_index()) { player.play_track(*selected); }
            break;
        case ' ':
            if (player.state() == PlayerState::playing) { player.pause(); }
            else if (player.current_index()) { player.play(); }
            else if (const auto selected = view.selected_index()) { player.play_track(*selected); }
            break;
        case 'n': player.next(); break;
        case 'p': player.previous(); break;
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
    const bool export_mode = options.export_file.has_value();
    LibraryRefresh refresh{std::move(options)};
    refresh.start(true);
    std::string notice = export_mode ? "Loading library in background."
        : "Loading/indexing in background. Close lazer before indexing; r retries.";
    bool searching = false;
    std::string saved_query;
    bool running = true;
    auto next_frame = std::chrono::steady_clock::now();
    while (running && !interrupted) {
        while (auto event = refresh.poll()) {
            if (event->loaded) {
                std::string selected_id;
                if (const auto index = view.selected_index()) {
                    selected_id = player.library().tracks()[*index].stable_id;
                }
                player.replace_library(std::move(event->loaded->library));
                view.refresh(selected_id);
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
                   terminal.colors, refresh.busy());
            next_frame = now + std::chrono::milliseconds{100};
        }
        wint_t key{};
        const int result = wget_wch(stdscr, &key);
        try {
            if (result != ERR) {
                if (result == KEY_CODE_YES && key == KEY_RESIZE) {
                    clearok(stdscr, true);
                } else if (searching) {
                    search_key(key, result == KEY_CODE_YES, view, searching, saved_query);
                } else if (result != KEY_CODE_YES && key == 'r') {
                    notice = refresh.start() ? (export_mode ? "Reloading library in background."
                        : "Indexing in background; lazer must be closed.") : "Refresh already running.";
                } else {
                    running = normal_key(key, result == KEY_CODE_YES, player, view,
                                         searching, saved_query);
                }
                next_frame = std::chrono::steady_clock::now();
            }
            player.update();
        } catch (const AudioOutputError& error) {
            notice = "Audio error: " + std::string{error.what()};
            next_frame = std::chrono::steady_clock::now();
        }
        const auto errors = player.take_errors();
        const Settings current_settings{player.volume(), player.shuffle(), player.repeat(),
                                        searching ? saved_query : view.query()};
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
            notice = "Skipped track: " + errors.tracks.back().message;
            if (errors.omitted) {
                notice += " (" + std::to_string(errors.omitted) + " earlier errors omitted)";
            }
            next_frame = std::chrono::steady_clock::now();
        }
    }
    player.shutdown();
    return interrupted ? 128 + interrupted : 0;
}

}  // namespace osu_player
