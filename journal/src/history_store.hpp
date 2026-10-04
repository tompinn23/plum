#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "journal/game_event.hpp"
#include "journal/history.hpp"
#include "journal/sql.hpp"

namespace journal {

// The ingest side of one commander's history, as a feed sees it. Used only on the ingest thread.
//
// Calls come in file order: begin_file, record*, checkpoint. A checkpoint commits everything
// since the last one. The file being tailed is checkpointed repeatedly and only marked complete
// when the game moves on to a new one.
//
// A store holds exactly one commander, the one it is bound to. The feed attributes each file
// before ingesting it and calls skip() for anyone else's, so projections see one commander's
// events because that is all there is.
//
// A default-constructed store is disabled: every call is a no-op and history() is disabled.
class history_store {
public:
    history_store();

    // Opens or creates the store, migrating and catching projections up as needed.
    // Throws history_error if it cannot.
    history_store(const std::filesystem::path& file, const history_config& config);

    history_store(history_store&&) noexcept;
    history_store& operator=(history_store&&) noexcept;
    ~history_store();

    [[nodiscard]] bool enabled() const { return db_.has_value(); }

    // The FID of the commander this store holds, if bound.
    [[nodiscard]] const std::optional<std::string>& commander() const { return commander_fid_; }

    // Binds to a commander unless already bound. Permanent: a store that followed whoever played
    // most recently would discard its history every time an alt was launched.
    void bind(const std::string& fid, const std::string& name);

    // Whether a file is wholly accounted for (ingested, or skipped as someone else's), so the scan
    // need not open it. This is what makes the second launch fast.
    [[nodiscard]] bool ingested(const std::string& file, std::uint64_t size);

    // Records that a file belongs to another commander, so later launches skip it unread.
    void skip(const std::string& file, std::uint64_t size, const std::optional<std::string>& owner);

    // Opens a batch for a journal. Lines below the returned offset are already stored and are
    // ignored by record(), so a partly ingested file can simply be re-read from the start.
    std::uint64_t begin_file(const std::string& file, const std::optional<std::string>& owner);

    // Adds one line to the ledger and to every interested projection.
    void record(const game_event& event, std::string_view line, std::uint64_t offset);

    // Commits everything since the last checkpoint and records how far into the file it reached.
    void checkpoint(std::uint64_t bytes, bool complete);

    // Abandons the current batch, e.g. after an unreadable file.
    void abort();

    [[nodiscard]] journal::history history() const;

    // Commits anything open and closes the connection, leaving the store disabled.
    void close();

private:
    struct statements;

    std::optional<sql::database> db_;
    std::filesystem::path file_;
    std::vector<std::shared_ptr<projection>> projections_;
    std::set<std::string> recorded_;
    std::unordered_map<std::string, std::vector<projection*>> by_event_;
    std::vector<projection*> for_all_events_;
    std::unique_ptr<statements> stmts_;

    std::optional<std::string> commander_fid_;
    std::int64_t file_id_ = -1;  // -1: no batch open
    std::uint64_t watermark_ = 0;
    std::int64_t last_seq_ = 0;
};

}  // namespace journal
