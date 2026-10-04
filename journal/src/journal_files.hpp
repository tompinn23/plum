#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Finding, ordering and reading journal files.
namespace journal::files {

namespace fs = std::filesystem;

std::string utf8(const fs::path& path);

// A journal file with the ordering key parsed out of its name.
//
// Sorting the names does not work. The game has used two schemes, Journal.190102030405.01.log
// before 2022 and Journal.2024-01-02T030405.01.log after, and the part number is not fixed
// width, so a long session's .10 sorts before its .9 as text. For a fold-forward replay that is
// the wrong answer, not merely an odd order.
struct journal_file {
    fs::path path;
    std::array<int, 6> stamp{};  // y m d h m s, only ever compared
    int part = 0;

    // nullopt if the name is not a journal. Only the last path segment is examined.
    static std::optional<journal_file> of(const fs::path& path);

    friend bool operator<(const journal_file& a, const journal_file& b) {
        return a.stamp != b.stamp ? a.stamp < b.stamp : a.part < b.part;
    }
};

// Every journal in a directory, oldest first. Throws fs::filesystem_error.
std::vector<journal_file> list(const fs::path& directory);

// The newest journal in a directory, if any. Throws fs::filesystem_error.
std::optional<journal_file> newest(const fs::path& directory);

// Receives a complete line, without its terminator, and the byte offset where it starts.
using line_sink = std::function<void(std::string_view line, std::uint64_t offset)>;

// Reads complete lines from `start` to the end of the file and returns the offset of the first
// byte not consumed. A journal is appended to while it is read, so the last line is routinely
// partial; it is held back, and the next read starts at its first byte. The file is opened and
// closed per call so nothing here holds a handle on a file the game is writing.
// Throws std::system_error if the file cannot be opened.
std::uint64_t read_lines(const fs::path& file, std::uint64_t start, const line_sink& sink);

// Who wrote a journal, from the Commander or LoadGame line near its head.
struct owner {
    std::string fid;   // the stable account key
    std::string name;  // for display and logging
};

// nullopt if the file names no commander with an FID: an empty session, an unreadable file, or
// a journal old enough to predate FIDs. That means "unattributed", not "someone else's".
std::optional<owner> owner_of(const fs::path& journal);

// Whether a journal records a session that got as far as loading a commander. One that reached
// the main menu and quit holds nothing but a Fileheader and a Shutdown.
bool names_commander(const fs::path& journal);

}  // namespace journal::files
