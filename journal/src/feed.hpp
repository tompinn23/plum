#pragma once

#include <array>
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
#include <tuple>
#include <utility>
#include <vector>

#include "archive.hpp"
#include "history_store.hpp"
#include "journal/journal_service.hpp"
#include "journal_files.hpp"
#include "journal_parser.hpp"

namespace journal {
    // Listeners added from any thread and called on the ingest thread. Delivery iterates an
    // immutable snapshot, so adding or removing never blocks behind a slow listener.
    template<class F>
    class listener_list : public std::enable_shared_from_this<listener_list<F> > {
    public:
        using entry = std::pair<std::uint64_t, F>;

        // `id`, if given, receives the listener's id, for finding it in a snapshot later.
        subscription add(F fn, std::uint64_t *id_out = nullptr) {
            std::lock_guard lock(mutex_);
            auto next = std::make_shared<std::vector<entry> >(*items_);
            const auto id = ++next_id_;
            if (id_out) *id_out = id;
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
            auto next = std::make_shared<std::vector<entry> >(*items_);
            std::erase_if(*next, [id](const entry &e) { return e.first == id; });
            items_ = std::move(next);
        }

        mutable std::mutex mutex_;
        std::shared_ptr<const std::vector<entry>> items_ = std::make_shared<const std::vector<entry>>();
        std::uint64_t next_id_ = 0;
    };

    // Where a feed's next scan step sits in the service's queue: catch-up first, so a long backfill
    // elsewhere never holds back a directory's live state, then history oldest journal first across
    // every directory, so a commander playing from several directories gets their journals in order.
    using scan_key = std::tuple<int, std::array < int, 6>
    ,
    int
    >;

    // One journal directory's ingest pipeline. Everything except the commander_feed interface runs on
    // the service's ingest thread; the scan is cut into one-file steps that the service runs in
    // scan_key order across all feeds.
    class feed final : public commander_feed, public std::enable_shared_from_this<feed> {
    public:
        using poster = std::function<void(std::function < void() >)>;

        struct hooks {
            poster post; // runs a task on the ingest thread
            std::function<void(std::shared_ptr<feed>)> scan; // hands a ready scan to the scheduler
        };

        // `history` is null when the service keeps none; it outlives every feed.
        feed(std::string id, std::filesystem::path directory, archive *history, hooks hooks);

        // commander_feed — any thread
        [[nodiscard]] const std::string &id() const override { return id_; }
        [[nodiscard]] const std::filesystem::path &directory() const override { return directory_; }
        [[nodiscard]] std::shared_ptr<const game_state> state() const override { return published_.load(); }

        [[nodiscard]] journal::history history() const override;

        subscription subscribe(std::set<std::string> events, event_listener listener, std::set<phase> phases) override;

        subscription on_progress(progress_listener listener) override;

        [[nodiscard]] bool game_running() const override { return game_running_.load(); }

        [[nodiscard]] std::vector<std::uint32_t> game_pids() const override;

        void inject(game_event event) override;

        void start() override;

        // ingest thread only
        [[nodiscard]] std::optional<scan_key> next_scan() const;

        void scan_step();

        void on_file_event(const std::filesystem::path &file);

        void on_overflow();

        void poll(bool check_rollover);

        void set_game_running(bool running);

        void set_game_pids(std::vector<std::uint32_t> pids);

        void shutdown();

    private:
        struct event_sub {
            std::set<std::string> events;
            std::set<phase> phases;
            event_listener fn;
        };

        void begin_scan();

        void finish_scan();

        bool replay_historical(const files::journal_file &file, const archive::attribution &owner);

        void follow(const std::string &key);

        void begin_recording(const std::string &key, const files::journal_file &file, const std::string &started);

        void claim_current(const game_event &event);

        void open_file(const files::journal_file &file);

        void close_file();

        void read_from(const std::filesystem::path &file);

        void pump();

        void handle(std::string_view line, std::uint64_t offset);

        void publish_state();

        void dispatch(const game_event &event, phase p);

        void greet(std::uint64_t subscriber);

        void report(ingest_progress p);

        const std::string id_;
        const std::filesystem::path directory_;
        archive *const archive_;
        const hooks hooks_;

        std::atomic<bool> started_{false};
        std::atomic<bool> game_running_{false};
        std::atomic<std::shared_ptr<const std::vector<std::uint32_t> > > game_pids_;
        // The store of whoever is playing here: the owner of the journal being tailed, or before
        // that, whoever this directory last saw. Null without history, or until anyone is known.
        std::atomic<std::shared_ptr<const std::filesystem::path> > store_file_;
        std::atomic<std::shared_ptr<const game_state> > published_;
        std::shared_ptr<listener_list<event_sub> > subs_ = std::make_shared<listener_list<event_sub> >();
        std::shared_ptr<listener_list<state_listener> > state_subs_ = std::make_shared<listener_list<
            state_listener> >();
        std::shared_ptr<listener_list<progress_listener> > progress_subs_ =
                std::make_shared<listener_list<progress_listener> >();

        // ── ingest thread only ──
        bool stopped_ = false;
        bool scanning_ = false;
        phase phase_ = phase::history;
        journal_parser parser_;
        std::optional<std::string> following_; // key behind store_file_

        std::vector<files::journal_file> scan_files_;
        std::vector<archive::attribution> scan_owners_; // parallel to scan_files_
        std::size_t scan_index_ = 0;
        std::size_t catchup_from_ = 0; // first file replayed as catchup rather than history
        int scan_skipped_ = 0;
        int files_total_ = 0; // journals in the directory: the scan's, then each new one
        bool live_reported_ = false; // the first phase::live report has gone out

        // Where lines read now are recorded: the journal being replayed during the scan, then the one
        // being tailed. Null for a journal with no history to go to.
        history_store *store_ = nullptr;
        history_store::batch batch_;

        std::optional<files::journal_file> current_; // the file being tailed
        std::string current_started_;
        std::uint64_t position_ = 0; // bytes of it consumed
        // The journal being tailed has named no commander yet. Its recorded lines wait here, since
        // which store they belong in is not known until someone logs in.
        bool awaiting_owner_ = false;
        std::vector<std::pair<std::string, std::uint64_t> > pending_;
        // The newest journal read has events after its last Shutdown, if it has one: a session the
        // game has not closed. Decides whether a game that exits needs a Shutdown made for it.
        bool session_open_ = false;
    };
} // namespace journal
