#include "history_store.hpp"

#include <algorithm>
#include <format>
#include <string>
#include <utility>

#include <spdlog/spdlog.h>

#include "journal/projections.hpp"
#include "journal_files.hpp"

namespace journal {
    namespace {
        // Bump to force every store to be rebuilt from the journals.
        constexpr int schema_version = 2;

        constexpr std::string_view key_schema = "schema_version";
        constexpr std::string_view key_events = "recorded_events";
        constexpr std::string_view key_commander_fid = "commander_fid";
        constexpr std::string_view key_commander_name = "commander_name";
        // Present while late events wait for a resequence, so one interrupted by a crash still happens.
        constexpr std::string_view key_unordered = "unordered";

        // The ledger. `seq` is game-time order once resequenced; see history_store.
        // "offset" is reserved in SQL, hence byte_offset.
        constexpr std::string_view event_table = R"(
    CREATE TABLE IF NOT EXISTS {} (
      seq         INTEGER PRIMARY KEY,
      ts          TEXT    NOT NULL,
      name        TEXT    NOT NULL,
      payload     TEXT    NOT NULL,
      file        INTEGER NOT NULL,
      byte_offset INTEGER NOT NULL
    );)";
        constexpr std::string_view event_indexes = R"(
    CREATE INDEX IF NOT EXISTS event_name_seq ON event(name, seq);
    CREATE INDEX IF NOT EXISTS event_ts ON event(ts);)";

        std::optional<std::string> meta(sql::database &db, std::string_view key) {
            auto s = db.prepare("SELECT value FROM meta WHERE key = :key");
            s.bind("key", key);
            return s.step() ? std::optional(s.text(0)) : std::nullopt;
        }

        void set_meta(sql::database &db, std::string_view key, std::string_view value) {
            db.prepare("INSERT INTO meta(key, value) VALUES (:key, :value) "
                        "ON CONFLICT(key) DO UPDATE SET value = excluded.value")
                    .bind("key", key)
                    .bind("value", value)
                    .run();
        }

        // FNV-1a over the sorted names: order-independent, so reshuffling the allowlist is not a change.
        std::string fingerprint(const std::set<std::string> &events) {
            std::uint64_t hash = 14695981039346656037ull;
            for (const auto &name: events) {
                // std::set iterates sorted
                for (const unsigned char c: name) hash = (hash ^ c) * 1099511628211ull;
                hash = (hash ^ 0xff) * 1099511628211ull; // separator, so {"ab","c"} != {"a","bc"}
            }
            return std::to_string(hash);
        }

        // Creates the core tables and decides whether what is on disk can be kept.
        void install(sql::database &db, const std::set<std::string> &recorded) {
            // meta first and alone: the decision about everything else is read out of it.
            db.exec("CREATE TABLE IF NOT EXISTS meta (key TEXT PRIMARY KEY, value TEXT NOT NULL)");

            const auto schema = meta(db, key_schema);
            const auto events = meta(db, key_events);
            const std::string current_schema = std::to_string(schema_version);
            const std::string current_events = fingerprint(recorded);

            const bool schema_changed = schema && *schema != current_schema;
            const bool allowlist_changed = events && *events != current_events;

            if (schema_changed) {
                // Dropped rather than emptied: CREATE TABLE IF NOT EXISTS would leave an old-shaped table
                // in place.
                spdlog::info("history store reset: schema version changed");
                db.exec(
                    "DROP TABLE IF EXISTS event; DROP TABLE IF EXISTS journal_file; DROP TABLE IF EXISTS projection;");
                db.prepare("DELETE FROM meta WHERE key = :key").bind("key", key_unordered).run();
            }

            // started is the journal's Fileheader time: with name, it orders journals for the ledger.
            db.exec(R"(
        CREATE TABLE IF NOT EXISTS journal_file (
          id       INTEGER PRIMARY KEY,
          name     TEXT    NOT NULL UNIQUE,
          started  TEXT    NOT NULL,
          bytes    INTEGER NOT NULL DEFAULT 0,
          complete INTEGER NOT NULL DEFAULT 0
        );
        CREATE TABLE IF NOT EXISTS projection (
          name        TEXT PRIMARY KEY,
          version     INTEGER NOT NULL,
          through_seq INTEGER NOT NULL DEFAULT 0
        );
    )");
            db.exec(std::format(event_table, "event"));
            db.exec(event_indexes);

            if (allowlist_changed && !schema_changed) {
                // Widening the allowlist means events were skipped that are now wanted, and only a re-read
                // recovers them; narrowing it leaves rows a projection no longer expects. Either way, start
                // over: one slow launch, no half-migrated state.
                spdlog::info("history store reset: recorded event set changed");
                db.exec("DELETE FROM event; UPDATE journal_file SET bytes = 0, complete = 0; DELETE FROM projection;");
            }

            set_meta(db, key_schema, current_schema);
            set_meta(db, key_events, current_events);
        }

        // Brings a projection's schema and registry row into line with the code and replays whatever
        // it has not seen. Reads the event table, never the journals, so adding or changing a projection
        // costs a table scan rather than a re-parse.
        void catch_up(sql::database &db, projection &p) {
            std::optional<int> stored_version;
            std::int64_t through = 0;
            {
                auto s = db.prepare("SELECT version, through_seq FROM projection WHERE name = :name");
                s.bind("name", p.name());
                if (s.step()) {
                    stored_version = static_cast<int>(s.integer(0));
                    through = s.integer(1);
                }
            }

            if (stored_version != p.version()) {
                if (stored_version)
                    spdlog::info("rebuilding projection {} (v{} -> v{})", p.name(), *stored_version, p.version());
                p.reset(db);
                through = 0;
            }
            p.schema(db);

            const std::set<std::string> wanted = p.events();
            std::string sql = "SELECT seq, payload FROM event WHERE seq > :from";
            if (!wanted.empty()) {
                sql += " AND name IN (";
                for (std::size_t i = 0; i < wanted.size(); ++i) sql += std::format("{}:n{}", i ? ", " : "", i);
                sql += ")";
            }
            sql += " ORDER BY seq";

            auto s = db.prepare(sql);
            s.bind("from", through);
            std::size_t i = 0;
            for (const auto &name: wanted) s.bind(std::format("n{}", i++), name);

            std::int64_t applied = 0;
            std::int64_t last = through;
            while (s.step()) {
                const std::int64_t seq = s.integer(0);
                if (const auto event = game_event::parse(s.text(1))) {
                    p.apply(db, seq, *event);
                } else {
                    // Stored rows came from lines that parsed once, so the row is damaged, not the journal.
                    spdlog::warn("skipping unreadable ledger row {}", seq);
                }
                last = seq;
                if (++applied % 250'000 == 0) spdlog::info("projection {}: {} events replayed", p.name(), applied);
            }
            if (applied > 0) spdlog::info("projection {} caught up over {} events", p.name(), applied);

            // A projection that wants only some events may have seen none of the newest rows; it is still
            // through all of them.
            auto max = db.prepare("SELECT COALESCE(MAX(seq), 0) FROM event");
            if (max.step()) last = std::max(last, max.integer(0));

            db.prepare("INSERT INTO projection(name, version, through_seq) VALUES (:name, :version, :through) "
                        "ON CONFLICT(name) DO UPDATE SET version = excluded.version, through_seq = excluded.through_seq")
                    .bind("name", p.name())
                    .bind("version", p.version())
                    .bind("through", last)
                    .run();
        }
    } // namespace

    // Prepared once, used for every line.
    struct history_store::statements {
        sql::statement insert;
    };

    history_store::history_store(const std::filesystem::path &file, const history_config &config)
        : file_(file), projections_(config.projections), recorded_(config.recorded) {
        for (const auto &p: projections_) {
            const auto wanted = p->events();
            if (wanted.empty()) {
                for_all_events_.push_back(p.get());
                continue;
            }
            for (const auto &name: wanted) {
                if (!recorded_.contains(name))
                    spdlog::warn("projection {} wants {}, which is not recorded; it will never see one", p->name(),
                                 name);
                by_event_[name].push_back(p.get());
            }
        }

        try {
            db_.emplace(file);
            // WAL lets readers run alongside ingest. The ledger is rebuildable from the journals, so
            // trading power-loss durability for write throughput is the right way round.
            db_->exec("PRAGMA journal_mode = WAL; PRAGMA synchronous = NORMAL; PRAGMA temp_store = MEMORY;");

            db_->exec("BEGIN");
            install(*db_, recorded_);
            unordered_ = meta(*db_, key_unordered).has_value();
            if (!unordered_) {
                for (const auto &p: projections_) catch_up(*db_, *p);
            }
            {
                auto s = db_->prepare(
                    "SELECT started, name FROM journal_file WHERE id IN (SELECT DISTINCT file FROM event) "
                    "ORDER BY started DESC, name DESC LIMIT 1");
                if (s.step()) newest_ = {s.text(0), s.text(1)};
            }
            db_->exec("COMMIT");

            stmts_ = std::make_unique<statements>(statements{
                db_->prepare("INSERT INTO event(ts, name, payload, file, byte_offset) "
                    "VALUES (:ts, :name, :payload, :file, :offset)"),
            });
        } catch (const sql::error &e) {
            throw history_error(std::format("cannot open history store {}: {}", files::utf8(file), e.what()));
        }

        if (unordered_) resequence();
    }

    history_store::history_store() = default;

    // Hand-written so the source is left disabled: a defaulted move would leave its optional
    // engaged around a moved-from connection, and its destructor would then try to use it.
    history_store::history_store(history_store &&other) noexcept {
        *this = std::move(other);
    }

    history_store &history_store::operator=(history_store &&other) noexcept {
        if (this == &other) return *this;
        close();
        db_ = std::exchange(other.db_, std::nullopt);
        file_ = std::move(other.file_);
        projections_ = std::move(other.projections_);
        recorded_ = std::move(other.recorded_);
        by_event_ = std::move(other.by_event_);
        for_all_events_ = std::move(other.for_all_events_);
        stmts_ = std::move(other.stmts_);
        newest_ = std::move(other.newest_);
        unordered_ = std::exchange(other.unordered_, false);
        return *this;
    }

    history_store::~history_store() {
        close();
    }

    void history_store::close() {
        if (!db_) return;
        if (db_->in_transaction()) {
            try {
                db_->exec("ROLLBACK");
            } catch (const sql::error &e) {
                spdlog::warn("could not close history store cleanly: {}", e.what());
            }
        }
        stmts_.reset(); // statements must be finalised before their connection closes
        db_.reset();
    }

    void history_store::begin() {
        if (!db_->in_transaction()) db_->exec("BEGIN");
    }

    void history_store::commit() {
        if (db_->in_transaction()) db_->exec("COMMIT");
    }

    void history_store::identify(const std::optional<std::string> &fid, const std::string &name) {
        if (!db_) return;
        try {
            if (fid) set_meta(*db_, key_commander_fid, *fid);
            if (!name.empty()) set_meta(*db_, key_commander_name, name);
        } catch (const sql::error &e) {
            spdlog::warn("cannot record commander {}: {}", name, e.what());
        }
    }

    bool history_store::ingested(const std::string &file, std::uint64_t size) {
        if (!db_) return false;
        try {
            auto s = db_->prepare("SELECT bytes, complete FROM journal_file WHERE name = :name");
            s.bind("name", file);
            return s.step() && s.integer(1) == 1 && static_cast<std::uint64_t>(s.integer(0)) >= size;
        } catch (const sql::error &e) {
            spdlog::warn("cannot check watermark for {}; will re-read it: {}", file, e.what());
            return false;
        }
    }

    history_store::batch history_store::begin_file(const std::string &file, const std::string &started) {
        if (!db_) return {};
        try {
            begin();
            db_->prepare("INSERT INTO journal_file(name, started) VALUES (:name, :started) "
                        "ON CONFLICT(name) DO NOTHING")
                    .bind("name", file)
                    .bind("started", started)
                    .run();
            auto s = db_->prepare("SELECT id, bytes, started FROM journal_file WHERE name = :name");
            s.bind("name", file);
            if (!s.step()) throw history_error("journal_file row vanished for " + file);

            batch b;
            b.file = s.integer(0);
            b.watermark = static_cast<std::uint64_t>(s.integer(1));
            // The same journal found in a second directory keeps the time it was first stored under.
            const std::pair<std::string, std::string> key{s.text(2), file};
            b.late = key < newest_;
            newest_ = std::max(newest_, key);
            return b;
        } catch (const sql::error &e) {
            throw history_error(std::format("cannot start ingesting {}: {}", file, e.what()));
        }
    }

    void history_store::record(batch &b, const game_event &event, std::string_view line, std::uint64_t offset) {
        // Below the watermark means a previous run already stored this line. The newest journal is
        // re-read from byte zero on every launch so the parser can rebuild state, so this is the
        // normal path, not an edge case.
        if (!db_ || b.file < 0 || offset < b.watermark) return;
        if (!recorded_.contains(event.name())) return;

        try {
            begin();
            auto &insert = stmts_->insert;
            insert.reset();
            insert.bind("ts", event.time() ? format_timestamp(*event.time()) : std::string())
                    .bind("name", event.name())
                    .bind("payload", line)
                    .bind("file", b.file)
                    .bind("offset", static_cast<std::int64_t>(offset))
                    .run();
            const std::int64_t seq = db_->last_insert_rowid();
            b.last_seq = seq;

            if (b.late || unordered_) {
                // Folding it in now would put it after events that happened later. The resequence
                // replays everything in order instead.
                if (!unordered_) {
                    set_meta(*db_, key_unordered, "1");
                    unordered_ = true;
                }
                return;
            }
            for (auto *p: for_all_events_) p->apply(*db_, seq, event);
            if (const auto it = by_event_.find(event.name()); it != by_event_.end()) {
                for (auto *p: it->second) p->apply(*db_, seq, event);
            }
        } catch (const sql::error &e) {
            throw history_error(std::format("cannot record {}: {}", event.name(), e.what()));
        }
    }

    void history_store::checkpoint(batch &b, std::uint64_t bytes, bool complete) {
        if (!db_ || b.file < 0) return;
        try {
            begin();
            b.watermark = std::max(bytes, b.watermark);
            db_->prepare("UPDATE journal_file SET bytes = MAX(bytes, :bytes), complete = MAX(complete, :complete) "
                        "WHERE id = :id")
                    .bind("bytes", static_cast<std::int64_t>(b.watermark))
                    .bind("complete", complete)
                    .bind("id", b.file)
                    .run();
            // Projection writes share this transaction with the ledger insert, so the watermark can
            // advance here rather than per event: either both land or neither does. While unordered,
            // projections are waiting for a rebuild and their watermark means nothing.
            if (b.last_seq > 0 && !unordered_) {
                db_->prepare("UPDATE projection SET through_seq = :seq WHERE through_seq < :seq")
                        .bind("seq", b.last_seq)
                        .run();
            }
            commit();
            if (complete) b.file = -1;
        } catch (const sql::error &e) {
            throw history_error(std::format("cannot checkpoint history: {}", e.what()));
        }
    }

    void history_store::abort(batch &b) {
        if (!db_ || b.file < 0) return;
        b.file = -1;
        if (!db_->in_transaction()) return;
        try {
            db_->exec("ROLLBACK");
        } catch (const sql::error &e) {
            spdlog::warn("could not roll back history batch: {}", e.what());
        }
    }

    // Copies the ledger into a fresh table in game-time order, so seq is renumbered to match, then
    // rebuilds every projection from it. Costs a pass over the ledger; it happens when journals turn
    // up out of order, not on an ordinary launch.
    void history_store::resequence() {
        if (!db_ || !unordered_) return;
        const auto prepare = [this] {
            stmts_ = std::make_unique<statements>(statements{
                db_->prepare("INSERT INTO event(ts, name, payload, file, byte_offset) "
                    "VALUES (:ts, :name, :payload, :file, :offset)"),
            });
        };
        stmts_.reset(); // nothing may hold the table being replaced
        try {
            commit();
            db_->exec("BEGIN");
            db_->exec("DROP TABLE IF EXISTS event_ordered");
            db_->exec(std::format(event_table, "event_ordered"));
            db_->exec(R"(
            INSERT INTO event_ordered(ts, name, payload, file, byte_offset)
            SELECT e.ts, e.name, e.payload, e.file, e.byte_offset
              FROM event e JOIN journal_file f ON f.id = e.file
             ORDER BY f.started, f.name, e.byte_offset;
            DROP TABLE event;
            ALTER TABLE event_ordered RENAME TO event;
            DELETE FROM projection;
        )");
            db_->exec(event_indexes);
            for (const auto &p: projections_) catch_up(*db_, *p);
            db_->prepare("DELETE FROM meta WHERE key = :key").bind("key", key_unordered).run();
            db_->exec("COMMIT");
            unordered_ = false;
            prepare();
            spdlog::info("history store {} resequenced", files::utf8(file_));
        } catch (const sql::error &e) {
            try {
                if (db_->in_transaction()) db_->exec("ROLLBACK");
                prepare();
            } catch (const sql::error &) {
            }
            // Left marked, so the next open tries again.
            throw history_error(std::format("cannot resequence {}: {}", files::utf8(file_), e.what()));
        }
    }

    journal::history history_store::history() const {
        return db_ ? journal::history(file_) : journal::history();
    }

    // ── history (read side) ─────────────────────────────────────────────────────

    sql::database history::open() const {
        if (!file_) throw history_error("this feed was opened without a history_config");
        return sql::database(*file_);
    }

    std::int64_t history::event_count() const {
        return query([](sql::database &db) {
            auto s = db.prepare("SELECT COUNT(*) FROM event");
            return s.step() ? s.integer(0) : 0;
        });
    }

    std::int64_t history::file_count() const {
        return query([](sql::database &db) {
            auto s = db.prepare("SELECT COUNT(*) FROM journal_file WHERE complete = 1");
            return s.step() ? s.integer(0) : 0;
        });
    }

    // ── configuration ───────────────────────────────────────────────────────────

    const std::set<std::string> &default_recorded_events() {
        static const std::set<std::string> events = {
            // Session
            "Fileheader", "Commander", "LoadGame", "NewCommander", "Shutdown", "Died", "Resurrect",
            // Location
            "Location", "FSDJump", "CarrierJump", "Docked", "Undocked", "ApproachBody", "LeaveBody",
            "ApproachSettlement", "SupercruiseEntry", "SupercruiseExit", "Liftoff", "Touchdown", "Embark",
            "Disembark", "DropshipDeploy", "BookTaxi", "BookDropship", "CancelTaxi", "CancelDropship",
            "NavRoute", "NavRouteClear",
            // Trade and cargo
            "MarketBuy", "MarketSell", "BuyDrones", "SellDrones", "CollectCargo", "EjectCargo",
            "MiningRefined", "CargoTransfer", "Cargo", "Market", "SearchAndRescue",
            // Money
            "RedeemVoucher", "PayBounties", "PayFines", "PayLegacyFines", "SellExplorationData",
            "MultiSellExplorationData", "SellOrganicData", "CommunityGoalReward", "PowerplaySalary",
            "BuyTradeData", "BuyExplorationData", "Bounty", "FactionKillBond", "CapShipBond",
            // Missions
            "MissionAccepted", "MissionCompleted", "MissionAbandoned", "MissionFailed", "MissionRedirected",
            // Ships and modules
            "Loadout", "ShipyardBuy", "ShipyardSell", "ShipyardSwap", "ShipyardTransfer", "ShipyardNew",
            "SellShipOnRebuy", "SetUserShipName", "ModuleBuy", "ModuleSell", "ModuleSellRemote",
            "ModuleStore", "ModuleRetrieve", "ModuleSwap", "MassModuleStore", "StoredModules",
            "StoredShips", "FetchRemoteModule", "Repair", "RepairAll", "RefuelAll", "RefuelPartial",
            "RestockVehicle", "BuyAmmo", "Outfitting", "Shipyard", "ModuleInfo",
            // Engineering and materials
            "EngineerCraft", "EngineerProgress", "EngineerContribution", "TechnologyBroker", "Synthesis",
            "MaterialTrade", "Materials", "MaterialCollected", "MaterialDiscarded", "ScientificResearch",
            // Exploration
            "FSSDiscoveryScan", "FSSAllBodiesFound", "SAAScanComplete", "CodexEntry", "Screenshot",
            // Standing
            "Rank", "Progress", "Promotion", "Reputation", "Statistics", "Powerplay", "PowerplayJoin",
            "PowerplayLeave", "PowerplayDefect", "PowerplayMerits", "PowerplayVoucher",
            // Fleet carrier
            "CarrierStats", "CarrierJumpRequest", "CarrierJumpCancelled", "CarrierLocation", "CarrierBuy",
            "CarrierDepositFuel", "CarrierBankTransfer", "CarrierFinance", "CarrierTradeOrder",
            "CarrierDockingPermission", "CarrierNameChange", "CarrierCrewServices", "CarrierModulePack",
            "CarrierShipPack", "FCMaterials",
            // Combat encounters
            "Interdicted", "Interdiction", "EscapeInterdiction",
            // Odyssey
            "BuyMicroResources", "SellMicroResources", "TradeMicroResources", "BackpackChange",
            "ShipLocker", "Backpack", "BuySuit", "BuyWeapon", "SuitLoadout", "SwitchSuitLoadout",
            // Crew
            "JoinACrew", "QuitACrew", "ChangeCrewRole", "CrewHire", "CrewFire", "CrewAssign",
        };
        return events;
    }

    history_config history_config::standard(std::filesystem::path directory) {
        history_config config;
        config.directory = std::move(directory);
        config.projections = {
            std::make_shared<ledger_projection>(),
            std::make_shared<visited_system_projection>(),
            std::make_shared<mission_log_projection>(),
            std::make_shared<carrier_projection>(),
            std::make_shared<massacre_projection>()
        };
        return config;
    }
} // namespace journal
