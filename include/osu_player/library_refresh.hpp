#pragma once

#include "osu_player/library_loader.hpp"

#include <atomic>
#include <chrono>
#include <deque>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace osu_player {

struct RefreshOptions {
    // An export reload needs no adapter or cache. Otherwise index osu_directory.
    std::optional<std::filesystem::path> export_file;
    std::filesystem::path osu_directory;
    std::vector<std::string> adapter_command;
    bool use_cache{true};
    std::chrono::milliseconds timeout{std::chrono::minutes{5}};
};

struct RefreshEvent {
    std::optional<LibraryLoadResult> loaded;
    std::string message;
};

// One owned worker; only immutable results cross the mutex-protected mailbox.
// No controller, audio, terminal, or original database handles enter this class.
class LibraryRefresh {
public:
    explicit LibraryRefresh(RefreshOptions options);
    ~LibraryRefresh();
    LibraryRefresh(const LibraryRefresh&) = delete;
    LibraryRefresh& operator=(const LibraryRefresh&) = delete;
    bool start(bool try_cache = false);
    [[nodiscard]] bool busy() const noexcept;
    [[nodiscard]] std::optional<RefreshEvent> poll();

private:
    void run(std::stop_token stop, bool try_cache);
    void publish(RefreshEvent event);

    RefreshOptions options_;
    std::atomic<bool> busy_{};
    std::mutex mutex_;
    std::deque<RefreshEvent> events_;
    std::jthread worker_; // Joins before mailbox destruction.
};

[[nodiscard]] std::vector<std::string> default_indexer_command();

}  // namespace osu_player
