#include "osu_player/library_view.hpp"

#include <algorithm>
#include <cstdint>
#include <utility>

namespace osu_player {

LibraryView::LibraryView(const Library& library) : library_{library}, matches_{library.search("")} { }

void LibraryView::set_query(std::string query)
{
    if (query == query_) {
        return;
    }
    const auto previous = selected_index();
    query_ = std::move(query);
    matches_ = library_.search(query_);
    selected_ = 0;
    if (previous) {
        const auto found = std::find(matches_.begin(), matches_.end(), *previous);
        if (found != matches_.end()) {
            selected_ = static_cast<std::size_t>(found - matches_.begin());
        }
    }
    ensure_visible();
}

void LibraryView::set_page_size(const std::size_t rows)
{
    page_size_ = std::max(std::size_t{1}, rows);
    ensure_visible();
}

void LibraryView::move(const int delta)
{
    if (matches_.empty()) {
        return;
    }
    if (delta < 0) {
        const auto steps = static_cast<std::size_t>(-static_cast<std::int64_t>(delta));
        selected_ -= std::min(selected_, steps);
    } else {
        selected_ += std::min(matches_.size() - 1 - selected_, static_cast<std::size_t>(delta));
    }
    ensure_visible();
}

void LibraryView::home() { selected_ = 0; ensure_visible(); }
void LibraryView::end() { selected_ = matches_.empty() ? 0 : matches_.size() - 1; ensure_visible(); }
void LibraryView::page_up() { selected_ -= std::min(selected_, page_size_); ensure_visible(); }

void LibraryView::page_down()
{
    if (!matches_.empty()) {
        selected_ += std::min(matches_.size() - 1 - selected_, page_size_);
    }
    ensure_visible();
}

const std::string& LibraryView::query() const noexcept { return query_; }
const std::vector<std::size_t>& LibraryView::matches() const noexcept { return matches_; }
std::optional<std::size_t> LibraryView::selected_index() const noexcept
{
    return matches_.empty() ? std::nullopt : std::optional{matches_[selected_]};
}
std::size_t LibraryView::selected_row() const noexcept { return selected_; }
std::size_t LibraryView::top() const noexcept { return top_; }

void LibraryView::ensure_visible()
{
    if (matches_.empty()) {
        selected_ = 0;
        top_ = 0;
        return;
    }
    selected_ = std::min(selected_, matches_.size() - 1);
    if (selected_ < top_) {
        top_ = selected_;
    } else if (selected_ - top_ >= page_size_) {
        top_ = selected_ - page_size_ + 1;
    }
    top_ = std::min(top_, matches_.size() - std::min(page_size_, matches_.size()));
}

}  // namespace osu_player
