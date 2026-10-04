#include "feed.hpp"

#include <spdlog/spdlog.h>

namespace journal {

namespace fs = std::filesystem;

namespace {

std::string excerpt(std::string_view line) {
    constexpr std::size_t limit = 200;
    return line.size() <= limit ? std::string(line) : std::string(line.substr(0, limit)) + "...";
}

std::optional<std::uint64_t> size_of(const fs::path& file) {
    std::error_code ec;
    const auto size = fs::file_size(file, ec);
    return ec ? std::nullopt : std::optional<std::uint64_t>(size);
}

std::optional<fs::path> store_path(const std::string& id, const std::optional<history_config>& config) {
    if (!config) return std::nullopt;
    return config->directory / (id + ".db");
}

}  // namespace

feed::feed(std::string id, fs::path directory, std::optional<history_config> config, poster post)
    : id_(std::move(id)), directory_(std::move(directory)), config_(std::move(config)),
      store_file_(store_path(id_, config_)), post_(std::move(post)),
      published_(std::make_shared<const game_state>()) {}

// ── commander_feed ──────────────────────────────────────────────────────────

journal::history feed::history() const {
    return store_file_ ? journal::history(*store_file_) : journal::history();
}

subscription feed::subscribe(std::set<std::string> events, event_listener listener, std::set<phase> phases) {
    return subs_->add({std::move(events), std::move(phases), std::move(listener)});
}

subscription feed::on_state(state_listener listener) {
    return state_subs_->add(std::move(listener));
}

subscription feed::on_progress(progress_listener listener) {
    return progress_subs_->add(std::move(listener));
}

void feed::start() {
    if (started_.exchange(true)) return;
    post_([self = shared_from_this()] { self->begin_scan(); });
}

// ── scan ────────────────────────────────────────────────────────────────────
// Older journals in phase::history; then, in phase::catchup, the newest journal that loaded a
// commander and anything after it, up to where it currently ends; then live. The directory is
// watched before this starts, and the newest file is looked up again on going live, so nothing
// written during the scan is missed.

void feed::begin_scan() {
    if (stopped_) return;

    if (store_file_) {
        try {
            store_ = history_store(*store_file_, *config_);
        } catch (const history_error& e) {
            // A corrupt or locked store should cost the history, not the live dashboard.
            spdlog::error("[{}] history disabled: {}", id_, e.what());
        }
    }

    try {
        scan_files_ = files::list(directory_);
    } catch (const fs::filesystem_error& e) {
        spdlog::warn("[{}] cannot list journal directory {}: {}", id_, files::utf8(directory_), e.what());
        scan_files_.clear();
    }
    scan_index_ = 0;
    scan_skipped_ = 0;
    phase_ = phase::history;

    // The game restates the whole session when a commander loads, so the state is rebuilt from
    // the newest journal that got that far. Newer ones (the game started and quit at the menu)
    // hold nothing, and starting from them would show an empty dashboard.
    catchup_from_ = scan_files_.empty() ? 0 : scan_files_.size() - 1;
    for (std::size_t i = scan_files_.size(); i-- > 0;) {
        if (files::names_commander(scan_files_[i].path)) {
            catchup_from_ = i;
            break;
        }
    }
    report({phase::history, 0, 0, static_cast<int>(scan_files_.size())});

    bind_commander();
    post_([self = shared_from_this()] { self->scan_step(); });
}

void feed::scan_step() {
    if (stopped_) return;
    const std::size_t total = scan_files_.size();
    if (scan_index_ >= catchup_from_) {
        finish_scan();
        return;
    }
    if (replay_historical(scan_files_[scan_index_])) ++scan_skipped_;
    ++scan_index_;
    report({phase::history, static_cast<int>(scan_index_), scan_skipped_, static_cast<int>(total)});

    // One file per turn, then back of the queue, so other feeds' scans and every live
    // notification get a look in.
    post_([self = shared_from_this()] { self->scan_step(); });
}

void feed::finish_scan() {
    const int total = static_cast<int>(scan_files_.size());
    if (total > 0) {
        // Re-read from byte zero even when the store already holds them, since the parser needs
        // every line to rebuild the state; the store discards what it has seen by byte offset.
        // read_from rolls from one file to the next exactly as it does live.
        phase_ = phase::catchup;
        for (std::size_t i = catchup_from_; i < scan_files_.size(); ++i) read_from(scan_files_[i].path);
        report({phase::catchup, total, scan_skipped_, total});
    }

    phase_ = phase::live;
    scan_files_.clear();
    scan_files_.shrink_to_fit();

    // The game may have started a new journal while the scan ran.
    try {
        if (const auto newest = files::newest(directory_)) read_from(newest->path);
    } catch (const fs::filesystem_error& e) {
        spdlog::warn("[{}] cannot rescan {}: {}", id_, files::utf8(directory_), e.what());
    }

    publish_state();
    report({phase::live, total, scan_skipped_, total});
    spdlog::info("[{}] caught up over {} journals ({} already stored or another commander's); now tailing {}",
                 id_, total, scan_skipped_, files::utf8(directory_));
}

// A journal directory is not a commander, so the store is bound to whoever played most recently,
// working backwards until a file names someone. Once made the binding is permanent. Costs one
// small read on a store's first launch and nothing after.
void feed::bind_commander() {
    if (!store_.enabled() || store_.commander()) return;
    for (auto it = scan_files_.rbegin(); it != scan_files_.rend(); ++it) {
        if (const auto owner = files::owner_of(it->path)) {
            store_.bind(owner->fid, owner->name);
            return;
        }
    }
    spdlog::info("[{}] no commander named in {}; history is unattributed", id_, files::utf8(directory_));
}

// An unattributed file counts as ours: it names no commander, so holds nothing commander-specific
// to contaminate the history with, and excluding it would drop the oldest part of a long history.
bool feed::ours(const std::optional<std::string>& owner) const {
    const auto& bound = store_.commander();
    return !owner || !bound || *bound == *owner;
}

// A file the store has in full is not opened at all. That is what turns the second launch from a
// long backfill into an instant one, and why phase::history listeners see a file once ever.
// Note the order: the store is asked before the header is read, so attribution costs nothing on
// a directory that has already been scanned.
//
// Returns true if the file was skipped, as already stored or as someone else's.
bool feed::replay_historical(const files::journal_file& file) {
    const std::string name = files::utf8(file.path.filename());
    const auto size = size_of(file.path);
    if (size && store_.ingested(name, *size)) return true;

    std::optional<std::string> owner;
    if (store_.enabled()) {
        if (auto o = files::owner_of(file.path)) owner = std::move(o->fid);
    }
    if (!ours(owner)) {
        store_.skip(name, size.value_or(0), owner);
        return true;
    }

    try {
        store_.begin_file(name, owner);
        files::read_lines(file.path, 0, [this](std::string_view line, std::uint64_t offset) { handle(line, offset); });
        store_.checkpoint(size.value_or(0), true);
    } catch (const std::exception& e) {
        // One unreadable file should not abandon the rest of the history.
        store_.abort();
        spdlog::warn("[{}] skipping unreadable journal {}: {}", id_, files::utf8(file.path), e.what());
    }
    return false;
}

// ── tail ────────────────────────────────────────────────────────────────────

void feed::open_file(const files::journal_file& file) {
    current_owner_.reset();
    if (store_.enabled()) {
        if (auto o = files::owner_of(file.path)) current_owner_ = std::move(o->fid);
    }
    current_recorded_ = ours(current_owner_);
    if (current_recorded_) {
        store_.begin_file(files::utf8(file.path.filename()), current_owner_);
    } else {
        spdlog::debug("[{}] tailing {} for state only; it belongs to {}", id_, files::utf8(file.path),
                      current_owner_.value_or("?"));
    }
    current_ = file;
    position_ = 0;
}

// Closes out the file being left: a batch to commit if it was ours, otherwise a note that it was
// someone else's, so the next launch skips it without reading its header.
void feed::close_file() {
    if (!current_) return;
    if (current_recorded_) {
        store_.checkpoint(position_, true);
    } else {
        // The real size, not the cursor: the cursor stops at the last complete line, and recording
        // that would leave the file looking unfinished and get its header read every launch.
        store_.skip(files::utf8(current_->path.filename()), size_of(current_->path).value_or(position_), current_owner_);
    }
}

void feed::pump() {
    if (!current_) return;
    position_ = files::read_lines(current_->path, position_,
                                  [this](std::string_view line, std::uint64_t offset) { handle(line, offset); });
}

// Reads new bytes, rolling over if the game has started a new file.
void feed::read_from(const fs::path& path) {
    const auto file = files::journal_file::of(path);
    if (!file) return;
    try {
        if (!current_) {
            open_file(*file);
        } else if (file->path != current_->path) {
            // An older file touched (a backup tool, a duplicate notification) must not roll the
            // state back to a stale session.
            if (!(*current_ < *file)) return;
            pump();        // drain the superseded file
            close_file();  // and close it out, however it was being treated
            open_file(*file);
        }
        pump();
        store_.checkpoint(position_, false);
    } catch (const std::exception& e) {
        store_.abort();
        spdlog::warn("[{}] failed reading {}: {}", id_, files::utf8(path), e.what());
    }
}

void feed::on_file_event(const fs::path& file) {
    // Before going live the scan will reach this file anyway, and finish_scan looks for a newer
    // one, so dropping the notification loses nothing.
    if (stopped_ || phase_ != phase::live) return;
    read_from(file);
}

// The watcher dropped notifications. The tail is offset-based, so re-reading from the newest file
// recovers everything appended in the meantime.
void feed::on_overflow() {
    if (stopped_ || phase_ != phase::live) return;
    try {
        if (const auto newest = files::newest(directory_)) read_from(newest->path);
    } catch (const fs::filesystem_error& e) {
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
    store_.record(*raw, line, offset);
    const game_event parsed = parser_.parse(*raw, phase_);

    // State first, so a listener reading state() sees the event it was just handed.
    if (phase_ == phase::live) publish_state();

    for (const auto& [_, sub] : *subs_->snapshot()) {
        if (!sub.phases.contains(phase_)) continue;
        if (!sub.events.empty() && !sub.events.contains(parsed.name())) continue;
        try {
            sub.fn(parsed, phase_);
        } catch (const std::exception& e) {
            spdlog::warn("[{}] listener threw on {}: {}", id_, parsed.name(), e.what());
        }
    }
}

void feed::publish_state() {
    auto snapshot = std::make_shared<const game_state>(parser_.state());
    published_.store(snapshot);
    for (const auto& [_, fn] : *state_subs_->snapshot()) {
        try {
            fn(snapshot);
        } catch (const std::exception& e) {
            spdlog::warn("[{}] state listener threw: {}", id_, e.what());
        }
    }
}

void feed::report(ingest_progress p) {
    for (const auto& [_, fn] : *progress_subs_->snapshot()) {
        try {
            fn(p);
        } catch (const std::exception& e) {
            spdlog::warn("[{}] progress listener threw: {}", id_, e.what());
        }
    }
}

void feed::shutdown() {
    if (stopped_) return;
    stopped_ = true;
    store_.close();  // commits whatever the tail had open
    subs_->clear();
    state_subs_->clear();
    progress_subs_->clear();
}

}  // namespace journal
