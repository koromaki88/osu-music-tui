#include "osu_player/storage_path.hpp"

#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

int failures = 0;

void expect(const bool condition, const std::string_view message)
{
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

template <typename Function>
void expect_invalid_argument(Function function, const std::string_view message)
{
    try {
        function();
        expect(false, message);
    } catch (const std::invalid_argument&) {
        // Expected.
    } catch (...) {
        expect(false, message);
    }
}

void test_hash_validation()
{
    expect(osu_player::is_sha256(std::string(64, '0')), "64 hexadecimal characters are valid");
    expect(osu_player::is_sha256(std::string(64, 'A')), "uppercase hexadecimal characters are valid");
    expect(!osu_player::is_sha256(std::string(63, '0')), "a short hash is invalid");
    expect(!osu_player::is_sha256(std::string(65, '0')), "a long hash is invalid");
    expect(!osu_player::is_sha256(std::string(64, 'g')), "a non-hexadecimal hash is invalid");
}

void test_normalization()
{
    const std::string uppercase(64, 'A');
    expect(osu_player::normalize_sha256(uppercase) == std::string(64, 'a'),
           "hash normalization converts A-F to lowercase");

    expect_invalid_argument(
        [] { static_cast<void>(osu_player::normalize_sha256("not-a-hash")); },
        "normalizing an invalid hash throws invalid_argument");
}

void test_content_path()
{
    constexpr std::string_view hash =
        "47b895484e7751f3ab429694ff6dbf21e774ab023e4f6c5b481476f04ff22f0f";
    const std::filesystem::path expected =
        "/data/osu/files/4/47/47b895484e7751f3ab429694ff6dbf21e774ab023e4f6c5b481476f04ff22f0f";

    expect(osu_player::content_path("/data/osu", hash) == expected,
           "content path uses the first one and first two hash characters");
}

}  // namespace

int main()
{
    test_hash_validation();
    test_normalization();
    test_content_path();

    if (failures != 0) {
        std::cerr << failures << " test(s) failed\n";
        return 1;
    }

    std::cout << "all storage path tests passed\n";
    return 0;
}
