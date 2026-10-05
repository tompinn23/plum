// inotify backend.
#include "detail.hpp"

#include <poll.h>
#include <sys/eventfd.h>
#include <sys/inotify.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>

namespace notify {
namespace fs = std::filesystem;
using detail::make_event;

namespace {

constexpr std::uint32_t kWatchMask = IN_CREATE | IN_DELETE | IN_MODIFY | IN_ATTRIB |
                                     IN_MOVED_FROM | IN_MOVED_TO | IN_DELETE_SELF |
                                     IN_MOVE_SELF | IN_EXCL_UNLINK;

// How long to wait for the IN_MOVED_TO half of a rename before reporting RenameFrom.
constexpr int kRenamePairTimeoutMs = 10;

std::error_code last_error() { return {errno, std::system_category()}; }

}  // namespace

struct watcher::Impl {
    explicit Impl(Handler handler) : handler_(std::move(handler)) {
        inotify_fd_ = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
        if (inotify_fd_ < 0) throw std::system_error(last_error(), "inotify_init1");

        wake_fd_ = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
        if (wake_fd_ < 0) {
            auto ec = last_error();
            ::close(inotify_fd_);
            throw std::system_error(ec, "eventfd");
        }

        try {
            worker_ = std::thread([this] { run(); });
        } catch (...) {
            ::close(wake_fd_);
            ::close(inotify_fd_);
            throw;
        }
    }

    ~Impl() {
        std::uint64_t one = 1;
        (void)!::write(wake_fd_, &one, sizeof one);
        worker_.join();
        ::close(wake_fd_);
        ::close(inotify_fd_);
    }

    std::error_code add(const fs::path& path) {
        // Hold the lock across inotify_add_watch so the worker can't see events for
        // the new descriptor before we've recorded which path it belongs to.
        std::lock_guard<std::mutex> lock(mutex_);
        int wd = inotify_add_watch(inotify_fd_, path.c_str(), kWatchMask);
        if (wd < 0) return last_error();
        by_wd_[wd] = path;
        by_path_[path.native()] = wd;
        return {};
    }

    std::error_code remove(const fs::path& path) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = by_path_.find(path.native());
        if (it == by_path_.end()) return std::make_error_code(std::errc::no_such_file_or_directory);
        int wd = it->second;
        by_path_.erase(it);
        by_wd_.erase(wd);  // later events for wd (incl. IN_IGNORED) are now dropped
        if (inotify_rm_watch(inotify_fd_, wd) < 0) return last_error();
        return {};
    }

private:
    void run() {
        alignas(inotify_event) char buffer[64 * 1024];
        pollfd fds[2] = {{inotify_fd_, POLLIN, 0}, {wake_fd_, POLLIN, 0}};

        for (;;) {
            int n = ::poll(fds, 2, pending_from_ ? kRenamePairTimeoutMs : -1);
            if (n < 0) {
                if (errno == EINTR) continue;
                handler_(result{error{last_error(), {}}});
                return;
            }
            if (n == 0) {  // rename partner didn't show up
                flush_rename();
                continue;
            }
            if (fds[1].revents & POLLIN) {  // shutdown requested
                flush_rename();
                return;
            }
            if (!(fds[0].revents & POLLIN)) continue;

            for (;;) {
                ssize_t len = ::read(inotify_fd_, buffer, sizeof buffer);
                if (len < 0) {
                    if (errno == EINTR) continue;
                    if (errno == EAGAIN) break;
                    handler_(result{error{last_error(), {}}});
                    return;
                }
                for (char* p = buffer; p < buffer + len;) {
                    const auto* ev = reinterpret_cast<const inotify_event*>(p);
                    p += sizeof(inotify_event) + ev->len;
                    handle(*ev);
                }
            }
        }
    }

    void handle(const inotify_event& ev) {
        if (ev.mask & IN_Q_OVERFLOW) {
            flush_rename();
            handler_(result(make_event(event_kind::Other, {}, modify_kind::Any, /*need_rescan=*/true)));
            return;
        }

        fs::path base;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = by_wd_.find(ev.wd);
            if (it == by_wd_.end()) return;  // unwatched meanwhile
            if (ev.mask & IN_IGNORED) {      // kernel dropped the watch (target deleted, unmounted…)
                auto p = by_path_.find(it->second.native());
                if (p != by_path_.end() && p->second == ev.wd) by_path_.erase(p);
                by_wd_.erase(it);
                return;
            }
            base = it->second;
        }

        // For directory watches the event names a child; for file watches len == 0.
        fs::path path = ev.len ? base / ev.name : base;

        if (pending_from_ && !((ev.mask & IN_MOVED_TO) && ev.cookie == pending_from_->first))
            flush_rename();

        if (ev.mask & IN_MOVED_FROM) {
            pending_from_.emplace(ev.cookie, std::move(path));
        } else if (ev.mask & IN_MOVED_TO) {
            if (pending_from_) {
                fs::path from = std::move(pending_from_->second);
                pending_from_.reset();
                handler_(result(make_event(event_kind::Rename, {std::move(from), std::move(path)})));
            } else {
                handler_(result(make_event(event_kind::RenameTo, {std::move(path)})));
            }
        } else if (ev.mask & IN_CREATE) {
            handler_(result(make_event(event_kind::Create, {std::move(path)})));
        } else if (ev.mask & (IN_DELETE | IN_DELETE_SELF)) {
            handler_(result(make_event(event_kind::Remove, {std::move(path)})));
        } else if (ev.mask & IN_MODIFY) {
            handler_(result(make_event(event_kind::Modify, {std::move(path)}, modify_kind::Data)));
        } else if (ev.mask & IN_ATTRIB) {
            handler_(result(make_event(event_kind::Modify, {std::move(path)}, modify_kind::Metadata)));
        } else if (ev.mask & IN_MOVE_SELF) {
            // The watched item itself was moved; inotify doesn't say where to.
            handler_(result(make_event(event_kind::RenameFrom, {std::move(path)})));
        }
    }

    void flush_rename() {
        if (!pending_from_) return;
        fs::path from = std::move(pending_from_->second);
        pending_from_.reset();
        handler_(result(make_event(event_kind::RenameFrom, {std::move(from)})));
    }

    Handler handler_;
    int inotify_fd_ = -1;
    int wake_fd_ = -1;

    std::mutex mutex_;  // guards by_wd_ / by_path_
    std::unordered_map<int, fs::path> by_wd_;
    std::unordered_map<std::string, int> by_path_;

    std::optional<std::pair<std::uint32_t, fs::path>> pending_from_;  // worker thread only
    std::thread worker_;
};

}  // namespace notify

#include "watcher_common.inl"
