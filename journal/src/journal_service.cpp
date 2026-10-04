#include "journal/journal_service.hpp"

#include <atomic>
#include <chrono>
#include <deque>
#include <map>
#include <mutex>
#include <thread>
#include <unordered_map>

#include <notify/notify.hpp>
#include <spdlog/spdlog.h>

#include "feed.hpp"
#include "journal_files.hpp"

namespace journal {

namespace fs = std::filesystem;
using namespace std::chrono_literals;

const char* to_string(phase p) {
    switch (p) {
        case phase::history: return "history";
        case phase::catchup: return "catchup";
        case phase::live: return "live";
    }
    return "?";
}

namespace {

// How long the ingest thread waits for a notification when it has nothing else to do; also the
// worst-case delay before it notices a newly posted task.
constexpr auto idle_wait = 100ms;
// How often open journals are re-read regardless of notifications, and how often the directory
// is checked for a new journal the same way.
constexpr auto poll_interval = 1s;
constexpr auto rollover_interval = 10s;

// Matches how notify reports a watched directory: absolute, normalised, no trailing separator.
fs::path normalise(const fs::path& p) {
    fs::path abs = fs::absolute(p).lexically_normal();
    if (!abs.has_filename() && abs != abs.root_path()) abs = abs.parent_path();
    return abs;
}

}  // namespace

struct journal_service::impl {
    explicit impl(std::optional<history_config> cfg) : config(std::move(cfg)) {
        if (config) fs::create_directories(config->directory);
        thread = std::thread([this] { run(); });
    }

    ~impl() {
        stopping = true;
        thread.join();
    }

    void post(std::function<void()> task) {
        std::lock_guard lock(mutex);
        tasks.push_back(std::move(task));
    }

    std::optional<std::function<void()>> next_task() {
        std::lock_guard lock(mutex);
        if (tasks.empty()) return std::nullopt;
        auto task = std::move(tasks.front());
        tasks.pop_front();
        return task;
    }

    static void run_safely(const std::function<void()>& task) {
        try {
            task();
        } catch (const std::exception& e) {
            spdlog::error("journal task failed: {}", e.what());
        }
    }

    void dispatch(const notify::result& r) {
        if (!r) {
            spdlog::warn("journal watch error: {}", r.error().code.message());
            return;
        }
        const notify::event& e = r.event();
        if (e.need_rescan) {
            spdlog::warn("journal watch overflow; rescanning");
            for (auto& [_, feeds] : by_directory)
                for (auto& f : feeds) f->on_overflow();
            return;
        }
        if (e.kind == notify::event_kind::Remove || e.kind == notify::event_kind::RenameFrom) return;

        for (const auto& path : e.paths) {
            if (!files::journal_file::of(path)) continue;  // Status.json, Market.json and friends
            const auto it = by_directory.find(path.parent_path());
            if (it == by_directory.end()) continue;
            for (auto& f : it->second) f->on_file_event(path);
        }
    }

    void poll(bool check_rollover) {
        for (auto& [_, feeds] : by_directory)
            for (auto& f : feeds) run_safely([&] { f->poll(check_rollover); });
    }

    // The single ingest thread. Notifications are drained between tasks, so live tails stay
    // responsive while scans advance a file at a time.
    void run() {
        auto last_poll = std::chrono::steady_clock::now();
        auto last_rollover = last_poll;

        while (!stopping) {
            while (auto r = watcher.receive_for(0ms)) dispatch(*r);

            if (auto task = next_task()) {
                run_safely(*task);
            } else if (auto r = watcher.receive_for(idle_wait)) {
                dispatch(*r);
            }

            const auto now = std::chrono::steady_clock::now();
            if (now - last_poll >= poll_interval) {
                const bool rollover = now - last_rollover >= rollover_interval;
                poll(rollover);
                last_poll = now;
                if (rollover) last_rollover = now;
            }
        }

        std::lock_guard lock(mutex);
        for (auto& [_, f] : feeds) f->shutdown();
    }

    std::optional<history_config> config;
    notify::watcher watcher;
    std::atomic<bool> stopping{false};

    std::mutex mutex;  // guards tasks and feeds
    std::deque<std::function<void()>> tasks;
    std::unordered_map<std::string, std::shared_ptr<feed>> feeds;

    std::map<fs::path, std::vector<std::shared_ptr<feed>>> by_directory;  // ingest thread only

    std::thread thread;  // last, so it starts after everything it uses exists
};

journal_service::journal_service(std::optional<history_config> history)
    : impl_(std::make_unique<impl>(std::move(history))) {}

journal_service::~journal_service() = default;

std::shared_ptr<commander_feed> journal_service::open(const std::string& id, const fs::path& directory) {
    const fs::path dir = normalise(directory);

    std::lock_guard lock(impl_->mutex);
    if (const auto it = impl_->feeds.find(id); it != impl_->feeds.end()) return it->second;

    // Watch before anything is read, so a write landing during the scan is already caught.
    impl_->watcher.watch(dir);

    auto f = std::make_shared<feed>(id, dir, impl_->config, [i = impl_.get()](std::function<void()> task) {
        i->post(std::move(task));
    });
    impl_->feeds.emplace(id, f);
    impl_->tasks.push_back([i = impl_.get(), f, dir] { i->by_directory[dir].push_back(f); });
    return f;
}

void journal_service::close(const std::string& id) {
    std::lock_guard lock(impl_->mutex);
    const auto it = impl_->feeds.find(id);
    if (it == impl_->feeds.end()) return;
    auto f = std::move(it->second);
    impl_->feeds.erase(it);

    impl_->tasks.push_back([i = impl_.get(), f] {
        const fs::path& dir = f->directory();
        if (auto list = i->by_directory.find(dir); list != i->by_directory.end()) {
            std::erase(list->second, f);
            if (list->second.empty()) {
                i->by_directory.erase(list);
                try {
                    i->watcher.unwatch(dir);
                } catch (const fs::filesystem_error& e) {
                    spdlog::warn("cannot stop watching {}: {}", files::utf8(dir), e.what());
                }
            }
        }
        f->shutdown();
    });
}

}  // namespace journal
