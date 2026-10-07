#include "journal/projections.hpp"

#include <charconv>

namespace journal {
    namespace {
        using status = mission_log_projection::status;

        const char *code(status s) {
            switch (s) {
                case status::accepted: return "accepted";
                case status::completed: return "completed";
                case status::failed: return "failed";
                case status::abandoned: return "abandoned";
            }
            return "accepted";
        }

        status status_of(std::string_view s) {
            if (s == "completed") return status::completed;
            if (s == "failed") return status::failed;
            if (s == "abandoned") return status::abandoned;
            return status::accepted;
        }

        // MissionAccepted says nothing about where you were standing when you took it. The ledger does:
        // the last arrival before this sequence number. All four events carry StarSystem, but only the
        // ones that leave you somewhere dockable carry StationName, so taking both from whichever is most
        // recent clears the station on a jump rather than attaching one seen light years away.
        constexpr std::string_view origin = R"(
    WITH origin AS (
        SELECT payload FROM event
        WHERE seq < :seq AND name IN ('Docked', 'Location', 'FSDJump', 'CarrierJump')
        ORDER BY seq DESC LIMIT 1)
)";

        // Accepting sets almost every column. Each write sets only the columns its event owns and
        // COALESCEs the rest against what is stored, which is also what makes this correct for missions
        // accepted before recorded history began.
        constexpr std::string_view accept_sql = R"(
    INSERT INTO mission(
        id, seq, name, title, faction, status, accepted_at, expires_at,
        reward, target, target_type, commodity, quantity,
        origin_system, origin_station,
        destination_system, destination_station, destination_settlement)
    VALUES (
        :id, :seq, :name, :title, :faction, 'accepted', :accepted_at, :expires_at,
        :reward, :target, :target_type, :commodity, :quantity,
        (SELECT json_extract(payload, '$.StarSystem') FROM origin),
        (SELECT json_extract(payload, '$.StationName') FROM origin),
        :destination_system, :destination_station, :destination_settlement)
    ON CONFLICT(id) DO UPDATE SET
      seq                    = excluded.seq,
      name                   = COALESCE(excluded.name, mission.name),
      title                  = COALESCE(excluded.title, mission.title),
      faction                = COALESCE(excluded.faction, mission.faction),
      accepted_at            = COALESCE(excluded.accepted_at, mission.accepted_at),
      expires_at             = COALESCE(excluded.expires_at, mission.expires_at),
      reward                 = COALESCE(excluded.reward, mission.reward),
      target                 = COALESCE(excluded.target, mission.target),
      target_type            = COALESCE(excluded.target_type, mission.target_type),
      commodity              = COALESCE(excluded.commodity, mission.commodity),
      quantity               = COALESCE(excluded.quantity, mission.quantity),
      origin_system          = COALESCE(excluded.origin_system, mission.origin_system),
      origin_station         = COALESCE(excluded.origin_station, mission.origin_station),
      destination_system     = COALESCE(excluded.destination_system, mission.destination_system),
      destination_station    = COALESCE(excluded.destination_station, mission.destination_station),
      destination_settlement = COALESCE(excluded.destination_settlement, mission.destination_settlement)
)";

        // Completion, failure and abandonment carry the outcome and little else. reward is overwritten:
        // on completion it is what was paid, which replaces what was promised.
        constexpr std::string_view outcome_sql = R"(
    INSERT INTO mission(id, seq, name, title, faction, status, ended_at, reward, fine, donation)
    VALUES (:id, :seq, :name, :title, :faction, :status, :ended_at, :reward, :fine, :donation)
    ON CONFLICT(id) DO UPDATE SET
      seq      = excluded.seq,
      status   = excluded.status,
      ended_at = excluded.ended_at,
      name     = COALESCE(excluded.name, mission.name),
      title    = COALESCE(excluded.title, mission.title),
      faction  = COALESCE(excluded.faction, mission.faction),
      reward   = COALESCE(excluded.reward, mission.reward),
      fine     = COALESCE(excluded.fine, mission.fine),
      donation = COALESCE(excluded.donation, mission.donation)
)";

        // A redirect moves the destination and nothing else; it never touches status, so a stray
        // redirect cannot resurrect a mission that has ended.
        constexpr std::string_view redirect_sql = R"(
    INSERT INTO mission(id, seq, name, title, status, destination_system, destination_station)
    VALUES (:id, :seq, :name, :title, 'accepted', :destination_system, :destination_station)
    ON CONFLICT(id) DO UPDATE SET
      seq                 = excluded.seq,
      title               = COALESCE(excluded.title, mission.title),
      destination_system  = COALESCE(excluded.destination_system, mission.destination_system),
      destination_station = COALESCE(excluded.destination_station, mission.destination_station)
)";

        // Donations moved from a string Donation to a numeric Donated partway through the game's life.
        std::optional<std::int64_t> donation(const game_event &e) {
            if (auto donated = e.opt_long("Donated")) return donated;
            const auto raw = e.opt_str("Donation");
            if (!raw) return std::nullopt;
            std::int64_t value = 0;
            const auto [_, ec] = std::from_chars(raw->data(), raw->data() + raw->size(), value);
            return ec == std::errc{} ? std::optional(value) : std::nullopt;
        }

        // How many of the thing: cargo, kills or passengers, whichever kind of mission this is.
        std::optional<std::int64_t> quantity(const game_event &e) {
            if (auto n = e.opt_long("Count")) return n;
            if (auto n = e.opt_long("KillCount")) return n;
            return e.opt_long("PassengerCount");
        }

        std::optional<timestamp> expiry(const game_event &e) {
            const auto raw = e.opt_str("Expiry");
            return raw ? parse_timestamp(*raw) : std::nullopt;
        }

        constexpr std::string_view columns = R"(
    SELECT id, name, title, faction, status,
           accepted_at, ended_at, expires_at,
           reward, fine, donation,
           target, target_type, commodity, quantity,
           origin_system, origin_station,
           destination_system, destination_station, destination_settlement
    FROM mission)";

        std::vector<mission_log_projection::mission> read(sql::statement &s) {
            std::vector<mission_log_projection::mission> out;
            while (s.step()) {
                out.push_back({
                    .id = s.integer(0),
                    .name = s.opt_text(1),
                    .title = s.opt_text(2),
                    .faction = s.opt_text(3),
                    .state = status_of(s.text(4)),
                    .accepted_at = s.opt_time(5),
                    .ended_at = s.opt_time(6),
                    .expires_at = s.opt_time(7),
                    .reward = s.opt_integer(8),
                    .fine = s.opt_integer(9),
                    .donation = s.opt_integer(10),
                    .target = s.opt_text(11),
                    .target_type = s.opt_text(12),
                    .commodity = s.opt_text(13),
                    .quantity = s.opt_integer(14),
                    .origin_system = s.opt_text(15),
                    .origin_station = s.opt_text(16),
                    .destination_system = s.opt_text(17),
                    .destination_station = s.opt_text(18),
                    .destination_settlement = s.opt_text(19),
                });
            }
            return out;
        }
    } // namespace

    std::set<std::string> mission_log_projection::events() const {
        // The arrivals the origin subquery reads are deliberately not listed: they are looked up from
        // the ledger when a mission is taken, rather than fed through here on every jump.
        return {"MissionAccepted", "MissionCompleted", "MissionFailed", "MissionAbandoned", "MissionRedirected"};
    }

    void mission_log_projection::schema(sql::database &db) {
        db.exec(R"(
        CREATE TABLE IF NOT EXISTS mission (
          id                     INTEGER PRIMARY KEY,
          seq                    INTEGER NOT NULL,
          name                   TEXT,
          title                  TEXT,
          faction                TEXT,
          status                 TEXT    NOT NULL,
          accepted_at            TEXT,
          ended_at               TEXT,
          expires_at             TEXT,
          reward                 INTEGER,
          fine                   INTEGER,
          donation               INTEGER,
          target                 TEXT,
          target_type            TEXT,
          commodity              TEXT,
          quantity               INTEGER,
          origin_system          TEXT,
          origin_station         TEXT,
          destination_system     TEXT,
          destination_station    TEXT,
          destination_settlement TEXT
        );
        CREATE INDEX IF NOT EXISTS mission_status ON mission(status, expires_at);
        CREATE INDEX IF NOT EXISTS mission_faction ON mission(faction);
        CREATE INDEX IF NOT EXISTS mission_origin ON mission(origin_system);
    )");
    }

    void mission_log_projection::reset(sql::database &db) {
        db.exec("DROP TABLE IF EXISTS mission");
    }

    void mission_log_projection::apply(sql::database &db, std::int64_t seq, const game_event &e) {
        const auto id = e.opt_long("MissionID");
        if (!id) return; // nothing to key on, so the row could never be joined to the rest of its life

        const std::string &name = e.name();
        if (name == "MissionAccepted") {
            db.prepare(std::string(origin) + std::string(accept_sql))
                    .bind("id", *id)
                    .bind("seq", seq)
                    .bind("name", e.opt_str("Name"))
                    .bind("title", e.opt_str("LocalisedName"))
                    .bind("faction", e.opt_str("Faction"))
                    .bind("accepted_at", e.time())
                    .bind("expires_at", expiry(e))
                    .bind("reward", e.opt_long("Reward"))
                    .bind("target", e.opt_str("Target"))
                    .bind("target_type", e.opt_str("TargetType"))
                    .bind("commodity", e.opt_str("Commodity"))
                    .bind("quantity", quantity(e))
                    .bind("destination_system", e.opt_str("DestinationSystem"))
                    .bind("destination_station", e.opt_str("DestinationStation"))
                    .bind("destination_settlement", e.opt_str("DestinationSettlement"))
                    .run();
        } else if (name == "MissionCompleted" || name == "MissionFailed" || name == "MissionAbandoned") {
            const status s = name == "MissionCompleted"
                                 ? status::completed
                                 : name == "MissionFailed"
                                       ? status::failed
                                       : status::abandoned;
            db.prepare(outcome_sql)
                    .bind("id", *id)
                    .bind("seq", seq)
                    .bind("name", e.opt_str("Name"))
                    .bind("title", e.opt_str("LocalisedName"))
                    .bind("faction", e.opt_str("Faction"))
                    .bind("status", code(s))
                    .bind("ended_at", e.time())
                    .bind("reward", e.opt_long("Reward"))
                    .bind("fine", e.opt_long("Fine"))
                    .bind("donation", donation(e))
                    .run();
        } else if (name == "MissionRedirected") {
            db.prepare(redirect_sql)
                    .bind("id", *id)
                    .bind("seq", seq)
                    .bind("name", e.opt_str("Name"))
                    .bind("title", e.opt_str("LocalisedName"))
                    .bind("destination_system", e.opt_str("NewDestinationSystem"))
                    .bind("destination_station", e.opt_str("NewDestinationStation"))
                    .run();
        }
    }

    std::vector<mission_log_projection::mission> mission_log_projection::active(const history &h) {
        // No known expiry sorts last: a null there means the acceptance was never recorded.
        return h.query([](sql::database &db) {
            auto s = db.prepare(std::string(columns) +
                                " WHERE status = 'accepted' ORDER BY expires_at IS NULL, expires_at");
            return read(s);
        });
    }

    std::vector<mission_log_projection::mission> mission_log_projection::recent(const history &h, int limit) {
        return h.query([&](sql::database &db) {
            auto s = db.prepare(std::string(columns) + " ORDER BY seq DESC LIMIT :limit");
            s.bind("limit", limit);
            return read(s);
        });
    }

    std::int64_t mission_log_projection::count(const history &h, status st) {
        return h.query([&](sql::database &db) {
            auto s = db.prepare("SELECT COUNT(*) FROM mission WHERE status = :status");
            s.bind("status", code(st));
            return s.step() ? s.integer(0) : 0;
        });
    }

    std::int64_t mission_log_projection::earned(const history &h) {
        return h.query([](sql::database &db) {
            auto s = db.prepare("SELECT COALESCE(SUM(reward), 0) FROM mission WHERE status = 'completed'");
            return s.step() ? s.integer(0) : 0;
        });
    }
} // namespace journal
