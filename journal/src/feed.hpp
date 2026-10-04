#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "history_store.hpp"
#include "journal/journal_service.hpp"
#include "journal_files.hpp"
#include "journal_parser.hpp"

namespace journal {

// Listeners added from any thread and called on the ingest thread. Delivery iterates an
// immutable snapshot, so adding or removing never blocks behind a slow listener.
template <class F>
class listener_list : public std::enable_shared_from_this<listener_list<F>> {
public:
    using entry = std::pair<std::uint64_t, F>;

    subscription add(F fn) {
        std::lock_guard lock(mutex_);
        auto next = std::make_shared<std::vector<entry>>(*items_);
        const auto id = ++next_id_;
        next->emplace_back(id, std::move(fn));
        items_ = std::move(next);
        return subscription([weak = this->weak_from_this(), id] {
            if (auto self = weak.lock()) self->remove(id);
        });
    }

    [[nodiscard]] std::shared_ptr<const std::vector<entry>> snapshot() const {
        std::lock_guard lock(mutex_);
        return items_;
    }

    void clear() {
        std::lock_guard lock(mutex_);
        items_ = std::make_shared<const std::vector<entry>>();
    }

private:
    void remove(std::uint64_t id) {
        std::lock_guard lock(mutex_);
        auto next = std::make_shared<std::vector<entry>>(*items_);
        std::erase_if(*next, [id](const entry& e) { return e.first == id; });
        items_ = std::move(next);
    }

    mutable std::mutex mutex_;
    std::shared_ptr<const std::vector<entry>> items_ = std::make_shared<const std::vector<entry>>();
    std::uint64_t next_id_ = 0;
};

// One commander's ingest pipeline. Everything except the commander_feed interface runs on the
// service's ingest thread; the scan is cut into one-file steps posted back to that thread so
// feeds take turns.
class feed final : public commander_feed, public std::enable_shared_from_this<feed> {
public:
    using poster = std::function<void(std::function<void()>)>;

    feed(std::string id, std::filesystem::path directory, std::optional<history_config> config, poster post);

    // commander_feed — any thread
    [[nodiscard]] const std::string& id() const override { return id_; }
    [[nodiscard]] const std::filesystem::path& directory() const override { return directory_; }
    [[nodiscard]] std::shared_ptr<const game_state> state() const override { return published_.load(); }
    [[nodiscard]] journal::history history() const override;
    subscription subscribe(std::set<std::string> events, event_listener listener, std::set<phase> phases) override;
    subscription on_state(state_listener listener) override;
    subscription on_progress(progress_listener listener) override;
    void start() override;

    // ingest thread only
    void on_file_event(const std::filesystem::path& file);
    void on_overflow();
    void poll(bool check_rollover);
    void shutdown();

private:
    struct event_sub {
        std::set<std::string> events;
        std::set<phase> phases;
        event_listener fn;
    };

    void begin_scan();
    void scan_step();
    void finish_scan();
    void bind_commander();
    bool replay_historical(const files::journal_file& file);
    [[nodiscard]] bool ours(const std::optional<std::string>& owner) const;
    void open_file(const files::journal_file& file);
    void close_file();
    void read_from(const std::filesystem::path& file);
    void pump();
    void handle(std::string_view line, std::uint64_t offset);
    void publish_state();
    void report(ingest_progress p);

    const std::string id_;
    const std::filesystem::path directory_;
    const std::optional<history_config> config_;
    const std::optional<std::filesystem::path> store_file_;
    const poster post_;

    std::atomic<bool> started_{false};
    std::atomic<std::shared_ptr<const game_state>> published_;
    std::shared_ptr<listener_list<event_sub>> subs_ = std::make_shared<listener_list<event_sub>>();
    std::shared_ptr<listener_list<state_listener>> state_subs_ = std::make_shared<listener_list<state_listener>>();
    std::shared_ptr<listener_list<progress_listener>> progress_subs_ =
        std::make_shared<listener_list<progress_listener>>();

    // ── ingest thread only ──
    bool stopped_ = false;
    phase phase_ = phase::history;
    journal_parser parser_;
    history_store store_;

    std::vector<files::journal_file> scan_files_;
    std::size_t scan_index_ = 0;
    std::size_t catchup_from_ = 0;  // first file replayed as catchup rather than history
    int scan_skipped_ = 0;

    std::optional<files::journal_file> current_;  // the file being tailed
    std::uint64_t position_ = 0;                   // bytes of it consumed
    std::optional<std::string> current_owner_;
    // A file belonging to another commander is still tailed, since live state follows whoever is
    // playing, but no store batch is opened for it.
    bool current_recorded_ = false;
};

}  // namespace journal
