#include "osu_player/library_loader.hpp"

#include <iostream>
#include <sstream>
#include <string>
#include <string_view>

namespace {

constexpr std::string_view hash_a =
    "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
constexpr std::string_view hash_b =
    "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";

int failures = 0;

void expect(const bool condition, const std::string_view message)
{
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

template <typename Function>
void expect_format_error(Function function, const std::string_view message)
{
    try {
        function();
        expect(false, message);
    } catch (const osu_player::LibraryFormatError&) {
        // Expected.
    } catch (...) {
        expect(false, message);
    }
}

[[nodiscard]] std::string track_record(
    const std::string_view id,
    const std::string_view set_id,
    const std::string_view title,
    const std::string_view artist,
    const std::string_view hash,
    const std::string_view extra = {})
{
    return "{\"type\":\"track\",\"stable_id\":\"" + std::string{id} +
           "\",\"beatmap_set_id\":\"" + std::string{set_id} +
           "\",\"title\":\"" + std::string{title} +
           "\",\"artist\":\"" + std::string{artist} +
           "\",\"audio_filename\":\"audio.mp3\",\"audio_hash\":\"" +
           std::string{hash} + "\",\"audio_path\":\"/osu/files/audio\"" +
           std::string{extra} + "}\n";
}

void test_loading_sorting_and_deduplication()
{
    std::stringstream input;
    input << "{\"type\":\"header\",\"schema_version\":1,\"lazer_version\":\"test\"}\n";
    input << track_record("set-b:b", "set-b", "Zulu", "beta", hash_b);
    input << track_record("set-a:a", "set-a", "Alpha", "Alpha", hash_a,
                          ",\"title_unicode\":\"アルファ\",\"artist_unicode\":\"あ\"");
    input << track_record("duplicate-difficulty", "set-b", "Different difficulty", "beta", hash_b);
    input << "{\"type\":\"warning\",\"message\":\"one file was missing\"}\n";

    const osu_player::LibraryLoadResult loaded = osu_player::load_library_ndjson(input);
    const auto& tracks = loaded.library.tracks();

    expect(loaded.schema_version == 1, "schema version is retained");
    expect(loaded.lazer_version == "test", "lazer version is retained");
    expect(loaded.warnings.size() == 1, "warning records are retained");
    expect(tracks.size() == 2, "duplicate set/audio pairs collapse to one track");
    expect(tracks[0].display_artist() == "beta", "tracks sort by display artist");
    expect(tracks[1].display_title() == "アルファ", "Unicode title is preferred for display");
    expect(tracks[1].display_artist() == "あ", "Unicode artist is preferred for display");
}

void test_search()
{
    std::stringstream input;
    input << "{\"type\":\"header\",\"schema_version\":1}\n";
    input << track_record("first", "set-a", "Triangles", "cYsmix", hash_a,
                          ",\"creator\":\"peppy\"");
    input << track_record("second", "set-b", "Other", "Someone", hash_b);

    const osu_player::LibraryLoadResult loaded = osu_player::load_library_ndjson(input);
    expect(loaded.library.search("TRI").size() == 1, "ASCII search is case-insensitive");
    expect(loaded.library.search("PePpY").size() == 1, "search includes creator");
    expect(loaded.library.search("missing").empty(), "search excludes non-matches");
    expect(loaded.library.search("").size() == 2, "an empty search returns all tracks");
}

void test_invalid_input()
{
    expect_format_error(
        [] {
            std::stringstream input{"{\"type\":\"track\"}\n"};
            static_cast<void>(osu_player::load_library_ndjson(input));
        },
        "a missing header is rejected");

    expect_format_error(
        [] {
            std::stringstream input{"{\"type\":\"header\",\"schema_version\":2}\n"};
            static_cast<void>(osu_player::load_library_ndjson(input));
        },
        "an unsupported schema version is rejected");

    expect_format_error(
        [] {
            std::stringstream input;
            input << "{\"type\":\"header\",\"schema_version\":1}\n";
            input << track_record("bad", "set", "Title", "Artist", "not-a-hash");
            static_cast<void>(osu_player::load_library_ndjson(input));
        },
        "an invalid audio hash is rejected");

    expect_format_error(
        [] {
            std::stringstream input{"{this is not JSON}\n"};
            static_cast<void>(osu_player::load_library_ndjson(input));
        },
        "malformed JSON is rejected");
}

}  // namespace

int main()
{
    test_loading_sorting_and_deduplication();
    test_search();
    test_invalid_input();

    if (failures != 0) {
        std::cerr << failures << " test(s) failed\n";
        return 1;
    }

    std::cout << "all library tests passed\n";
    return 0;
}
