#pragma once

#include <filesystem>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>

#include "history_store.hpp"
#include "journal/history.hpp"
#include "journal/sql.hpp"
#include "journal_files.hpp"

namespace journal {
    // Every commander's history, as the service's feeds share it. Ingest thread only.
    //
    // History is kept per commander, not per directory: a directory can hold several commanders'
    // journals, and one commander can play from several directories. Each journal goes to the store
    // of whoever wrote it, keyed by FID, or by name for journals from before FIDs existed.
    //
    // The catalog (catalog.db, beside the stores) remembers each journal's owner and start time, so
    // a launch reads the head of a journal once ever rather than every time, and remembers which
    // commander each directory last saw, so its history is still found once the journals are gone.
    class archive {
    public:
        // Throws history_error if the catalog cannot be opened.
        explicit archive(history_config config);

        archive(const archive &) = delete;

        archive &operator=(const archive &) = delete;

        struct attribution {
            std::optional<std::string> key; // nullopt: names no commander, so holds no history
            std::string started; // ISO 8601 UTC
        };

        // Who a journal belongs to and when it began.
        attribution attribute(const files::journal_file &file);

        // Attributes a journal from a Commander or LoadGame line read while tailing it: the game
        // creates the file at the main menu, before anyone has logged in. nullopt if the line names
        // no one after all.
        std::optional<std::string> claim(const files::journal_file &file, const std::string &started,
                                         const std::optional<std::string> &fid, const std::string &name);

        [[nodiscard]] std::optional<std::string> directory_owner(const std::filesystem::path &directory);

        void set_directory_owner(const std::filesystem::path &directory, const std::string &key);

        // The commander's store, opened on first use. nullptr if it cannot be opened; that costs the
        // commander's history, not the live dashboard.
        history_store *store(const std::string &key);

        // Resequences every store that has taken journals out of order.
        void settle();

        // Closes every store.
        void close();

        [[nodiscard]] std::filesystem::path file_for(std::string_view key) const {
            return store_file(config_.directory, key);
        }

        // Where a commander's store lives. Keys are FIDs, or "cmdr:<name>" for journals that predate
        // them; anything but letters and digits becomes '_' to keep it a plain file name.
        static std::filesystem::path store_file(const std::filesystem::path &directory, std::string_view key);

    private:
        std::string resolve(const std::optional<std::string> &fid, const std::optional<std::string> &name);

        void remember(const files::journal_file &file, const std::string &started,
                      const std::optional<std::string> &fid,
                      const std::optional<std::string> &name);

        history_config config_;
        sql::database catalog_;
        std::map<std::string, history_store> stores_;
        std::set<std::string> failed_; // keys whose store would not open, so it is not retried per file
    };
} // namespace journal
