#include "feed.hpp"

#include <spdlog/spdlog.h>

namespace journal {
    namespace fs = std::filesystem;

    namespace {
        std::string excerpt(std::string_view line) {
            constexpr std::size_t limit = 200;
            return line.size() <= limit ? std::string(line) : std::string(line.substr(0, limit)) + "...";
        }

        std::optional<std::uint64_t> size_of(const fs::path &file) {
            std::error_code ec;
            const auto size = fs::file_size(file, ec);
            return ec ? std::nullopt : std::optional<std::uint64_t>(size);
        }

        // The session as the parser has it, in the shape of the journal's own LoadGame and Location, for a
        // subscriber arriving mid-session. Only what is known is included.
        game_event startup_event(const game_state &s) {
            json p = json::object();
            p["timestamp"] = format_timestamp(
                std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now()));
            p["event"] = "StartUp";
            const auto put = [&p](const char *key, const auto &value) {
                if (value) p[key] = *value;
            };

            put("Commander", s.name);
            put("FID", s.fid);
            if (!s.mode.empty()) p["GameMode"] = s.mode;
            put("Group", s.group);
            p["Credits"] = s.credits;
            put("Loan", s.loan);
            put("Horizons", s.horizons);
            p["Odyssey"] = s.odyssey;
            put("language", s.game_language);
            put("gameversion", s.game_version);
            put("build", s.game_build);

            put("Ship", s.ship_type);
            put("Ship_Localised", s.ship_localised);
            put("ShipID", s.ship_id);
            put("ShipName", s.ship_name);
            put("ShipIdent", s.ship_ident);

            put("StarSystem", s.system_name);
            put("SystemAddress", s.system_address);
            put("StarPos", s.star_pos);
            put("Population", s.system_population);
            put("Body", s.body);
            put("BodyID", s.body_id);
            put("BodyType", s.body_type);
            p["Docked"] = s.is_docked;
            put("StationName", s.station_name);
            put("StationType", s.station_type);
            put("MarketID", s.market_id);
            p["OnFoot"] = s.on_foot;
            p["Taxi"] = s.taxi;
            put("Captain", s.captain);
            return game_event(std::move(p));
        }

        // A tailed journal that has named nobody is a main-menu session; it holds a few lines at most
        // before someone logs in. Beyond this it is not worth holding them.
        constexpr std::size_t pending_limit = 256;
    } // namespace

    feed::feed(std::string id, fs::path directory, archive *history, hooks hooks)
        : id_(std::move(id)), directory_(std::move(directory)), archive_(history), hooks_(std::move(hooks)),
          published_(std::make_shared<const game_state>()) {
    }

    // ── commander_feed ──────────────────────────────────────────────────────────

    journal::history feed::history() const {
        const auto file = store_file_.load();
        return file ? journal::history(*file) : journal::history();
    }

    subscription feed::subscribe(std::set<std::string> events, event_listener listener, std::set<phase> phases) {
        const bool wants_startup = phases.contains(phase::live) && (events.empty() || events.contains("StartUp"));
        std::uint64_t id = 0;
        auto sub = subs_->add({.events = std::move(events), .phases = std::move(phases), .fn = std::move(listener)},
                              &id);
        // Weak, so a subscription made just before the feed is closed does not keep it alive.
        if (wants_startup)
            hooks_.post([weak = weak_from_this(), id] {
                if (const auto self = weak.lock()) self->greet(id);
            });
        return sub;
    }

    // Onto the ingest thread, so it reaches listeners between journal events rather than among them.
    void feed::inject(game_event event) {
        spdlog::trace("[{}] {} queued for injection", id_, event.name());
        hooks_.post([weak = weak_from_this(), event = std::move(event)] {
            if (const auto self = weak.lock(); self && !self->stopped_) self->dispatch(event, phase::live);
        });
    }

    subscription feed::on_progress(progress_listener listener) {
        return progress_subs_->add(std::move(listener));
    }

    void feed::start() {
        if (started_.exchange(true)) return;
        spdlog::debug("[{}] starting", id_);
        hooks_.post([self = shared_from_this()] { self->begin_scan(); });
    }

    // ── scan ────────────────────────────────────────────────────────────────────
    // Older journals in phase::history; then, in phase::catchup, the newest journal that loaded a
    // commander and anything after it, up to where it currently ends; then live. The directory is
    // watched before this starts, and the newest file is looked up again on going live, so nothing
    // written during the scan is missed.

    void feed::begin_scan() {
        if (stopped_) return;

        try {
            scan_files_ = files::list(directory_);
        } catch (const fs::filesystem_error &e) {
            spdlog::warn("[{}] cannot list journal directory {}: {}", id_, files::utf8(directory_), e.what());
            scan_files_.clear();
        }
        scan_index_ = 0;
        scan_skipped_ = 0;
        phase_ = phase::history;

        // Every journal is attributed before any is read, so a journal from before FIDs can be matched
        // to the FID its commander later had. From the catalog after the first launch; on the first,
        // a read of each journal's head.
        scan_owners_.clear();
        scan_owners_.reserve(scan_files_.size());
        for (const auto &file: scan_files_) {
            if (archive_) {
                scan_owners_.push_back(archive_->attribute(file));
            } else {
                const auto head = files::read_head(file.path);
                scan_owners_.push_back({
                    head.names_commander() ? std::optional<std::string>("") : std::nullopt,
                    head.started.value_or(file.stamp_text())
                });
            }
        }
        // Older journals were attributed before the newer ones that carry the FID; ask again now the
        // catalog has seen them all. A catalog lookup each, no file reads.
        if (archive_) {
            for (std::size_t i = 0; i < scan_files_.size(); ++i) {
                if (scan_owners_[i].key && scan_owners_[i].key->starts_with("cmdr:"))
                    scan_owners_[i] = archive_->attribute(scan_files_[i]);
            }
        }

        // The game restates the whole session when a commander loads, so the state is rebuilt from
        // the newest journal that got that far. Newer ones (the game started and quit at the menu)
        // hold nothing, and starting from them would show an empty dashboard.
        catchup_from_ = scan_files_.empty() ? 0 : scan_files_.size() - 1;
        for (std::size_t i = scan_files_.size(); i-- > 0;) {
            if (scan_owners_[i].key) {
                catchup_from_ = i;
                break;
            }
        }

        // Whoever this directory last saw, so history is there from the start, even if the journals
        // that named them have since been deleted.
        if (archive_) {
            auto owner = archive_->directory_owner(directory_);
            if (catchup_from_ < scan_owners_.size() && scan_owners_[catchup_from_].key)
                owner = scan_owners_[catchup_from_].key;
            if (owner) follow(*owner);
        }

        spdlog::info("[{}] scanning {} journals in {}; state rebuilt from #{}", id_, scan_files_.size(),
                     files::utf8(directory_), catchup_from_);
        report({phase::history, 0, 0, static_cast<int>(scan_files_.size())});
        scanning_ = true;
        hooks_.scan(shared_from_this());
    }

    std::optional<scan_key> feed::next_scan() const {
        if (stopped_ || !scanning_) return std::nullopt;
        if (scan_index_ >= catchup_from_) return scan_key{0, {}, 0};
        const auto &file = scan_files_[scan_index_];
        return scan_key{1, file.stamp, file.part};
    }

    void feed::scan_step() {
        if (stopped_ || !scanning_) return;
        if (scan_index_ >= catchup_from_) {
            finish_scan();
            return;
        }
        if (replay_historical(scan_files_[scan_index_], scan_owners_[scan_index_])) ++scan_skipped_;
        ++scan_index_;
        report({phase::history, static_cast<int>(scan_index_), scan_skipped_, static_cast<int>(scan_files_.size())});
    }

    void feed::finish_scan() {
        scanning_ = false;
        const int total = static_cast<int>(scan_files_.size());
        files_total_ = total;
        if (total > 0) {
            // Re-read from byte zero even when the store already holds them, since the parser needs
            // every line to rebuild the state; the store discards what it has seen by byte offset.
            // read_from rolls from one file to the next exactly as it does live.
            phase_ = phase::catchup;
            spdlog::debug("[{}] catching up over the {} newest journals", id_, scan_files_.size() - catchup_from_);
            for (std::size_t i = catchup_from_; i < scan_files_.size(); ++i) read_from(scan_files_[i].path);
            report({phase::catchup, total, scan_skipped_, total});
        }

        phase_ = phase::live;
        scan_files_.clear();
        scan_files_.shrink_to_fit();
        scan_owners_.clear();
        scan_owners_.shrink_to_fit();

        // The game may have started a new journal while the scan ran.
        try {
            if (const auto newest = files::newest(directory_)) read_from(newest->path);
        } catch (const fs::filesystem_error &e) {
            spdlog::warn("[{}] cannot rescan {}: {}", id_, files::utf8(directory_), e.what());
        }

        // Anything this scan stored out of order is put right before the history is called live.
        if (archive_) archive_->settle();

        publish_state();
        // Started mid-session: tell everyone where things stand rather than leave them waiting for
        // the next LoadGame.
        if (game_running_) dispatch(startup_event(parser_.state()), phase::live);
        live_reported_ = true;
        report({phase::live, files_total_, scan_skipped_, files_total_});
        spdlog::info("[{}] caught up over {} journals ({} already stored or naming no commander); now tailing {}",
                     id_, files_total_, scan_skipped_, files::utf8(directory_));
    }

    // Points history() at a commander's store.
    void feed::follow(const std::string &key) {
        if (!archive_ || following_ == key) return;
        spdlog::info("[{}] following commander {}", id_, key);
        following_ = key;
        archive_->store(key); // opening it brings its projections up to date, journals or not
        store_file_.store(std::make_shared<const fs::path>(archive_->file_for(key)));
        archive_->set_directory_owner(directory_, key);
    }

    // A journal the store has in full is not opened at all. That is what turns the second launch
    // from a long backfill into an instant one, and why phase::history listeners see a journal once
    // ever. One that names no commander is still replayed for listeners, but recorded nowhere.
    //
    // Returns true if the journal was skipped as already stored, or recorded nothing.
    bool feed::replay_historical(const files::journal_file &file, const archive::attribution &owner) {
        const std::string name = files::utf8(file.path.filename());
        const auto size = size_of(file.path);

        store_ = archive_ && owner.key ? archive_->store(*owner.key) : nullptr;
        if (store_ && size &&store_->ingested(name, *size))
        {
            spdlog::trace("[{}] {} already stored", id_, name);
            store_ = nullptr;
            return true;
        }
        spdlog::debug("[{}] replaying {} for {}", id_, name, owner.key.value_or("nobody"));

        try {
            if (store_) batch_ = store_->begin_file(name, owner.started);
            files::read_lines(file.path, 0, [this](std::string_view line, std::uint64_t offset) {
                handle(line, offset);
            });
            if (store_) store_->checkpoint(batch_, size.value_or(0), true);
        } catch (const std::exception &e) {
            // One unreadable file should not abandon the rest of the history.
            if (store_) store_->abort(batch_);
            spdlog::warn("[{}] skipping unreadable journal {}: {}", id_, files::utf8(file.path), e.what());
        }
        const bool recorded = store_ != nullptr;
        store_ = nullptr;
        return !recorded;
    }

    // ── tail ────────────────────────────────────────────────────────────────────

    void feed::begin_recording(const std::string &key, const files::journal_file &file, const std::string &started) {
        store_ = archive_->store(key);
        if (store_) batch_ = store_->begin_file(files::utf8(file.path.filename()), started);
        follow(key);
    }

    void feed::open_file(const files::journal_file &file) {
        spdlog::info("[{}] reading {}", id_, files::utf8(file.path.filename()));
        current_ = file;
        position_ = 0;
        store_ = nullptr;
        awaiting_owner_ = false;
        pending_.clear();
        if (!archive_) return;

        const auto owner = archive_->attribute(file);
        current_started_ = owner.started;
        if (owner.key) {
            begin_recording(*owner.key, file, owner.started);
        } else {
            spdlog::debug("[{}] {} names no commander yet; holding its lines", id_,
                          files::utf8(file.path.filename()));
            awaiting_owner_ = true;
        }
    }

    // The journal being tailed has just named its commander. Its earlier lines were held back until
    // now; they are recorded first, in order, then this one carries on as normal.
    void feed::claim_current(const game_event &event) {
        awaiting_owner_ = false;
        const bool is_commander = event.name() == "Commander";
        const auto key = archive_->claim(*current_, current_started_, event.opt_str("FID"),
                                         event.str(is_commander ? "Name" : "Commander"));
        auto pending = std::exchange(pending_, {});
        if (!key) {
            spdlog::warn("[{}] {} names no commander after all; not recording it", id_,
                         files::utf8(current_->path.filename()));
            return;
        }
        spdlog::debug("[{}] {} belongs to {}; recording {} held lines", id_, files::utf8(current_->path.filename()),
                      *key, pending.size());

        begin_recording(*key, *current_, current_started_);
        if (!store_) return;
        for (const auto &[line, offset]: pending) {
            if (const auto raw = game_event::parse(line)) store_->record(batch_, *raw, line, offset);
        }
    }

    // Closes out the file being left, marking it complete in its store.
    void feed::close_file() {
        if (!current_) return;
        spdlog::debug("[{}] done with {} at byte {}", id_, files::utf8(current_->path.filename()), position_);
        if (store_) store_->checkpoint(batch_, position_, true);
        store_ = nullptr;
        awaiting_owner_ = false;
        pending_.clear();
    }

    void feed::pump() {
        if (!current_) return;
        position_ = files::read_lines(current_->path, position_,
                                      [this](std::string_view line, std::uint64_t offset) { handle(line, offset); });
    }

    // Reads new bytes, rolling over if the game has started a new file.
    void feed::read_from(const fs::path &path) {
        const auto file = files::journal_file::of(path);
        if (!file) return;
        try {
            bool opened = false;
            if (!current_) {
                open_file(*file);
                opened = true;
            } else if (file->path != current_->path) {
                // An older file touched (a backup tool, a duplicate notification) must not roll the
                // state back to a stale session.
                if (!(*current_ < *file)) {
                    spdlog::trace("[{}] ignoring older {}", id_, files::utf8(file->path.filename()));
                    return;
                }
                pump(); // drain the superseded file
                close_file(); // and close it out
                open_file(*file);
                opened = true;
            }
            pump();
            if (store_) store_->checkpoint(batch_, position_, false);

            // A journal the game started after the scan: one more in the directory. Reported once the
            // feed has said it is live, so the first live report stays the one that says so.
            if (opened && phase_ == phase::live) {
                ++files_total_;
                if (live_reported_) report({phase::live, files_total_, scan_skipped_, files_total_});
            }
        } catch (const std::exception &e) {
            if (store_) store_->abort(batch_);
            spdlog::warn("[{}] failed reading {}: {}", id_, files::utf8(path), e.what());
        }
    }

    void feed::on_file_event(const fs::path &file) {
        // Before going live the scan will reach this file anyway, and finish_scan looks for a newer
        // one, so dropping the notification loses nothing.
        if (stopped_ || phase_ != phase::live) return;
        read_from(file);
    }

    // The watcher dropped notifications. The tail is offset-based, so re-reading from the newest file
    // recovers everything appended in the meantime.
    void feed::on_overflow() {
        if (stopped_ || phase_ != phase::live) return;
        spdlog::trace("[{}] rescanning {}", id_, files::utf8(directory_));
        try {
            if (const auto newest = files::newest(directory_)) read_from(newest->path);
        } catch (const fs::filesystem_error &e) {
            spdlog::warn("[{}] cannot rescan {} after watch overflow: {}", id_, files::utf8(directory_), e.what());
        }
    }

    // The safety net for notifications that are late or never come.
    void feed::poll(bool check_rollover) {
        if (stopped_ || phase_ != phase::live) return;
        if (check_rollover) {
            on_overflow();
        } else if (current_) {
            read_from(current_->path);
        }
    }

    // Folds one line in and fans it out.
    void feed::handle(std::string_view line, std::uint64_t offset) {
        const auto raw = game_event::parse(line);
        if (!raw) {
            // Journals do contain the occasional truncated line, usually where the game was killed
            // mid-write. Losing one is far better than abandoning the file.
            spdlog::warn("[{}] unparseable journal line: {}", id_, excerpt(line));
            return;
        }

        // The store gets the raw line; listeners get the parser's result, which may be enriched.
        // Projections must see what a rebuild from the ledger would reproduce.
        if (awaiting_owner_ && (raw->name() == "Commander" || raw->name() == "LoadGame")) claim_current(*raw);
        spdlog::trace("[{}] {} {} at {}", id_, to_string(phase_), raw->name(), offset);
        if (awaiting_owner_) {
            if (pending_.size() < pending_limit) {
                pending_.emplace_back(line, offset);
                if (pending_.size() == pending_limit) spdlog::debug("[{}] holding no more lines", id_);
            }
        } else if (store_) {
            store_->record(batch_, *raw, line, offset);
        }
        const game_event parsed = parser_.parse(*raw, phase_);
        // The game writes Shutdown on its way out, and nothing after it until the next launch.
        session_open_ = raw->name() != "Shutdown";

        // State first, so a listener reading state() sees the event it was just handed.
        if (phase_ == phase::live) publish_state();
        dispatch(parsed, phase_);
    }

    void feed::dispatch(const game_event &event, phase p) {
        for (const auto &[_, sub]: *subs_->snapshot()) {
            if (!sub.phases.contains(p)) continue;
            if (!sub.events.empty() && !sub.events.contains(event.name())) continue;
            try {
                sub.fn(event, p);
            } catch (const std::exception &e) {
                spdlog::warn("[{}] listener threw on {}: {}", id_, event.name(), e.what());
            }
        }
    }

    // A subscriber that arrived while the game is running and the feed is live gets a StartUp of its
    // own. One that arrived earlier gets the one sent on going live instead.
    void feed::greet(std::uint64_t subscriber) {
        if (stopped_ || phase_ != phase::live || !game_running_) return;
        for (const auto &[id, sub]: *subs_->snapshot()) {
            if (id != subscriber) continue;
            try {
                sub.fn(startup_event(parser_.state()), phase::live);
            } catch (const std::exception &e) {
                spdlog::warn("[{}] listener threw on StartUp: {}", id_, e.what());
            }
            return;
        }
    }

    std::vector<std::uint32_t> feed::game_pids() const {
        const auto pids = game_pids_.load();
        return pids ? *pids : std::vector<std::uint32_t>{};
    }

    void feed::set_game_pids(std::vector<std::uint32_t> pids) {
        std::ranges::sort(pids);
        if (const auto old = game_pids_.load(); old ? *old == pids : pids.empty()) return;
        spdlog::debug("[{}] game pids now {}", id_, pids);
        game_pids_.store(std::make_shared<const std::vector<std::uint32_t> >(std::move(pids)));
    }

    // A game launched while being watched restates the session in its own journal, so there is
    // nothing to synthesise. One that goes away without writing Shutdown crashed or was killed, and
    // gets a Shutdown made for it, so listeners do not go on showing a session that has ended.
    void feed::set_game_running(bool running) {
        if (game_running_.exchange(running) == running) return;
        spdlog::info("[{}] game {}", id_, running ? "running" : "not running");
        if (running || stopped_ || phase_ != phase::live) return;

        // A clean exit writes Shutdown just before the process ends, which can beat the file
        // notification here; read what is there first. The newest file, in case the game rolled over.
        try {
            if (const auto newest = files::newest(directory_)) read_from(newest->path);
        } catch (const fs::filesystem_error &e) {
            spdlog::warn("[{}] cannot rescan {}: {}", id_, files::utf8(directory_), e.what());
        }
        if (!session_open_) return;

        session_open_ = false;
        spdlog::warn("[{}] game exited without a Shutdown; it probably crashed", id_);
        json shutdown = json::object();
        shutdown["timestamp"] = format_timestamp(
            std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now()));
        shutdown["event"] = "Shutdown";
        dispatch(game_event(std::move(shutdown)), phase::live);
    }

    void feed::publish_state() {
        auto snapshot = std::make_shared<const game_state>(parser_.state());
        published_.store(snapshot);
        for (const auto &[_, fn]: *state_subs_->snapshot()) {
            try {
                fn(snapshot);
            } catch (const std::exception &e) {
                spdlog::warn("[{}] state listener threw: {}", id_, e.what());
            }
        }
    }

    void feed::report(ingest_progress p) {
        for (const auto &[_, fn]: *progress_subs_->snapshot()) {
            try {
                fn(p);
            } catch (const std::exception &e) {
                spdlog::warn("[{}] progress listener threw: {}", id_, e.what());
            }
        }
    }

    void feed::shutdown() {
        if (stopped_) return;
        spdlog::debug("[{}] shutting down", id_);
        stopped_ = true;
        scanning_ = false;
        // Every batch is checkpointed within the task that wrote it, so there is nothing to commit;
        // the stores themselves belong to the archive.
        store_ = nullptr;
        subs_->clear();
        state_subs_->clear();
        progress_subs_->clear();
    }
} // namespace journal
