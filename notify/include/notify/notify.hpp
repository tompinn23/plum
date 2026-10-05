// notify-cpp: a small C++17 file-system watcher modelled on Rust's `notify` crate.
// Backends: inotify (Linux) and ReadDirectoryChangesW (Windows). Non-recursive only.
#pragma once

#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>

namespace notify {

enum class event_kind {
    Create,      // paths = {created}
    Remove,      // paths = {removed}
    Modify,      // paths = {modified}; see Event::modify
    Rename,      // paths = {from, to}, both inside the watched directory
    RenameFrom,  // paths = {from}; moved out of the watch (or partner never arrived)
    RenameTo,    // paths = {to};   moved in from outside the watch
    Other,       // e.g. kernel queue overflow; check Event::need_rescan
};

enum class modify_kind {
    Any,       // backend can't tell (always the case on Windows)
    Data,      // file contents changed            (Linux: IN_MODIFY)
    Metadata,  // permissions, timestamps, owner…  (Linux: IN_ATTRIB)
};

struct event {
    event_kind kind = event_kind::Other;
    std::vector<std::filesystem::path> paths;
    modify_kind modify = modify_kind::Any;
    // True when events were dropped (buffer/queue overflow). Rescan the watched paths.
    bool need_rescan = false;
};

struct error {
    std::error_code code;
    std::vector<std::filesystem::path> paths;
};

// Equivalent of notify's `Result<Event>`.
class result {
public:
    explicit result(notify::event e) : v_(std::move(e)) {}
    explicit result(notify::error e) : v_(std::move(e)) {}

    [[nodiscard]] bool ok() const noexcept { return std::holds_alternative<notify::event>(v_); }
    explicit operator bool() const noexcept { return ok(); }

    [[nodiscard]] const notify::event& event() const { return std::get<notify::event>(v_); }
    [[nodiscard]] const notify::error& error() const { return std::get<notify::error>(v_); }

private:
    std::variant<notify::event, notify::error> v_;
};

// Watches files and directories (non-recursively). Events are queued by an internal
// background thread and retrieved with receive().
class watcher {
public:
    watcher();  // throws std::system_error
    ~watcher();

    watcher(watcher&&) noexcept;
    watcher& operator=(watcher&&) noexcept;
    watcher(const watcher&) = delete;
    watcher& operator=(const watcher&) = delete;

    // Start watching a directory (its direct children) or a single file.
    void watch(const std::filesystem::path& path);  // throws std::filesystem::filesystem_error

    // Stop watching a path previously passed to watch().
    void unwatch(const std::filesystem::path& path);  // throws std::filesystem::filesystem_error

    // Block until the next event or error is available.
    [[nodiscard]] result receive() const;

    // Wait at most `timeout`; std::nullopt if nothing arrived. A zero timeout polls.
    [[nodiscard]] std::optional<result> receive_for(std::chrono::milliseconds timeout) const;

private:
    using Handler = std::function<void(result)>;  // how backends deliver events
    struct Queue;
    struct Impl;
    std::unique_ptr<Queue> queue_;  // declared first: must outlive impl_'s thread
    std::unique_ptr<Impl> impl_;
};

inline const char* to_string(event_kind kind) noexcept {
    switch (kind) {
        case event_kind::Create:     return "create";
        case event_kind::Remove:     return "remove";
        case event_kind::Modify:     return "modify";
        case event_kind::Rename:     return "rename";
        case event_kind::RenameFrom: return "rename-from";
        case event_kind::RenameTo:   return "rename-to";
        case event_kind::Other:      return "other";
    }
    return "unknown";
}

}  // namespace notify
