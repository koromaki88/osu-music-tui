#include "osu_player/track.hpp"

namespace osu_player {

const std::string& Track::display_title() const noexcept
{
    return title_unicode.empty() ? title : title_unicode;
}

const std::string& Track::display_artist() const noexcept
{
    return artist_unicode.empty() ? artist : artist_unicode;
}

}  // namespace osu_player
