#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "journal/game_event.hpp"
#include "journal/history.hpp"
#include "journal/sql.hpp"

namespace journal {
    // The ingest side of one commander's history. Used only on the ingest thread, where several
    // feeds may share it: the same commander can play from more than one journal directory.
    //
    // Calls come in file order per feed: begin_file, record*, checkpoint. Each feed keeps its own
    // batch, and a checkpoint commits it, so a transaction never outlives the ingest task that
    // opened it and feeds sharing a store cannot see each other's half-written files.
    //
    // Events are kept in game-time order: each journal is placed by its Fileheader time, and lines
    // within it by offset. Journals usually arrive in that order, but not always (an older directory
    // added later, a backup restored, two directories scanned side by side). Events that land before
    // ones already stored mark the store for resequence(), which renumbers the ledger and replays
    // every projection from it. Until then projections skip them rather than fold them in wrongly.
    //
    // A default-constructed store is disabled: every call is a no-op and history() is disabled.
    class history_store {
    public:
        // One journal being written by one feed.
        struct batch {
            std::int64_t file = -1; // -1: none open
            std::uint64_t watermark = 0;
            bool late = false; // older than something already stored
            std::int64_t last_seq = 0;
        };

        history_store();

        // Opens or creates the store, migrating and catching projections up as needed, and finishing
        // a resequence an earlier run was stopped before. Throws history_error if it cannot.
        history_store(const std::filesystem::path &file, const history_config &config);

        history_store(history_store &&) noexcept;

        history_store &operator=(history_store &&) noexcept;

        ~history_store();

        [[nodiscard]] bool enabled() const { return db_.has_value(); }

        // Records who this store belongs to, for display. The latest name wins: commanders can rename.
        void identify(const std::optional<std::string> &fid, const std::string &name);

        // Whether a file is wholly stored, so the scan need not open it. This is what makes the
        // second launch fast.
        [[nodiscard]] bool ingested(const std::string &file, std::uint64_t size);

        // Opens a batch for a journal that began at `started` (ISO 8601 UTC). Lines below the
        // batch's watermark are already stored and are ignored by record(), so a partly ingested
        // file can simply be re-read from the start.
        batch begin_file(const std::string &file, const std::string &started);

        // Adds one line to the ledger and, unless it is late, to every interested projection.
        void record(batch &b, const game_event &event, std::string_view line, std::uint64_t offset);

        // Commits everything since the last checkpoint and records how far into the file it reached.
        void checkpoint(batch &b, std::uint64_t bytes, bool complete);

        // Abandons a batch, e.g. after an unreadable file.
        void abort(batch &b);

        // Whether late events are waiting for resequence().
        [[nodiscard]] bool unordered() const { return unordered_; }

        // Renumbers the ledger in game-time order and rebuilds every projection from it.
        void resequence();

        [[nodiscard]] journal::history history() const;

        // Closes the connection, abandoning anything uncommitted, and leaves the store disabled.
        void close();

    private:
        struct statements;

        void begin();

        void commit();

        std::optional<sql::database> db_;
        std::filesystem::path file_;
        std::vector<std::shared_ptr<projection> > projections_;
        std::set<std::string> recorded_;
        std::unordered_map<std::string, std::vector<projection *> > by_event_;
        std::vector<projection *> for_all_events_;
        std::unique_ptr<statements> stmts_;

        // The latest (started, name) of any journal holding events. A batch for one before it is late.
        std::pair<std::string, std::string> newest_;
        bool unordered_ = false;
    };
} // namespace journal
