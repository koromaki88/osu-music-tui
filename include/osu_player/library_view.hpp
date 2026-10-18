#pragma once

#include "osu_player/library.hpp"

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace osu_player {

// Pure selection/viewport state. The referenced library object must outlive
// this view; call refresh after replacing its contents. Filtering never reorders
// the controller's playback queue.
class LibraryView {
public:
    explicit LibraryView(const Library& library);
    void set_query(std::string query);
    void set_page_size(std::size_t rows);
    void move(int delta);
    void home();
    void end();
    void page_up();
    void page_down();
    // Call after the referenced Library is replaced; old row indices are stale.
    void refresh(std::string_view selected_stable_id);

    [[nodiscard]] const std::string& query() const noexcept;
    [[nodiscard]] const std::vector<std::size_t>& matches() const noexcept;
    [[nodiscard]] std::optional<std::size_t> selected_index() const noexcept;
    [[nodiscard]] std::size_t selected_row() const noexcept;
    [[nodiscard]] std::size_t top() const noexcept;

private:
    void ensure_visible();

    const Library& library_;
    std::string query_;
    std::vector<std::size_t> matches_;
    std::size_t selected_{};
    std::size_t top_{};
    std::size_t page_size_{1};
};

}  // namespace osu_player
