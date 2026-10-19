#pragma once

#include "osu_player/lastfm_client.hpp"

#include <fcntl.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <string_view>
#include <utility>

namespace osu_player::lastfm_storage {

struct File {
    int fd{-1};
    explicit File(int value = -1) : fd{value} { }
    ~File() { if (fd >= 0) { close(fd); } }
    File(const File&) = delete;
    File& operator=(const File&) = delete;
    File(File&& other) noexcept : fd{std::exchange(other.fd, -1)} { }
};

// File operations stay relative to an owned directory descriptor. O_NOFOLLOW on
// just the final pathname is insufficient if its parent is replaced meanwhile.
class Directory {
public:
    Directory(std::filesystem::path path, bool queue)
        : path_{std::move(path)}, queue_{queue}, file_{open(path_.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC)}
    {
        if (file_.fd < 0) { fail(); }
        struct stat info{};
        if (fstat(file_.fd, &info) != 0) { fail(); }
        if (!S_ISDIR(info.st_mode) || info.st_uid != getuid() || (info.st_mode & 077) != 0) { unsafe(); }
    }

    File open_file(const std::string& name, int flags, bool missing = false) const
    {
        File result{openat(file_.fd, name.c_str(), flags | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC, 0600)};
        if (result.fd < 0) { if (missing && errno == ENOENT) { return result; } fail(); }
        struct stat info{};
        if (fstat(result.fd, &info) != 0) { fail(); }
        if (!S_ISREG(info.st_mode) || info.st_uid != getuid() || (info.st_mode & 077) != 0 || info.st_nlink != 1) { unsafe(); }
        return result;
    }

    void atomic_write(const std::string& name, std::string_view text) const
    {
        check_current(); check_target(name);
        std::string temporary;
        File staging{make_temporary(temporary)};
        try {
            std::size_t offset{};
            while (offset < text.size()) {
                const auto bytes = write(staging.fd, text.data() + offset, text.size() - offset);
                if (bytes < 0 && errno == EINTR) { continue; }
                if (bytes <= 0) { fail(); }
                offset += static_cast<std::size_t>(bytes);
            }
            if (fsync(staging.fd) != 0) { fail(); }
            check_current(); check_target(name);
            if (renameat(file_.fd, temporary.c_str(), file_.fd, name.c_str()) != 0 || fsync(file_.fd) != 0) { fail(); }
        } catch (...) { unlinkat(file_.fd, temporary.c_str(), 0); throw; }
    }

private:
    [[noreturn]] void fail() const
    {
        throw LastfmFailure{queue_ ? LastfmFailureKind::queue : LastfmFailureKind::credential_io, queue_ ? 2 : errno};
    }
    [[noreturn]] void unsafe() const
    {
        throw LastfmFailure{queue_ ? LastfmFailureKind::queue : LastfmFailureKind::credential_permissions, queue_ ? 2 : 0};
    }
    void check_current() const
    {
        struct stat owned{}, named{};
        if (fstat(file_.fd, &owned) != 0 || lstat(path_.c_str(), &named) != 0) { fail(); }
        if (!S_ISDIR(named.st_mode) || owned.st_dev != named.st_dev || owned.st_ino != named.st_ino ||
            owned.st_uid != getuid() || (owned.st_mode & 077) != 0) { unsafe(); }
    }
    void check_target(const std::string& name) const
    {
        struct stat info{};
        if (fstatat(file_.fd, name.c_str(), &info, AT_SYMLINK_NOFOLLOW) != 0) {
            if (errno == ENOENT) { return; }
            fail();
        }
        if (!S_ISREG(info.st_mode) || info.st_uid != getuid() || (info.st_mode & 077) != 0 || info.st_nlink != 1) { unsafe(); }
    }
    int make_temporary(std::string& name) const
    {
        for (int attempt = 0; attempt < 32; ++attempt) {
            std::array<unsigned char, 16> random{};
            std::size_t offset{};
            while (offset < random.size()) {
                const auto bytes = getrandom(random.data() + offset, random.size() - offset, 0);
                if (bytes < 0 && errno == EINTR) { continue; }
                if (bytes <= 0) { fail(); }
                offset += static_cast<std::size_t>(bytes);
            }
            name = ".lastfm-";
            constexpr char hex[] = "0123456789abcdef";
            for (const auto byte : random) { name += hex[byte >> 4]; name += hex[byte & 15]; }
            const int fd = openat(file_.fd, name.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
            if (fd >= 0) { return fd; }
            if (errno != EEXIST) { fail(); }
        }
        errno = EEXIST; fail();
    }
    std::filesystem::path path_;
    bool queue_;
    File file_;
};
}  // namespace osu_player::lastfm_storage
