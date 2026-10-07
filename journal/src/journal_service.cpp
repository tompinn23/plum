#include "journal/journal_service.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <deque>
#include <map>
#include <mutex>
#include <thread>
#include <unordered_map>

#include <notify/notify.hpp>
#include <spdlog/spdlog.h>

#include "archive.hpp"
#include "feed.hpp"
#include "game_process.hpp"
#include "journal_files.hpp"

namespace journal {
    namespace fs = std::filesystem;
    using namespace std::chrono_literals;

    const char *to_string(phase p) {
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
        // How often each directory's game_probe is asked, and the most often the default probe will do a
        // full process scan while the game is not found.
        constexpr auto game_interval = 5s;

        // Matches how notify reports a watched directory: absolute, normalised, no trailing separator.
        fs::path normalise(const fs::path &p) {
            fs::path abs = fs::absolute(p).lexically_normal();
            if (!abs.has_filename() && abs != abs.root_path()) abs = abs.parent_path();
            return abs;
        }
    } // namespace

    game_probe process_game_probe() {
        struct tracker {
            std::map<fs::path, std::optional<process::user> > owners; // per directory, read once
            std::vector<process::game> games; // clients found by the last scan
            std::optional<std::chrono::steady_clock::time_point> last_scan;
        };
        auto t = std::make_shared<tracker>();

        return [t](const fs::path &directory) {
            auto [it, inserted] = t->owners.try_emplace(directory);
            if (inserted) {
                it->second = process::owner_of(directory);
                if (it->second) spdlog::debug("{} is owned by {}", files::utf8(directory), *it->second);
                if (!it->second)
                    spdlog::warn("cannot tell who owns {}; any running game will count",
                                 files::utf8(directory));
            }
            const auto &owner = it->second;
            // A client that could not be inspected is someone else's: our own processes always can be.
            const auto ours = [&](const process::game &g) { return !owner || (g.owner && *g.owner == *owner); };

            const auto pids = [&] {
                std::vector<std::uint32_t> out;
                for (const auto &g: t->games)
                    if (ours(g)) out.push_back(g.pid);
                return out;
            };

            // Tracked clients are checked by pid, which is cheap; only the gone are dropped.
            std::erase_if(t->games, [](const process::game &g) { return !process::is_game(g.pid); });
            if (auto found = pids(); !found.empty()) return found;

            const auto now = std::chrono::steady_clock::now();
            if (t->last_scan && now - *t->last_scan < game_interval) return std::vector<std::uint32_t>{};
            t->last_scan = now;
            t->games = process::running_games();
            spdlog::trace("process scan found {} game clients", t->games.size());
            for (const auto &g: t->games)
                spdlog::debug("game client pid {} owned by {}", g.pid, g.owner.value_or("?"));
            return pids();
        };
    }

    struct journal_service::impl {
        impl(std::optional<history_config> cfg, game_probe game) : config(std::move(cfg)), probe(std::move(game)) {
            if (config) {
                try {
                    history.emplace(*config);
                } catch (const history_error &e) {
                    // Costs the history, not the live dashboard.
                    spdlog::error("history disabled: {}", e.what());
                }
            } else {
                spdlog::info("journal service running without history");
            }
            thread = std::thread([this] { run(); });
        }

        ~impl() {
            spdlog::debug("journal service stopping");
            stopping = true;
            thread.join();
            spdlog::debug("journal service stopped");
        }

        void post(std::function<void()> task) {
            std::lock_guard lock(mutex);
            tasks.push_back(std::move(task));
        }

        std::optional<std::function<void()> > next_task() {
            std::lock_guard lock(mutex);
            if (tasks.empty()) return std::nullopt;
            auto task = std::move(tasks.front());
            tasks.pop_front();
            return task;
        }

        static void run_safely(const std::function<void()> &task) {
            try {
                task();
            } catch (const std::exception &e) {
                spdlog::error("journal task failed: {}", e.what());
            }
        }

        void dispatch(const notify::result &r) {
            if (!r) {
                spdlog::warn("journal watch error: {}", r.error().code.message());
                return;
            }
            const notify::event &e = r.event();
            if (e.need_rescan) {
                spdlog::warn("journal watch overflow; rescanning");
                for (auto &[_, feeds]: by_directory)
                    for (auto &f: feeds) f->on_overflow();
                return;
            }
            if (e.kind == notify::event_kind::Remove || e.kind == notify::event_kind::RenameFrom) return;

            for (const auto &path: e.paths) {
                if (!files::journal_file::of(path)) continue; // Status.json, Market.json and friends
                spdlog::trace("journal changed: {}", files::utf8(path));
                const auto it = by_directory.find(path.parent_path());
                if (it == by_directory.end()) continue;
                for (auto &f: it->second) f->on_file_event(path);
            }
        }

        // Runs one scan step: whichever feed's next journal is oldest, catch-up before all, so
        // journals reach each commander's store in time order wherever they came from.
        void step_scans() {
            step_queued = false;
            std::erase_if(scanning, [](const auto &f) { return !f->next_scan(); });
            if (scanning.empty()) return;

            const auto next = std::ranges::min_element(scanning, {}, [](const auto &f) { return *f->next_scan(); });
            const auto f = *next;
            spdlog::trace("[{}] scan step ({} feeds scanning)", f->id(), scanning.size());
            run_safely([&] { f->scan_step(); });

            // One step per task, so live notifications and other work get a look in between.
            queue_scans();
        }

        void queue_scans() {
            if (step_queued) return;
            step_queued = true;
            post([this] { step_scans(); });
        }

        // Tells each directory's feeds which game clients are running there.
        void check_game(const fs::path &directory, const std::vector<std::shared_ptr<feed> > &list) {
            std::vector<std::uint32_t> pids;
            try {
                if (probe) pids = probe(directory);
            } catch (const std::exception &e) {
                spdlog::warn("cannot tell whether the game is running for {}: {}", files::utf8(directory), e.what());
            }
            // Pids first, so anything reacting to the game starting can already find its window.
            for (const auto &f: list) {
                f->set_game_pids(pids);
                f->set_game_running(!pids.empty());
            }
        }

        void check_games() {
            for (const auto &[directory, list]: by_directory) check_game(directory, list);
        }

        void poll(bool check_rollover) {
            for (auto &[_, feeds]: by_directory)
                for (auto &f: feeds) run_safely([&] { f->poll(check_rollover); });
        }

        // The single ingest thread. Notifications are drained between tasks, so live tails stay
        // responsive while scans advance a file at a time.
        void run() {
            auto last_poll = std::chrono::steady_clock::now();
            auto last_rollover = last_poll;
            auto last_game = last_poll;
            spdlog::debug("journal ingest thread started");

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
                if (now - last_game >= game_interval) {
                    check_games();
                    last_game = now;
                }
            }

            {
                std::lock_guard lock(mutex);
                for (auto &[_, f]: feeds) f->shutdown();
            }
            if (history) history->close();
            spdlog::debug("journal ingest thread finished");
        }

        std::optional<history_config> config;
        std::optional<archive> history; // when config is set and the catalog could be opened
        game_probe probe; // ingest thread only
        notify::watcher watcher;
        std::atomic<bool> stopping{false};

        std::mutex mutex; // guards tasks and feeds
        std::deque<std::function<void()> > tasks;
        std::unordered_map<std::string, std::shared_ptr<feed> > feeds;

        // ingest thread only
        std::map<fs::path, std::vector<std::shared_ptr<feed> > > by_directory;
        std::vector<std::shared_ptr<feed> > scanning;
        bool step_queued = false;

        std::thread thread; // last, so it starts after everything it uses exists
    };

    journal_service::journal_service(std::optional<history_config> history, game_probe probe)
        : impl_(std::make_unique<impl>(std::move(history), std::move(probe))) {
    }

    journal_service::~journal_service() = default;

    std::shared_ptr<commander_feed> journal_service::open(const std::string &id, const fs::path &directory) {
        const fs::path dir = normalise(directory);

        std::lock_guard lock(impl_->mutex);
        if (const auto it = impl_->feeds.find(id); it != impl_->feeds.end()) return it->second;

        // Watch before anything is read, so a write landing during the scan is already caught.
        spdlog::info("[{}] watching {}", id, files::utf8(dir));
        impl_->watcher.watch(dir);

        auto *i = impl_.get();
        auto f = std::make_shared<feed>(id, dir, i->history ? &*i->history : nullptr,
                                        feed::hooks{
                                            .post = [i](std::function<void()> task) { i->post(std::move(task)); },
                                            .scan =
                                            [i](std::shared_ptr<feed> ready) {
                                                i->scanning.push_back(std::move(ready));
                                                i->queue_scans();
                                            },
                                        });
        impl_->feeds.emplace(id, f);
        // Asked straight away, so the feed knows whether the game is up before its scan finishes.
        impl_->tasks.push_back([i = impl_.get(), f, dir] {
            i->by_directory[dir].push_back(f);
            i->check_game(dir, {f});
        });
        return f;
    }

    std::optional<std::string> latest_commander(const fs::path &directory) {
        try {
            const auto journals = files::list(directory);
            for (auto it = journals.rbegin(); it != journals.rend(); ++it) {
                if (auto name = files::read_head(it->path).commander) return name;
            }
        } catch (const fs::filesystem_error &e) {
            spdlog::debug("cannot list {}: {}", files::utf8(directory), e.what());
        }
        return std::nullopt;
    }

    journal::history journal_service::history(const std::string &key) const {
        if (!impl_->config) return {};
        const auto file = archive::store_file(impl_->config->directory, key);
        std::error_code ec;
        return fs::exists(file, ec) ? journal::history(file) : journal::history();
    }

    std::vector<commander_info> journal_service::commanders() const {
        std::vector<commander_info> out;
        if (!impl_->config) return out;

        std::error_code ec;
        for (const auto &entry: fs::directory_iterator(impl_->config->directory, ec)) {
            if (entry.path().extension() != ".db" || entry.path().filename() == "catalog.db") continue;
            try {
                sql::database db(entry.path());
                auto s = db.prepare("SELECT key, value FROM meta WHERE key IN ('commander_fid', 'commander_name')");
                std::optional<std::string> fid, name;
                while (s.step()) (s.text(0) == "commander_fid" ? fid : name) = s.text(1);
                if (!fid && !name) continue;
                out.push_back({fid ? *fid : "cmdr:" + *name, name ? *name : *fid, entry.path()});
            } catch (const sql::error &e) {
                spdlog::warn("cannot read {}: {}", files::utf8(entry.path()), e.what());
            }
        }
        return out;
    }

    void journal_service::close(const std::string &id) {
        std::lock_guard lock(impl_->mutex);
        const auto it = impl_->feeds.find(id);
        if (it == impl_->feeds.end()) return;
        auto f = std::move(it->second);
        impl_->feeds.erase(it);
        spdlog::info("[{}] closing", id);

        impl_->tasks.push_back([i = impl_.get(), f] {
            const fs::path &dir = f->directory();
            if (auto list = i->by_directory.find(dir); list != i->by_directory.end()) {
                std::erase(list->second, f);
                if (list->second.empty()) {
                    spdlog::debug("no longer watching {}", files::utf8(dir));
                    i->by_directory.erase(list);
                    try {
                        i->watcher.unwatch(dir);
                    } catch (const fs::filesystem_error &e) {
                        spdlog::warn("cannot stop watching {}: {}", files::utf8(dir), e.what());
                    }
                }
            }
            f->shutdown();
        });
    }
} // namespace journal
