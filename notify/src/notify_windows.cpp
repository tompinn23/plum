// ReadDirectoryChangesW backend, driven by an I/O completion port.
//
// All watch state is owned by the worker thread. watch()/unwatch() from other threads
// are marshalled onto it as completion-port packets, so no locking is needed and every
// ReadDirectoryChangesW / CancelIoEx happens on one thread.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "detail.hpp"

#include <algorithm>
#include <future>
#include <map>
#include <memory>
#include <optional>
#include <ranges>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace notify {
namespace fs = std::filesystem;
using detail::make_event;

namespace {

// Completion keys. Watch keys are WatchState pointers, which are never 1 or 2.
constexpr ULONG_PTR kCommandKey = 1;
constexpr ULONG_PTR kQuitKey = 2;

constexpr DWORD kNotifyFilter =
    FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME | FILE_NOTIFY_CHANGE_ATTRIBUTES |
    FILE_NOTIFY_CHANGE_SIZE | FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_CREATION |
    FILE_NOTIFY_CHANGE_SECURITY;

// 64 KiB is the maximum ReadDirectoryChangesW accepts for network shares.
constexpr DWORD kBufferBytes = 64 * 1024;

std::error_code win_error(const DWORD code) { return {static_cast<int>(code), std::system_category()}; }

bool same_name(const std::wstring& a, const std::wstring& b) {
    return CompareStringOrdinal(a.c_str(), static_cast<int>(a.size()), b.c_str(),
                                static_cast<int>(b.size()), TRUE) == CSTR_EQUAL;
}

struct WatchState {
    fs::path key;              // normalised path the user passed to watch()
    fs::path dir;              // directory handed to ReadDirectoryChangesW
    std::wstring file_filter;  // set when watching a single file: only report this name
    HANDLE handle = INVALID_HANDLE_VALUE;
    OVERLAPPED overlapped{};
    std::unique_ptr<DWORD[]> buffer{new DWORD[kBufferBytes / sizeof(DWORD)]};  // DWORD-aligned
    bool pending = false;  // a read is in flight; buffer/overlapped must stay alive
    bool closing = false;  // cancelled; free when its completion arrives

    WatchState() = default;
    WatchState(const WatchState&) = delete;
    WatchState& operator=(const WatchState&) = delete;
    ~WatchState() {
        if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle);
    }
};

struct Command {
    enum class Op { Add, Remove } op;
    fs::path path;
    std::promise<std::error_code> done;
};

}  // namespace

struct watcher::Impl {
    explicit Impl(Handler handler) : handler_(std::move(handler)) {
        port_ = CreateIoCompletionPort(INVALID_HANDLE_VALUE, nullptr, 0, 1);
        if (!port_) throw std::system_error(win_error(GetLastError()), "CreateIoCompletionPort");
        try {
            worker_ = std::thread([this] { run(); });
        } catch (...) {
            CloseHandle(port_);
            throw;
        }
    }

    ~Impl() {
        PostQueuedCompletionStatus(port_, 0, kQuitKey, nullptr);
        worker_.join();
        CloseHandle(port_);
    }

    std::error_code add(const fs::path& path) { return submit(Command::Op::Add, path); }
    std::error_code remove(const fs::path& path) { return submit(Command::Op::Remove, path); }

private:
    std::error_code submit(const Command::Op op, const fs::path& path) {
        // Defensive: if ever called on the worker thread, posting and waiting would deadlock.
        if (std::this_thread::get_id() == worker_.get_id()) return execute(op, path);

        Command cmd{.op = op, .path = path, .done = {}};
        auto result = cmd.done.get_future();
        if (!PostQueuedCompletionStatus(port_, 0, kCommandKey, reinterpret_cast<LPOVERLAPPED>(&cmd)))
            return win_error(GetLastError());
        return result.get();
    }

    std::error_code execute(const Command::Op op, const fs::path& path) {
        return op == Command::Op::Add ? do_add(path) : do_remove(path);
    }

    void run() {
        for (;;) {
            DWORD bytes = 0;
            ULONG_PTR key = 0;
            OVERLAPPED* overlapped = nullptr;
            const BOOL ok = GetQueuedCompletionStatus(port_, &bytes, &key, &overlapped, INFINITE);
            const DWORD err = ok ? ERROR_SUCCESS : GetLastError();

            if (!ok && overlapped == nullptr) {  // the port itself failed
                handler_(result(error{.code = win_error(err), .paths = {}}));
                return;
            }

            if (key == kCommandKey) {
                auto* cmd = reinterpret_cast<Command*>(overlapped);
                cmd->done.set_value(execute(cmd->op, cmd->path));  // cmd may be gone after this
                continue;
            }

            if (key == kQuitKey) {
                stopping_ = true;
                for (auto &val: watches_ | std::views::values) retire(std::move(val));
                watches_.clear();
            } else {
                on_completion(reinterpret_cast<WatchState*>(key), ok, err, bytes);
            }

            // Don't leave while a cancelled read could still write into freed memory.
            if (stopping_ && closing_.empty()) return;
        }
    }

    std::error_code do_add(const fs::path& path) {
        if (watches_.contains(path)) return {};

        std::error_code ec;
        const auto status = fs::status(path, ec);
        if (ec) return ec;

        auto w = std::make_unique<WatchState>();
        w->key = path;
        if (fs::is_directory(status)) {
            w->dir = path;
        } else {
            // ReadDirectoryChangesW only takes directories: watch the parent and filter.
            w->dir = path.parent_path();
            w->file_filter = path.filename().wstring();
        }

        w->handle = CreateFileW(w->dir.c_str(), FILE_LIST_DIRECTORY,
                                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED,
                                nullptr);
        if (w->handle == INVALID_HANDLE_VALUE) return win_error(GetLastError());

        if (!CreateIoCompletionPort(w->handle, port_, reinterpret_cast<ULONG_PTR>(w.get()), 0))
            return win_error(GetLastError());

        if (const auto err = issue_read(*w)) return err;
        watches_.emplace(path, std::move(w));
        return {};
    }

    std::error_code do_remove(const fs::path& path) {
        const auto it = watches_.find(path);
        if (it == watches_.end()) return std::make_error_code(std::errc::no_such_file_or_directory);
        auto w = std::move(it->second);
        watches_.erase(it);
        retire(std::move(w));
        return {};
    }

    // Free a watch, deferring until its outstanding read has completed or been cancelled.
    void retire(std::unique_ptr<WatchState> w) {
        if (!w->pending) return;  // nothing in flight: destroy now
        w->closing = true;
        CancelIoEx(w->handle, &w->overlapped);
        closing_.push_back(std::move(w));
    }

    static std::error_code issue_read(WatchState& w) {
        w.overlapped = OVERLAPPED{};
        if (!ReadDirectoryChangesW(w.handle, w.buffer.get(), kBufferBytes, /*bWatchSubtree=*/FALSE,
                                   kNotifyFilter, nullptr, &w.overlapped, nullptr)) {
            w.pending = false;
            return win_error(GetLastError());
        }
        w.pending = true;
        return {};
    }

    void on_completion(WatchState* w, const BOOL ok, const DWORD err, const DWORD bytes) {
        w->pending = false;

        if (w->closing) {
            closing_.erase(std::ranges::remove_if(closing_,
                                                  [w](const auto& p) { return p.get() == w; }).begin(),
                           closing_.end());
            return;
        }

        // Collect first, dispatch last, so delivering events never overlaps with
        // mutating watch state.
        std::vector<result> out;
        std::error_code failure;

        if (!ok) {
            failure = win_error(err);
        } else {
            if (bytes == 0) {
                // Buffer overflowed and the kernel discarded the changes.
                out.emplace_back(make_event(event_kind::Other, {w->key}, modify_kind::Any, true));
            } else {
                parse(*w, out);
            }
            failure = issue_read(*w);
        }

        if (failure) {
            if (std::error_code ec; !fs::exists(w->dir, ec))
                out.emplace_back(make_event(event_kind::Remove, {w->key}));  // watched dir is gone
            else
                out.emplace_back(error{.code = failure, .paths = {w->key}});
            watches_.erase(w->key);  // destroys *w (no read pending)
        }

        for (auto& r : out) handler_(std::move(r));
    }

    static void parse(const WatchState& w, std::vector<result>& out) {
        const auto* base = reinterpret_cast<const BYTE*>(w.buffer.get());

        // Windows reports a rename as OLD_NAME immediately followed by NEW_NAME within
        // the same buffer, so pairing never needs to span reads.
        std::optional<std::pair<fs::path, bool>> rename_from;  // (path, matched filter)
        auto flush_rename = [&] {
            if (rename_from && rename_from->second)
                out.emplace_back(make_event(event_kind::RenameFrom, {std::move(rename_from->first)}));
            rename_from.reset();
        };

        for (DWORD offset = 0;;) {
            const auto* info = reinterpret_cast<const FILE_NOTIFY_INFORMATION*>(base + offset);
            std::wstring name(info->FileName, info->FileNameLength / sizeof(WCHAR));
            fs::path path = w.dir / name;
            bool match = w.file_filter.empty() || same_name(name, w.file_filter);

            if (info->Action != FILE_ACTION_RENAMED_NEW_NAME) flush_rename();

            switch (info->Action) {
                case FILE_ACTION_ADDED:
                    if (match) out.emplace_back(make_event(event_kind::Create, {path}));
                    break;
                case FILE_ACTION_REMOVED:
                    if (match) out.emplace_back(make_event(event_kind::Remove, {path}));
                    break;
                case FILE_ACTION_MODIFIED:
                    if (match) out.emplace_back(make_event(event_kind::Modify, {path}));
                    break;
                case FILE_ACTION_RENAMED_OLD_NAME:
                    rename_from.emplace(path, match);
                    break;
                case FILE_ACTION_RENAMED_NEW_NAME:
                    if (rename_from) {
                        // For a file watch, report the rename if either side is the file.
                        if (rename_from->second || match)
                            out.emplace_back(
                                make_event(event_kind::Rename, {std::move(rename_from->first), path}));
                        rename_from.reset();
                    } else if (match) {
                        out.emplace_back(make_event(event_kind::RenameTo, {path}));
                    }
                    break;
                default:
                    break;
            }

            if (info->NextEntryOffset == 0) break;
            offset += info->NextEntryOffset;
        }
        flush_rename();
    }

    Handler handler_;
    HANDLE port_ = nullptr;
    bool stopping_ = false;                                         // worker thread only
    std::map<fs::path, std::unique_ptr<WatchState>> watches_;       // worker thread only
    std::vector<std::unique_ptr<WatchState>> closing_;              // worker thread only
    std::thread worker_;
};

}  // namespace notify

#include "watcher_common.inl"
