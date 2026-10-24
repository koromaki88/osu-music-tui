#pragma once

// Supplied by Make/CMake from the release VERSION file, never read at runtime.
#ifndef OSU_PLAYER_VERSION
#error "Build with Make or CMake to supply OSU_PLAYER_VERSION from VERSION"
#endif

namespace osu_player {
inline constexpr char application_version[] = OSU_PLAYER_VERSION;
inline constexpr char application_user_agent[] = "osu-music-player/" OSU_PLAYER_VERSION;
} // namespace osu_player
