#include "game_process.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <aclapi.h>
#include <sddl.h>
#include <tlhelp32.h>

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <memory>
#include <vector>

namespace journal::process {
    namespace {
        struct handle_closer {
            void operator()(HANDLE h) const {
                if (h && h != INVALID_HANDLE_VALUE) CloseHandle(h);
            }
        };

        using unique_handle = std::unique_ptr<void, handle_closer>;

        struct local_freer {
            void operator()(void *p) const { LocalFree(p); }
        };

        std::string narrow(const std::wstring &w) {
            if (w.empty()) return {};
            const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr,
                                              nullptr);
            std::string out(static_cast<std::size_t>(n), '\0');
            WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), out.data(), n, nullptr, nullptr);
            return out;
        }

        std::optional<user> sid_text(PSID sid) {
            LPWSTR text = nullptr;
            if (!sid || !ConvertSidToStringSidW(sid, &text)) return std::nullopt;
            const std::unique_ptr<void, local_freer> owned(text);
            return narrow(text);
        }

        // The account a process runs as. Fails for other users' processes without elevation, which is
        // fine: those are not the ones being looked for.
        std::optional<user> process_owner(HANDLE process) {
            HANDLE raw = nullptr;
            if (!OpenProcessToken(process, TOKEN_QUERY, &raw)) return std::nullopt;
            const unique_handle token(raw);

            DWORD size = 0;
            GetTokenInformation(token.get(), TokenUser, nullptr, 0, &size);
            if (size == 0) return std::nullopt;
            std::vector<std::byte> buffer(size);
            if (!GetTokenInformation(token.get(), TokenUser, buffer.data(), size, &size)) return std::nullopt;
            return sid_text(reinterpret_cast<TOKEN_USER *>(buffer.data())->User.Sid);
        }

        unique_handle open_process(std::uint32_t pid) {
            return unique_handle(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid));
        }
    } // namespace

    bool is_game_image(std::string_view file_name) {
        constexpr std::string_view client = "elitedangerous64.exe";
        return file_name.size() == client.size() &&
               std::ranges::equal(file_name, client, [](char a, char b) {
                   return std::tolower(static_cast<unsigned char>(a)) == b;
               });
    }

    std::optional<user> owner_of(const std::filesystem::path &path) {
        PSID owner = nullptr;
        PSECURITY_DESCRIPTOR descriptor = nullptr;
        if (GetNamedSecurityInfoW(path.c_str(), SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION, &owner, nullptr, nullptr,
                                  nullptr, &descriptor) != ERROR_SUCCESS)
            return std::nullopt;
        const std::unique_ptr<void, local_freer> owned(descriptor);
        return sid_text(owner);
    }

    std::vector<game> running_games() {
        std::vector<game> out;
        const unique_handle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
        if (snapshot.get() == INVALID_HANDLE_VALUE) return out;

        PROCESSENTRY32W entry{};
        entry.dwSize = sizeof(entry);
        for (BOOL more = Process32FirstW(snapshot.get(), &entry); more; more = Process32NextW(snapshot.get(), &entry)) {
            if (!is_game_image(narrow(entry.szExeFile))) continue;
            game g{.pid = entry.th32ProcessID};
            if (const auto process = open_process(g.pid)) g.owner = process_owner(process.get());
            out.push_back(std::move(g));
        }
        return out;
    }

    bool is_game(std::uint32_t pid) {
        const auto process = open_process(pid);
        if (!process) return false;

        DWORD code = 0;
        if (!GetExitCodeProcess(process.get(), &code) || code != STILL_ACTIVE) return false;

        wchar_t image[MAX_PATH];
        DWORD size = MAX_PATH;
        if (!QueryFullProcessImageNameW(process.get(), 0, image, &size)) return false;
        return is_game_image(narrow(std::filesystem::path(std::wstring(image, size)).filename().wstring()));
    }
} // namespace journal::process
