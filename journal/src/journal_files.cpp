#include "journal_files.hpp"

#include <algorithm>
#include <cerrno>
#include <fstream>
#include <regex>
#include <system_error>

#include "journal/game_event.hpp"
#include <spdlog/spdlog.h>

namespace journal::files {

std::string utf8(const fs::path& path) {
    const auto s = path.u8string();
    return {s.begin(), s.end()};
}

std::optional<journal_file> journal_file::of(const fs::path& path) {
    // Journal.2024-01-02T030405.01.log
    static const std::regex modern(R"(^Journal\.(\d{4})-(\d{2})-(\d{2})T(\d{2})(\d{2})(\d{2})\.(\d+)\.log$)");
    // Journal.190102030405.01.log, two-digit year, pre-2022
    static const std::regex legacy(R"(^Journal\.(\d{2})(\d{2})(\d{2})(\d{2})(\d{2})(\d{2})\.(\d+)\.log$)");

    const std::string name = utf8(path.filename());
    std::smatch m;
    int year_offset = 0;
    if (std::regex_match(name, m, modern)) {
        year_offset = 0;
    } else if (std::regex_match(name, m, legacy)) {
        year_offset = 2000;
    } else {
        return std::nullopt;
    }

    // The two schemes disagree about time zone (legacy names are local, modern ones UTC), so the
    // stamp is only used for ordering; the schemes do not overlap in time.
    journal_file file;
    file.path = path;
    for (int i = 0; i < 6; ++i) file.stamp[static_cast<std::size_t>(i)] = std::stoi(m[i + 1].str());
    file.stamp[0] += year_offset;
    file.part = std::stoi(m[7].str());
    return file;
}

std::vector<journal_file> list(const fs::path& directory) {
    std::vector<journal_file> out;
    for (const auto& entry : fs::directory_iterator(directory)) {
        if (auto file = journal_file::of(entry.path())) out.push_back(std::move(*file));
    }
    std::sort(out.begin(), out.end());
    return out;
}

std::optional<journal_file> newest(const fs::path& directory) {
    std::optional<journal_file> best;
    for (const auto& entry : fs::directory_iterator(directory)) {
        auto file = journal_file::of(entry.path());
        if (file && (!best || *best < *file)) best = std::move(file);
    }
    return best;
}

std::uint64_t read_lines(const fs::path& file, std::uint64_t start, const line_sink& sink) {
    std::ifstream in(file, std::ios::binary);
    if (!in) throw std::system_error(errno, std::generic_category(), "cannot open " + utf8(file));
    in.seekg(static_cast<std::streamoff>(start));
    if (!in) return start;  // shorter than we thought; nothing to read

    constexpr std::size_t chunk = 64 * 1024;
    std::vector<char> buf(chunk);
    std::string line;
    std::uint64_t pos = start;       // absolute offset of buf[0]
    std::uint64_t complete = start;  // end of the last finished line = start of the current one
    bool seen_cr = false;

    auto emit = [&] {
        if (!line.empty()) sink(line, complete);
        line.clear();
    };

    for (;;) {
        in.read(buf.data(), static_cast<std::streamsize>(chunk));
        const auto got = static_cast<std::size_t>(in.gcount());
        if (got == 0) break;

        for (std::size_t i = 0; i < got; ++i) {
            const char ch = buf[i];
            const std::uint64_t at = pos + i;
            if (ch == '\n') {  // LF, or the LF of a CRLF
                emit();
                complete = at + 1;
                seen_cr = false;
            } else if (ch == '\r') {
                if (seen_cr) {  // the previous CR stood alone and ended its line
                    emit();
                    complete = at;
                }
                seen_cr = true;
            } else {
                if (seen_cr) {  // a lone CR ended the previous line; this byte starts the next
                    emit();
                    complete = at;
                    seen_cr = false;
                }
                line.push_back(ch);
            }
        }
        pos += got;
    }
    // A trailing CR with nothing after it may be the first half of a CRLF still being written:
    // leave that line for the next read.
    return complete;
}

namespace {

// The first Commander or LoadGame line near the head of a journal. Fileheader is first and
// Commander normally second, but a session that fails to log in can put a few lines in between;
// past this the file has no commander to find.
std::optional<game_event> commander_line(const fs::path& journal) {
    constexpr int head_lines = 20;

    std::ifstream in(journal, std::ios::binary);
    if (!in) {
        spdlog::warn("cannot read the head of {}", utf8(journal));
        return std::nullopt;
    }
    std::string line;
    for (int i = 0; i < head_lines && std::getline(in, line); ++i) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        auto event = game_event::parse(line);
        if (event && (event->name() == "Commander" || event->name() == "LoadGame")) return event;
    }
    return std::nullopt;
}

}  // namespace

std::optional<owner> owner_of(const fs::path& journal) {
    const auto event = commander_line(journal);
    if (!event) return std::nullopt;

    // Journals from before 2017 name a commander but carry no FID, leaving nothing stable to key
    // on. Unattributed is the honest answer, and the safe one.
    auto fid = event->opt_str("FID");
    if (!fid) return std::nullopt;
    return owner{std::move(*fid), event->str(event->name() == "Commander" ? "Name" : "Commander")};
}

bool names_commander(const fs::path& journal) {
    return commander_line(journal).has_value();
}

}  // namespace journal::files
