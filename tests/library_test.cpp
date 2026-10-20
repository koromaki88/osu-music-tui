#include "osu_player/library_loader.hpp"

#include <iostream>
#include <algorithm>
#include <random>
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

void test_construction_matches_reference()
{
    // Keep an independent version of the original folded-string comparison as
    // an oracle, including UTF-8 byte order, prefixes, case ties and stable IDs.
    const auto fold = [](std::string text) {
        for (auto& c : text) { if (c >= 'A' && c <= 'Z') { c = static_cast<char>(c + ('a' - 'A')); } }
        return text;
    };
    const std::vector<std::string> names{"", "A", "a", "AA", "Ab", "aB", "B", "日本", "é", "あ",
        std::string(200, 'A'), std::string(200, 'a') + "b", std::string{"a\0B", 3}};
    std::vector<osu_player::Track> input;
    for (std::size_t i = 0; i < 10001; ++i) {
        osu_player::Track track;
        track.stable_id = std::to_string(i);
        track.beatmap_set_id = track.stable_id;
        track.audio_hash = std::string{hash_a};
        track.artist = names[i % names.size()];
        track.title = names[(i / names.size()) % names.size()];
        if (i % 3 == 0) { track.artist_unicode = names[(i + 7) % names.size()]; }
        if (i % 5 == 0) { track.title_unicode = names[(i + 3) % names.size()]; }
        input.push_back(std::move(track));
    }
    std::mt19937 random{42};
    std::shuffle(input.begin(), input.end(), random);
    auto reference = input;
    std::ranges::sort(reference, [&](const auto& a, const auto& b) {
        const auto artist_a = fold(a.display_artist()), artist_b = fold(b.display_artist());
        if (artist_a != artist_b) { return artist_a < artist_b; }
        const auto title_a = fold(a.display_title()), title_b = fold(b.display_title());
        return title_a == title_b ? a.stable_id < b.stable_id : title_a < title_b;
    });
    // Repeated difficulties must retain the first metadata, not whichever one sorts first.
    for (std::size_t i = 0; i < 1000; ++i) {
        auto duplicate = input[i];
        duplicate.stable_id = "duplicate";
        duplicate.title = "incorrect replacement";
        input.push_back(std::move(duplicate));
    }
    const osu_player::Library library{std::move(input)};
    expect(library.tracks().size() == reference.size(), "deduplication keeps unique count");
    for (std::size_t i = 0; i < reference.size() && i < library.tracks().size(); ++i) {
        expect(library.tracks()[i].stable_id == reference[i].stable_id, "allocation-free sort matches original ordering");
    }
    const osu_player::Library empty{std::vector<osu_player::Track>{}};
    expect(empty.tracks().empty(), "empty vector construction remains safe");
    std::vector<osu_player::Track> duplicates(10000, reference.front());
    const osu_player::Library single{std::move(duplicates)};
    expect(single.tracks().size() == 1 && single.tracks()[0].stable_id == reference.front().stable_id,
           "all-duplicate input compacts safely without self-moves");
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
    test_construction_matches_reference();
    test_invalid_input();

    if (failures != 0) {
        std::cerr << failures << " test(s) failed\n";
        return 1;
    }

    std::cout << "all library tests passed\n";
    return 0;
}
