#include "archive.hpp"

#include <cctype>
#include <format>

#include <spdlog/spdlog.h>

namespace journal {
    namespace fs = std::filesystem;

    namespace {
        sql::database open_catalog(const fs::path &directory) {
            const fs::path file = directory / "catalog.db";
            try {
                fs::create_directories(directory);
                sql::database db(file);
                db.exec("PRAGMA journal_mode = WAL; PRAGMA synchronous = NORMAL;");
                // Only journals that name a commander are kept: the rest are short main-menu sessions,
                // or the journal being written right now, whose commander may yet appear.
                db.exec(R"(
            CREATE TABLE IF NOT EXISTS journal (
              name      TEXT PRIMARY KEY,
              started   TEXT NOT NULL,
              fid       TEXT,
              commander TEXT
            );
            CREATE INDEX IF NOT EXISTS journal_commander ON journal(commander);
            CREATE TABLE IF NOT EXISTS directory (
              path  TEXT PRIMARY KEY,
              owner TEXT NOT NULL
            );
        )");
                return db;
            } catch (const std::exception &e) {
                throw history_error(std::format("cannot open history catalog {}: {}", files::utf8(file), e.what()));
            }
        }
    } // namespace

    archive::archive(history_config config)
        : config_(std::move(config)), catalog_(open_catalog(config_.directory)) {
        spdlog::info("history catalog in {}", files::utf8(config_.directory / "catalog.db"));
    }

    fs::path archive::store_file(const fs::path &directory, std::string_view key) {
        std::string name(key);
        for (char &c: name)
            if (!std::isalnum(static_cast<unsigned char>(c))) c = '_';
        return directory / (name + ".db");
    }

    // A journal from before FIDs carries only a name. If exactly one FID is known to have used that
    // name, it is the same commander and their history belongs together; otherwise it is kept apart
    // rather than guessed at.
    std::string archive::resolve(const std::optional<std::string> &fid, const std::optional<std::string> &name) {
        if (fid) return *fid;
        auto s = catalog_.prepare(
            "SELECT DISTINCT fid FROM journal WHERE commander = :name AND fid IS NOT NULL LIMIT 2");
        s.bind("name", *name);
        std::optional<std::string> only;
        int found = 0;
        while (s.step()) {
            only = s.text(0);
            ++found;
        }
        return found == 1 ? *only : "cmdr:" + *name;
    }

    void archive::remember(const files::journal_file &file, const std::string &started,
                           const std::optional<std::string> &fid, const std::optional<std::string> &name) {
        catalog_
                .prepare(
                    "INSERT INTO journal(name, started, fid, commander) VALUES (:name, :started, :fid, :commander) "
                    "ON CONFLICT(name) DO UPDATE SET fid = excluded.fid, commander = excluded.commander")
                .bind("name", files::utf8(file.path.filename()))
                .bind("started", started)
                .bind("fid", fid)
                .bind("commander", name)
                .run();
    }

    archive::attribution archive::attribute(const files::journal_file &file) {
        try {
            auto s = catalog_.prepare("SELECT started, fid, commander FROM journal WHERE name = :name");
            s.bind("name", files::utf8(file.path.filename()));
            if (s.step()) return {resolve(s.opt_text(1), s.opt_text(2)), s.text(0)};

            const auto head = files::read_head(file.path);
            const std::string started = head.started.value_or(file.stamp_text());
            if (!head.names_commander()) return {std::nullopt, started};
            spdlog::debug("cataloguing {}: {} {}", files::utf8(file.path.filename()), head.commander.value_or("?"),
                          head.fid.value_or("(no FID)"));
            remember(file, started, head.fid, head.commander);
            return {resolve(head.fid, head.commander), started};
        } catch (const sql::error &e) {
            spdlog::warn("cannot attribute {}: {}", files::utf8(file.path), e.what());
            return {std::nullopt, file.stamp_text()};
        }
    }

    std::optional<std::string> archive::claim(const files::journal_file &file, const std::string &started,
                                              const std::optional<std::string> &fid, const std::string &name) {
        const std::optional<std::string> commander = name.empty() ? std::nullopt : std::optional(name);
        if (!fid && !commander) return std::nullopt;
        try {
            remember(file, started, fid, commander);
        } catch (const sql::error &e) {
            spdlog::warn("cannot record the owner of {}: {}", files::utf8(file.path), e.what());
        }
        auto key = resolve(fid, commander);
        spdlog::debug("{} claimed by {}", files::utf8(file.path.filename()), key);
        if (const auto it = stores_.find(key); it != stores_.end()) it->second.identify(fid, name);
        return key;
    }

    std::optional<std::string> archive::directory_owner(const fs::path &directory) {
        try {
            auto s = catalog_.prepare("SELECT owner FROM directory WHERE path = :path");
            s.bind("path", files::utf8(directory));
            return s.step() ? std::optional(s.text(0)) : std::nullopt;
        } catch (const sql::error &e) {
            spdlog::warn("cannot read the owner of {}: {}", files::utf8(directory), e.what());
            return std::nullopt;
        }
    }

    void archive::set_directory_owner(const fs::path &directory, const std::string &key) {
        try {
            catalog_
                    .prepare("INSERT INTO directory(path, owner) VALUES (:path, :owner) "
                        "ON CONFLICT(path) DO UPDATE SET owner = excluded.owner")
                    .bind("path", files::utf8(directory))
                    .bind("owner", key)
                    .run();
        } catch (const sql::error &e) {
            spdlog::warn("cannot record the owner of {}: {}", files::utf8(directory), e.what());
        }
    }

    history_store *archive::store(const std::string &key) {
        if (const auto it = stores_.find(key); it != stores_.end()) return &it->second;
        if (failed_.contains(key)) return nullptr;
        try {
            const auto file = store_file(config_.directory, key);
            auto [it, _] = stores_.emplace(key, history_store(file, config_));
            spdlog::info("history for {} in {}", key, files::utf8(file));

            // Named after whatever the commander last called themselves; they can rename.
            if (key.starts_with("cmdr:")) {
                it->second.identify(std::nullopt, key.substr(5));
            } else {
                auto s = catalog_.prepare(
                    "SELECT commander FROM journal WHERE fid = :fid AND commander IS NOT NULL ORDER BY started DESC LIMIT 1");
                s.bind("fid", key);
                it->second.identify(key, s.step() ? s.text(0) : std::string());
            }
            return &it->second;
        } catch (const std::exception &e) {
            spdlog::error("history disabled for {}: {}", key, e.what());
            failed_.insert(key);
            return nullptr;
        }
    }

    void archive::settle() {
        for (auto &[key, store]: stores_) {
            if (!store.unordered()) continue;
            spdlog::info("resequencing history for {}", key);
            try {
                store.resequence();
            } catch (const history_error &e) {
                spdlog::error("{}", e.what());
            }
        }
    }

    void archive::close() {
        spdlog::debug("closing {} history stores", stores_.size());
        stores_.clear();
    }
} // namespace journal
