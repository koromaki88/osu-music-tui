#pragma once

#include "osu_player/library.hpp"

#include <istream>
#include <stdexcept>
#include <string>
#include <vector>

namespace osu_player {

inline constexpr int library_schema_version = 1;

class LibraryFormatError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

struct LibraryLoadResult {
    Library library;
    int schema_version{};
    std::string lazer_version;
    std::vector<std::string> warnings;
};

[[nodiscard]] LibraryLoadResult load_library_ndjson(std::istream& input);

}  // namespace osu_player
