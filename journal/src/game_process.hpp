#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Finding a running Elite Dangerous client, and whose it is. Platform code lives in
// game_process_windows.cpp and game_process_linux.cpp.
namespace journal::process {
    // A user account, comparable only for equality: a SID string on Windows, a uid on Linux.
    using user = std::string;

    // Who owns a file or directory; nullopt if that cannot be read.
    std::optional<user> owner_of(const std::filesystem::path &path);

    struct game {
        std::uint32_t pid = 0;
        std::optional<user> owner; // nullopt where the process could not be inspected
    };

    // Every running game client. A full process scan, so not something to call often.
    std::vector<game> running_games();

    // Whether `pid` is still a running game client. Cheap, and checks the image rather than trusting
    // the number, since a pid is reused once its process exits.
    bool is_game(std::uint32_t pid);

    // The client is EliteDangerous64.exe, natively or under Wine/Proton. The launcher is not counted:
    // it runs without the game.
    bool is_game_image(std::string_view file_name);
} // namespace journal::process
