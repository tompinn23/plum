#include "game_process.hpp"

#include <sys/stat.h>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <fstream>
#include <iterator>
#include <string>

// The game runs under Wine or Proton, so it appears as an ordinary Linux process whose command
// line names the Windows executable.
namespace journal::process {
    namespace fs = std::filesystem;

    namespace {
        std::optional<std::uint32_t> pid_of(const fs::path &entry) {
            const std::string name = entry.filename().string();
            std::uint32_t pid = 0;
            const auto [end, ec] = std::from_chars(name.data(), name.data() + name.size(), pid);
            return ec == std::errc() && end == name.data() + name.size() ? std::optional(pid) : std::nullopt;
        }

        // argv[0] of a process, the Windows path for a Wine process. /proc/<pid>/comm would be shorter to
        // read but is cut to 15 characters.
        std::string image_of(std::uint32_t pid) {
            std::ifstream in("/proc/" + std::to_string(pid) + "/cmdline", std::ios::binary);
            std::string argv0;
            std::getline(in, argv0, '\0');
            return argv0;
        }

        bool runs_game(std::uint32_t pid) {
            const std::string image = image_of(pid);
            const auto slash = image.find_last_of("/\\");
            return is_game_image(slash == std::string::npos ? image : image.substr(slash + 1));
        }
    } // namespace

    bool is_game_image(std::string_view file_name) {
        constexpr std::string_view client = "elitedangerous64.exe";
        return file_name.size() == client.size() &&
               std::ranges::equal(file_name, client, [](char a, char b) {
                   return std::tolower(static_cast<unsigned char>(a)) == b;
               });
    }

    std::optional<user> owner_of(const fs::path &path) {
        struct stat st{};
        if (::stat(path.c_str(), &st) != 0) return std::nullopt;
        return std::to_string(st.st_uid);
    }

    std::vector<game> running_games() {
        std::vector<game> out;
        std::error_code ec;
        for (const auto &entry: fs::directory_iterator("/proc", ec)) {
            const auto pid = pid_of(entry.path());
            if (!pid || !runs_game(*pid)) continue;
            out.push_back({.pid = *pid, .owner = owner_of(entry.path())});
        }
        return out;
    }

    bool is_game(std::uint32_t pid) {
        std::error_code ec;
        return fs::exists("/proc/" + std::to_string(pid), ec) && runs_game(pid);
    }
} // namespace journal::process
