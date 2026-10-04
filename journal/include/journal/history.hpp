#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "journal/game_event.hpp"
#include "journal/sql.hpp"

namespace journal {

class history_error : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// A derived view over the event ledger: the extension point for anything worth remembering.
//
// The store keeps two tiers. The `event` table holds journal lines verbatim and its shape never
// changes; everything else belongs to a projection and can be rebuilt from `event` at any time.
// Adding a fact to track is therefore one class: on the next launch it backfills itself from
// what is already stored, with no journal re-read.
//
// version() is the contract. When apply() would produce different rows for the same events,
// bump it: the store resets the projection and replays the whole ledger through it. Forgetting
// to bump it leaves rows computed by code that no longer exists.
//
// apply() receives the raw event exactly as the journal wrote it, never the parser's enriched
// copy, because a rebuild can only reproduce what was stored.
//
// One instance is shared by every commander's store and all of them run on the service's single
// ingest thread, so implementations must keep no state between calls; anything they need to
// remember goes in their tables.
class projection {
public:
    virtual ~projection() = default;

    // Stable identifier. Renaming forces a rebuild.
    [[nodiscard]] virtual std::string_view name() const = 0;

    // Bump whenever apply() would produce different rows for the same events.
    [[nodiscard]] virtual int version() const = 0;

    // Event names wanted, or empty for all of them. Narrower is cheaper for live ingest and rebuild.
    [[nodiscard]] virtual std::set<std::string> events() const { return {}; }

    // Creates tables and indexes. Must be IF NOT EXISTS-safe.
    virtual void schema(sql::database& db) = 0;

    // Discards everything this projection owns, ahead of a rebuild.
    virtual void reset(sql::database& db) = 0;

    // Folds one event in. `seq` is the ledger sequence number, monotonic in ingest order.
    virtual void apply(sql::database& db, std::int64_t seq, const game_event& event) = 0;
};

// The event names that reach the ledger. Journals are dominated by events no history wants
// (Music, ReceiveText, Scan, ...). Changing the set forces one full re-read of the journals, since
// events skipped before cannot be recovered any other way.
const std::set<std::string>& default_recorded_events();

// Turns persistence on for a journal_service: one SQLite file per feed, in `directory`.
struct history_config {
    std::filesystem::path directory;
    std::vector<std::shared_ptr<projection>> projections;  // applied in this order
    std::set<std::string> recorded = default_recorded_events();

    // The built-in projections over the default allowlist.
    static history_config standard(std::filesystem::path directory);
};

// Read access to one commander's accumulated history. Every query opens its own short-lived
// connection, so this is safe to use from any thread, concurrently with ingest.
class history {
public:
    history() = default;  // a feed opened without persistence
    explicit history(std::filesystem::path file) : file_(std::move(file)) {}

    [[nodiscard]] bool enabled() const { return file_.has_value(); }

    // Events in the ledger, and journal files fully ingested.
    [[nodiscard]] std::int64_t event_count() const;
    [[nodiscard]] std::int64_t file_count() const;

    // Runs a read. Projections ship their own readers built on this; see projections.hpp.
    // Throws history_error when persistence is off, sql::error when the query fails.
    template <class F>
    auto query(F&& read) const {
        sql::database db = open();
        return std::invoke(std::forward<F>(read), db);
    }

private:
    [[nodiscard]] sql::database open() const;

    std::optional<std::filesystem::path> file_;
};

}  // namespace journal
